/*
 * claude_meter_v2  --  AIPI-Lite edition
 *
 * Hardware
 *   MCU    : ESP32-S3 (16 MB flash)
 *   Display: ST7735, 128x128, SPI
 *              CLK=16  MOSI=17  DC=7  CS=15  RST=18  BL=3
 *              invert_color=off
 *   LED    : WS2812 on GPIO46 (1 pixel)
 *   Button : GPIO42 active-low  (right button -> force poll)
 *   Power  : GPIO10 HIGH keeps rails up (power control pin per xiaozhi-esp32)
 *
 * Architecture
 *   - Device polls api.anthropic.com every POLL_INTERVAL_S seconds over Wi-Fi
 *   - OAuth bearer token stored in NVS; seeded from secrets.h on first boot
 *   - Config web server at http://claude-meter.local/ on port 80
 *     GET  /  -> stats page + token paste form
 *     POST /  -> save new token to NVS, trigger immediate re-poll
 *   - push_claude_token.py on ShadowTrooper pushes token every 4h via systemd timer
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_pm.h"

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/usb_serial_jtag.h"
#include "esp_adc/adc_oneshot.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7735.h"

#include "esp_http_client.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"

#include "mdns.h"
#include "esp_http_server.h"
#include "led_strip.h"
#include "lvgl.h"

#include "secrets.h"

/* ------------------------------------------------------------------ */
/* Hardware pins                                                        */
/* ------------------------------------------------------------------ */
#define PIN_LCD_CLK     16
#define PIN_LCD_MOSI    17
#define PIN_LCD_DC       7
#define PIN_LCD_CS      15
#define PIN_LCD_RST     18
#define PIN_LCD_BL       3
#define PIN_BTN_RIGHT   42
#define PIN_LED         46
#define PIN_PWR_HOLD    10

#define LCD_W           128
#define LCD_H           128
#define LCD_SPI_HOST    SPI2_HOST
#define LCD_PIXEL_CLK   27000000

/* ------------------------------------------------------------------ */
/* App config                                                           */
/* ------------------------------------------------------------------ */
#define POLL_INTERVAL_S     120
#define NVS_NAMESPACE       "cfg"
#define NVS_KEY_TOKEN       "token"
#define TOKEN_MAX           512
#define WIFI_RETRY_MAX      10
#define LVGL_BUF_ROWS       16

static const char *TAG = "claude_meter";

/* ------------------------------------------------------------------ */
/* Shared usage state                                                   */
/* ------------------------------------------------------------------ */
typedef struct {
    int  session_pct;
    long session_reset_epoch;
    int  weekly_pct;
    long weekly_reset_epoch;
    char status[24];
    bool ok;
} claude_usage_t;

static claude_usage_t    g_usage       = {0};
static SemaphoreHandle_t g_usage_mutex = NULL;
static volatile bool     g_force_poll  = false;
static volatile bool     g_usage_dirty = false;
static char              g_token[TOKEN_MAX] = {0};

/* ------------------------------------------------------------------ */
/* LED  (WS2812, 1 pixel)                                              */
/* ------------------------------------------------------------------ */
static led_strip_handle_t g_led = NULL;

static void led_init(void)
{
    led_strip_config_t cfg = {
        .strip_gpio_num = PIN_LED,
        .max_leds       = 1,
    };
    led_strip_rmt_config_t rmt = {
        .resolution_hz = 10 * 1000 * 1000,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&cfg, &rmt, &g_led));
    led_strip_clear(g_led);
}

static void led_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (!g_led) return;
    led_strip_set_pixel(g_led, 0, r, g, b);
    led_strip_refresh(g_led);
}

#define LED_BLUE()   led_set(0,   0,   1)
#define LED_GREEN()  led_set(0,   1,   0)
#define LED_AMBER()  led_set(1,   1,   0)
#define LED_RED()    led_set(1,   0,   0)
#define LED_OFF()    led_set(0,   0,   0)

static void led_update_from_pct(int a, int b)
{
    int w = (a > b) ? a : b;
    if      (w >= 85) LED_RED();
    else if (w >= 60) LED_AMBER();
    else              LED_GREEN();
}

/* ------------------------------------------------------------------ */
/* LVGL display driver                                                  */
/* ------------------------------------------------------------------ */
static lv_disp_draw_buf_t  g_disp_buf;
static lv_color_t          g_buf1[LCD_W * LVGL_BUF_ROWS];
static lv_color_t          g_buf2[LCD_W * LVGL_BUF_ROWS];
static lv_disp_drv_t       g_disp_drv;
static esp_lcd_panel_handle_t g_panel = NULL;

static void disp_flush_cb(lv_disp_drv_t *drv,
                           const lv_area_t *area,
                           lv_color_t *buf)
{
    esp_lcd_panel_draw_bitmap(g_panel,
        area->x1, area->y1, area->x2 + 1, area->y2 + 1, buf);
    lv_disp_flush_ready(drv);
}

static void lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(1);
}

/* ------------------------------------------------------------------ */
/* UI objects                                                           */
/* ------------------------------------------------------------------ */
static lv_obj_t *g_bar_session      = NULL;
static lv_obj_t *g_bar_weekly       = NULL;
static lv_obj_t *g_lbl_session_hdr  = NULL;
static lv_obj_t *g_lbl_session_rst  = NULL;
static lv_obj_t *g_lbl_weekly_hdr   = NULL;
static lv_obj_t *g_lbl_weekly_rst   = NULL;
static lv_obj_t *g_lbl_status       = NULL;
static lv_obj_t *g_lbl_battery      = NULL;
static adc_oneshot_unit_handle_t g_batt_adc = NULL;

static lv_color_t color_for_pct(int pct)
{
    // Empirically: LVGL red→blue, green→red, blue→green on this panel
    if (pct >= 85) return lv_color_make(  0, 255,   0);  // red on screen
    if (pct >= 60) return lv_color_make(  0, 200, 200);  // amber on screen
    return              lv_color_make(  0,   0, 255);  // green on screen
}

static void fmt_countdown(char *buf, size_t n, long epoch)
{
    time_t now  = time(NULL);
    long   diff = epoch - (long)now;
    if (diff <= 0) { snprintf(buf, n, "now"); return; }
    long h = diff / 3600;
    long m = (diff % 3600) / 60;
    long d = h / 24; h = h % 24;
    if (d > 0)      snprintf(buf, n, "resets %ldd %ldh", d, h);
    else if (h > 0) snprintf(buf, n, "resets %ldh %ldm", h, m);
    else            snprintf(buf, n, "resets %ldm", m);
}

static void ui_update(void)
{
    if (!g_bar_session) return;

    xSemaphoreTake(g_usage_mutex, portMAX_DELAY);
    claude_usage_t u = g_usage;
    xSemaphoreGive(g_usage_mutex);

    char tmp[32];

    /* session */
    snprintf(tmp, sizeof(tmp), "SESSION %d%%", u.session_pct);
    lv_label_set_text(g_lbl_session_hdr, tmp);
    lv_bar_set_value(g_bar_session, u.session_pct, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(g_bar_session, color_for_pct(u.session_pct),
                               LV_PART_INDICATOR);
    fmt_countdown(tmp, sizeof(tmp), u.session_reset_epoch);
    lv_label_set_text(g_lbl_session_rst, tmp);

    /* weekly */
    snprintf(tmp, sizeof(tmp), "WEEKLY %d%%", u.weekly_pct);
    lv_label_set_text(g_lbl_weekly_hdr, tmp);
    lv_bar_set_value(g_bar_weekly, u.weekly_pct, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(g_bar_weekly, color_for_pct(u.weekly_pct),
                               LV_PART_INDICATOR);
    fmt_countdown(tmp, sizeof(tmp), u.weekly_reset_epoch);
    lv_label_set_text(g_lbl_weekly_rst, tmp);

    /* status — only visible on error */
    if (!u.ok) {
        lv_label_set_text(g_lbl_status, "ERR");
        lv_obj_clear_flag(g_lbl_status, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_lbl_status, LV_OBJ_FLAG_HIDDEN);
    }

    g_usage_dirty = false;
}

static void ui_init(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* SESSION header + pct combined */
    g_lbl_session_hdr = lv_label_create(scr);
    lv_label_set_text(g_lbl_session_hdr, "SESSION 0%");
    lv_obj_set_style_text_color(g_lbl_session_hdr, lv_color_make(0, 255, 165), 0);
    lv_obj_set_style_text_font(g_lbl_session_hdr, &lv_font_unscii_8, 0);
    lv_obj_align(g_lbl_session_hdr, LV_ALIGN_TOP_LEFT, 4, 16);

    /* SESSION bar */
    g_bar_session = lv_bar_create(scr);
    lv_obj_set_size(g_bar_session, 120, 10);
    lv_bar_set_range(g_bar_session, 0, 100);
    lv_bar_set_value(g_bar_session, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(g_bar_session, lv_color_make(28, 28, 28), LV_PART_MAIN);
    lv_obj_set_style_radius(g_bar_session, 3, LV_PART_MAIN);
    lv_obj_set_style_border_color(g_bar_session, lv_color_make(60, 60, 60), LV_PART_MAIN);
    lv_obj_set_style_border_width(g_bar_session, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_bar_session, lv_color_make(40, 170, 40), LV_PART_INDICATOR);
    lv_obj_set_style_radius(g_bar_session, 3, LV_PART_INDICATOR);
    lv_obj_align(g_bar_session, LV_ALIGN_TOP_LEFT, 4, 28);

    /* SESSION reset */
    g_lbl_session_rst = lv_label_create(scr);
    lv_label_set_text(g_lbl_session_rst, "resets --");
    lv_obj_set_style_text_color(g_lbl_session_rst, lv_color_make(0, 255, 165), 0);
    lv_obj_set_style_text_font(g_lbl_session_rst, &lv_font_unscii_8, 0);
    lv_obj_align(g_lbl_session_rst, LV_ALIGN_TOP_LEFT, 4, 42);

    /* WEEKLY header + pct combined */
    g_lbl_weekly_hdr = lv_label_create(scr);
    lv_label_set_text(g_lbl_weekly_hdr, "WEEKLY 0%");
    lv_obj_set_style_text_color(g_lbl_weekly_hdr, lv_color_make(0, 255, 165), 0);
    lv_obj_set_style_text_font(g_lbl_weekly_hdr, &lv_font_unscii_8, 0);
    lv_obj_align(g_lbl_weekly_hdr, LV_ALIGN_TOP_LEFT, 4, 65);

    /* WEEKLY bar */
    g_bar_weekly = lv_bar_create(scr);
    lv_obj_set_size(g_bar_weekly, 120, 10);
    lv_bar_set_range(g_bar_weekly, 0, 100);
    lv_bar_set_value(g_bar_weekly, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(g_bar_weekly, lv_color_make(28, 28, 28), LV_PART_MAIN);
    lv_obj_set_style_radius(g_bar_weekly, 3, LV_PART_MAIN);
    lv_obj_set_style_border_color(g_bar_weekly, lv_color_make(60, 60, 60), LV_PART_MAIN);
    lv_obj_set_style_border_width(g_bar_weekly, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_bar_weekly, lv_color_make(40, 170, 40), LV_PART_INDICATOR);
    lv_obj_set_style_radius(g_bar_weekly, 3, LV_PART_INDICATOR);
    lv_obj_align(g_bar_weekly, LV_ALIGN_TOP_LEFT, 4, 77);

    /* WEEKLY reset */
    g_lbl_weekly_rst = lv_label_create(scr);
    lv_label_set_text(g_lbl_weekly_rst, "resets --");
    lv_obj_set_style_text_color(g_lbl_weekly_rst, lv_color_make(0, 255, 165), 0);
    lv_obj_set_style_text_font(g_lbl_weekly_rst, &lv_font_unscii_8, 0);
    lv_obj_align(g_lbl_weekly_rst, LV_ALIGN_TOP_LEFT, 4, 91);

    /* status */
    g_lbl_status = lv_label_create(scr);
    lv_label_set_text(g_lbl_status, "connecting...");
    lv_obj_set_style_text_color(g_lbl_status, lv_color_make(0, 255, 165), 0);
    lv_obj_set_style_text_font(g_lbl_status, &lv_font_unscii_8, 0);
    lv_obj_align(g_lbl_status, LV_ALIGN_BOTTOM_MID, 0, -4);

    /* battery */
    g_lbl_battery = lv_label_create(scr);
    lv_label_set_text(g_lbl_battery, "BATT --%");
    lv_obj_set_style_text_color(g_lbl_battery, lv_color_make(55, 75, 75), 0);
    lv_obj_set_style_text_font(g_lbl_battery, &lv_font_unscii_8, 0);
    lv_obj_align(g_lbl_battery, LV_ALIGN_BOTTOM_MID, 0, -10);
}

/* ------------------------------------------------------------------ */
/* Display init                                                         */
/* ------------------------------------------------------------------ */
static void display_init(void)
{
    /* SPI bus */
    spi_bus_config_t bus = {
        .mosi_io_num    = PIN_LCD_MOSI,
        .miso_io_num    = -1,
        .sclk_io_num    = PIN_LCD_CLK,
        .quadwp_io_num  = -1,
        .quadhd_io_num  = -1,
        .max_transfer_sz = LCD_W * LVGL_BUF_ROWS * sizeof(lv_color_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    /* Panel IO */
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num       = PIN_LCD_DC,
        .cs_gpio_num       = PIN_LCD_CS,
        .pclk_hz           = LCD_PIXEL_CLK,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
        .spi_mode          = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &io));

    /* ST7735 panel */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7735(io, &panel_cfg, &g_panel));

    esp_lcd_panel_reset(g_panel);
    esp_lcd_panel_init(g_panel);
    esp_lcd_panel_invert_color(g_panel, false);
    esp_lcd_panel_set_gap(g_panel, 0, 0);
    esp_lcd_panel_swap_xy(g_panel, true);
    esp_lcd_panel_mirror(g_panel, true, false);
    esp_lcd_panel_disp_on_off(g_panel, true);

    /* Clear GRAM before backlight on */
    uint16_t *clr = calloc(LCD_W * LCD_H, sizeof(uint16_t));
    if (clr) { esp_lcd_panel_draw_bitmap(g_panel, 0, 0, LCD_W, LCD_H, clr); free(clr); }

    /* Backlight — LEDC PWM, start dim, poll loop adjusts */
    ledc_timer_config_t bl_tmr = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = 5000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&bl_tmr));
    ledc_channel_config_t bl_ch = {
        .gpio_num   = PIN_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 51,   /* start at battery level */
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&bl_ch));

    /* Battery ADC init */
    adc_oneshot_unit_init_cfg_t adc_cfg = { .unit_id = ADC_UNIT_1 };
    adc_oneshot_new_unit(&adc_cfg, &g_batt_adc);
    adc_oneshot_chan_cfg_t adc_ch = {
        .atten    = ADC_ATTEN_DB_11,
        .bitwidth = ADC_BITWIDTH_12,
    };
    adc_oneshot_config_channel(g_batt_adc, ADC_CHANNEL_1, &adc_ch);

    /* LVGL init */
    lv_init();
    lv_disp_draw_buf_init(&g_disp_buf, g_buf1, g_buf2,
                           LCD_W * LVGL_BUF_ROWS);
    lv_disp_drv_init(&g_disp_drv);
    g_disp_drv.flush_cb = disp_flush_cb;
    g_disp_drv.draw_buf = &g_disp_buf;
    g_disp_drv.hor_res  = LCD_W;
    g_disp_drv.ver_res  = LCD_H;
    lv_disp_drv_register(&g_disp_drv);

    /* 1 ms tick timer */
    esp_timer_handle_t tick_tmr;
    const esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb,
        .name     = "lvgl_tick",
    };
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_tmr));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_tmr, 1000)); /* 1 ms */
}

/* ------------------------------------------------------------------ */
/* Wi-Fi                                                                */
/* ------------------------------------------------------------------ */
static EventGroupHandle_t g_wifi_eg;
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAILED_BIT     BIT1
static int g_wifi_retries = 0;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (g_wifi_retries < WIFI_RETRY_MAX) {
            esp_wifi_connect();
            g_wifi_retries++;
            ESP_LOGW(TAG, "Wi-Fi retry %d/%d", g_wifi_retries, WIFI_RETRY_MAX);
        } else {
            xEventGroupSetBits(g_wifi_eg, WIFI_FAILED_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        g_wifi_retries = 0;
        xEventGroupSetBits(g_wifi_eg, WIFI_CONNECTED_BIT);
    }
}

static bool wifi_init(void)
{
    g_wifi_eg = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t h_wifi, h_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, &h_wifi));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, &h_ip));

    wifi_config_t wcfg = {
        .sta = {
            .ssid     = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(g_wifi_eg,
        WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));

    return (bits & WIFI_CONNECTED_BIT) != 0;
}

/* ------------------------------------------------------------------ */
/* SNTP                                                                 */
/* ------------------------------------------------------------------ */
static void sntp_sync(void)
{
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    for (int i = 0; i < 20; i++) {
        if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
            ESP_LOGI(TAG, "SNTP synced");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGW(TAG, "SNTP timeout - reset countdowns will show 0");
}

/* ------------------------------------------------------------------ */
/* mDNS                                                                 */
/* ------------------------------------------------------------------ */
static void mdns_setup(void)
{
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set("claude-meter"));
    ESP_ERROR_CHECK(mdns_instance_name_set("Claude Meter"));
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS: claude-meter.local");
}

/* ------------------------------------------------------------------ */
/* Anthropic poll                                                        */
/* ------------------------------------------------------------------ */
typedef struct {
    int  session_pct;
    long session_reset;
    int  weekly_pct;
    long weekly_reset;
    char status[24];
    bool got_5h;
    bool got_7d;
} parse_ctx_t;

static esp_err_t http_evt_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_HEADER) return ESP_OK;
    parse_ctx_t *ctx = (parse_ctx_t *)evt->user_data;
    char *k = evt->header_key;
    char *v = evt->header_value;

    if (strcasecmp(k, "anthropic-ratelimit-unified-5h-utilization") == 0) {
        ctx->session_pct = (int)(atof(v) * 100.0f);
        ctx->got_5h = true;
    } else if (strcasecmp(k, "anthropic-ratelimit-unified-5h-reset") == 0) {
        ctx->session_reset = atol(v);
    } else if (strcasecmp(k, "anthropic-ratelimit-unified-7d-utilization") == 0) {
        ctx->weekly_pct = (int)(atof(v) * 100.0f);
        ctx->got_7d = true;
    } else if (strcasecmp(k, "anthropic-ratelimit-unified-7d-reset") == 0) {
        ctx->weekly_reset = atol(v);
    } else if (strcasecmp(k, "anthropic-ratelimit-unified-status") == 0) {
        strncpy(ctx->status, v, sizeof(ctx->status) - 1);
    }
    return ESP_OK;
}

static bool do_poll(void)
{
    char auth[TOKEN_MAX + 16];
    snprintf(auth, sizeof(auth), "Bearer %s", g_token);

    static const char body[] =
        "{\"model\":\"claude-haiku-4-5\","
        "\"max_tokens\":1,"
        "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";

    parse_ctx_t ctx = {0};
    strncpy(ctx.status, "allowed", sizeof(ctx.status));

    esp_http_client_config_t cfg = {
        .url               = "https://api.anthropic.com/v1/messages",
        .method            = HTTP_METHOD_POST,
        .event_handler     = http_evt_handler,
        .user_data         = &ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 20000,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    esp_http_client_set_header(client, "Content-Type",    "application/json");
    esp_http_client_set_header(client, "Authorization",   auth);
    esp_http_client_set_header(client, "anthropic-version", "2023-06-01");
    esp_http_client_set_header(client, "anthropic-beta",  "oauth-2025-04-20");
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err  = esp_http_client_perform(client);
    int       code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP error: %s", esp_err_to_name(err));
        return false;
    }
    if (code != 200 && code != 429) {
        ESP_LOGE(TAG, "HTTP %d", code);
        return false;
    }
    if (!ctx.got_5h || !ctx.got_7d) {
        ESP_LOGE(TAG, "Missing unified headers (HTTP %d) - token may be stale", code);
        return false;
    }

    xSemaphoreTake(g_usage_mutex, portMAX_DELAY);
    g_usage.session_pct         = ctx.session_pct;
    g_usage.session_reset_epoch = ctx.session_reset;
    g_usage.weekly_pct          = ctx.weekly_pct;
    g_usage.weekly_reset_epoch  = ctx.weekly_reset;
    strncpy(g_usage.status, ctx.status, sizeof(g_usage.status) - 1);
    g_usage.ok = true;
    xSemaphoreGive(g_usage_mutex);

    ESP_LOGI(TAG, "session=%d%% weekly=%d%% status=%s",
             ctx.session_pct, ctx.weekly_pct, ctx.status);

    led_update_from_pct(ctx.session_pct, ctx.weekly_pct);
    g_usage_dirty = true;
    return true;
}

/* ------------------------------------------------------------------ */
/* External-power detection — catches USB host + wall charger            */
/* ------------------------------------------------------------------ */
static int  g_prev_batt_raw    = 0;
static bool g_on_external_power = false;

static bool on_external_power(void)
{
    return g_on_external_power;
}

/* ------------------------------------------------------------------ */
/* Battery reading                                                       */
/* ------------------------------------------------------------------ */
static void batt_update(void)
{
    if (!g_batt_adc || !g_lbl_battery) return;

    int raw = 0;
    if (adc_oneshot_read(g_batt_adc, ADC_CHANNEL_1, &raw) != ESP_OK) return;

    /* Detect external power:
     *   - USB host: SOF packets present
     *   - Wall charger: voltage rising, or pinned at absolute max
     *     (a resting battery will drop >2 pts/cycle and exit) */
    bool usb_host = usb_serial_jtag_is_connected();
    bool charging = (g_prev_batt_raw > 0) && (raw > g_prev_batt_raw + 5);
    bool pinned_max = (raw >= 1975) && (raw >= g_prev_batt_raw - 2);
    g_on_external_power = usb_host || charging || pinned_max;
    g_prev_batt_raw = raw;

    /* Hide batt % when on external power */
    if (g_on_external_power) {
        lv_obj_add_flag(g_lbl_battery, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(g_lbl_battery, LV_OBJ_FLAG_HIDDEN);

    /* Lookup table from xiaozhi-esp32 AIPI-Lite: ADC → pct */
    static const int table[][2] = {
        {1480, 0}, {1581, 20}, {1663, 40}, {1750, 60}, {1840, 80}, {1980,100}
    };
    int pct = 0;
    for (int i = 0; i < 5; i++) {
        if (raw <= table[i+1][0]) {
            int d_raw = table[i+1][0] - table[i][0];
            int d_pct = table[i+1][1] - table[i][1];
            pct = table[i][1] + (raw - table[i][0]) * d_pct / d_raw;
            break;
        }
        pct = 100;
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "BATT %d%%", pct);
    lv_label_set_text(g_lbl_battery, buf);
}

/* ------------------------------------------------------------------ */
/* Poll task                                                            */
/* ------------------------------------------------------------------ */
static void poll_task(void *arg)
{
    (void)arg;
    /* small delay to let display paint first */
    vTaskDelay(pdMS_TO_TICKS(500));

    while (1) {
        batt_update();

        /* Adjust backlight based on external power */
        uint32_t bl_duty = on_external_power() ? 204 : 51;
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, bl_duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);

        LED_AMBER();
        bool ok = do_poll();
        if (!ok) {
            xSemaphoreTake(g_usage_mutex, portMAX_DELAY);
            g_usage.ok = false;
            xSemaphoreGive(g_usage_mutex);
            LED_RED();
            g_usage_dirty = true;
        }

        /* wait POLL_INTERVAL_S, waking early on g_force_poll */
        for (int i = 0; i < POLL_INTERVAL_S * 10; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
            if (g_force_poll) {
                g_force_poll = false;
                break;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* NVS token persistence                                                */
/* ------------------------------------------------------------------ */
static void nvs_load_token(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "NVS empty - seeding from secrets.h");
        strncpy(g_token, CLAUDE_TOKEN, TOKEN_MAX - 1);
        return;
    }
    size_t len = TOKEN_MAX;
    if (nvs_get_str(h, NVS_KEY_TOKEN, g_token, &len) == ESP_OK) {
        ESP_LOGI(TAG, "Loaded token from NVS");
    } else {
        ESP_LOGI(TAG, "No NVS token - seeding from secrets.h");
        strncpy(g_token, CLAUDE_TOKEN, TOKEN_MAX - 1);
    }
    nvs_close(h);
}

static void nvs_save_token(const char *tok)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed");
        return;
    }
    nvs_set_str(h, NVS_KEY_TOKEN, tok);
    nvs_commit(h);
    nvs_close(h);
    strncpy(g_token, tok, TOKEN_MAX - 1);
    ESP_LOGI(TAG, "Token saved to NVS (%u chars)", (unsigned)strlen(tok));
}

/* ------------------------------------------------------------------ */
/* URL decode                                                           */
/* ------------------------------------------------------------------ */
static void url_decode(char *dst, const char *src, size_t maxlen)
{
    size_t i = 0;
    while (*src && i < maxlen - 1) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = {src[1], src[2], '\0'};
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
        i++;
    }
    *dst = '\0';
}

/* ------------------------------------------------------------------ */
/* Config HTTP server                                                    */
/* ------------------------------------------------------------------ */
static esp_err_t cfg_get(httpd_req_t *req)
{
    xSemaphoreTake(g_usage_mutex, portMAX_DELAY);
    int sp = g_usage.session_pct;
    int wp = g_usage.weekly_pct;
    bool ok = g_usage.ok;
    xSemaphoreGive(g_usage_mutex);

    char page[2048];
    snprintf(page, sizeof(page),
        "<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Claude Meter</title>"
        "<style>"
        "body{font-family:sans-serif;max-width:480px;margin:2em auto;padding:0 1em;background:#111;color:#eee}"
        "h2{color:#c96}p{margin:.4em 0}"
        "label{display:block;margin-top:1em;color:#aaa}"
        "textarea{width:100%%;height:90px;font-size:11px;background:#222;color:#eee;border:1px solid #444}"
        "input[type=submit]{margin-top:.8em;padding:.4em 1.2em;background:#c96;border:none;cursor:pointer}"
        "</style></head><body>"
        "<h2>Claude Meter</h2>"
        "<p>Status: <b>%s</b></p>"
        "<p>SESSION: <b>%d%%</b> &nbsp; WEEKLY: <b>%d%%</b></p>"
        "<form method='POST'>"
        "<label>Paste OAuth access token (from ~/.claude/.credentials.json):</label>"
        "<textarea name='token' placeholder='eyJhbGci...'></textarea>"
        "<input type='submit' value='Save &amp; Poll'>"
        "</form></body></html>",
        ok ? "OK" : "ERROR", sp, wp);

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t cfg_post(httpd_req_t *req)
{
    char body[TOKEN_MAX + 32];
    int  len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty");
        return ESP_FAIL;
    }
    body[len] = '\0';

    char *val = strstr(body, "token=");
    if (!val) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No token field");
        return ESP_FAIL;
    }
    val += 6;

    char decoded[TOKEN_MAX];
    url_decode(decoded, val, sizeof(decoded));

    if (strlen(decoded) < 20) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Token too short");
        return ESP_FAIL;
    }

    nvs_save_token(decoded);
    g_force_poll = true;

    static const char *ok_page =
        "<!DOCTYPE html><html><body style='font-family:sans-serif;"
        "background:#111;color:#eee;padding:2em'>"
        "<p style='color:#6c6'>Token saved. Polling now...</p>"
        "<a href='/' style='color:#c96'>Back</a></body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, ok_page, HTTPD_RESP_USE_STRLEN);
}

static void config_server_start(void)
{
    httpd_config_t cfg  = HTTPD_DEFAULT_CONFIG();
    cfg.server_port     = 80;
    cfg.stack_size      = 8192;
    httpd_handle_t srv  = NULL;
    ESP_ERROR_CHECK(httpd_start(&srv, &cfg));

    static const httpd_uri_t get_uri  = {"/", HTTP_GET,  cfg_get,  NULL};
    static const httpd_uri_t post_uri = {"/", HTTP_POST, cfg_post, NULL};
    httpd_register_uri_handler(srv, &get_uri);
    httpd_register_uri_handler(srv, &post_uri);
    ESP_LOGI(TAG, "Config server on :80");
}

/* ------------------------------------------------------------------ */
/* Button  (GPIO42, active-low, force-poll on press)                    */
/* ------------------------------------------------------------------ */
static void IRAM_ATTR btn_isr_handler(void *arg)
{
    g_force_poll = true;
}

static void button_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_BTN_RIGHT),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&io);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(PIN_BTN_RIGHT, btn_isr_handler, NULL);
}

/* ------------------------------------------------------------------ */
/* app_main                                                             */
/* ------------------------------------------------------------------ */
void app_main(void)
{
    /* Power hold - keep rails alive on battery */
    gpio_reset_pin(PIN_PWR_HOLD);
    gpio_set_direction(PIN_PWR_HOLD, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_PWR_HOLD, 1);

    /* NVS init */
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);

    g_usage_mutex = xSemaphoreCreateMutex();

    /* Token (NVS or seed) */
    nvs_load_token();

    /* Power management: 40–160 MHz CPU frequency scaling */
    esp_pm_config_t pm_cfg = {
        .max_freq_mhz       = 160,
        .min_freq_mhz       = 40,
        .light_sleep_enable = false,
    };
    ESP_ERROR_CHECK(esp_pm_configure(&pm_cfg));

    /* LED: blue at boot */
    led_init();
    LED_BLUE();

    /* Display + LVGL + UI */
    display_init();
    ui_init();
    lv_task_handler();

    /* Wi-Fi */
    if (!wifi_init()) {
        ESP_LOGE(TAG, "Wi-Fi failed - offline mode");
        lv_label_set_text(g_lbl_status, "no wifi");
        LED_RED();
        /* spin LVGL only */
        while (1) {
            lv_task_handler();
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    /* SNTP */
    sntp_sync();

    /* mDNS */
    mdns_setup();

    /* Config web server */
    config_server_start();

    /* Button */
    button_init();

    /* Poll task */
    xTaskCreate(poll_task, "poll", 8192, NULL, 5, NULL);

    /* Main loop */
    while (1) {
        lv_task_handler();
        if (g_usage_dirty) {
            ui_update();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
