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
 *   - push_claude_token.py can push tokens via mDNS or explicit --url
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
#include "esp_ota_ops.h"
#include "esp_app_desc.h"

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
#include "audio.h"

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
#define POLL_INTERVAL_USB_S   120   /* snappier when externally powered */
#define POLL_INTERVAL_BATT_S  300   /* save radio energy on battery      */
#define BL_DUTY_USB           204   /* backlight ~80% on external power   */
#define BL_DUTY_BATT          38    /* backlight ~15% on battery          */
#define SCREEN_TIMEOUT_S      180   /* blank screen after idle (USB+batt) */
#define OTA_SELFTEST_S        15    /* run this long before confirming an OTA image */
#define NVS_NAMESPACE       "cfg"
#define NVS_KEY_TOKEN       "token"
#define TOKEN_MAX           512
#define WIFI_RETRY_MAX      10
#define LVGL_BUF_ROWS       16

/* Usage thresholds (percent) shared by LED, bar color, and alert tones */
#define USAGE_AMBER_PCT     60
#define USAGE_RED_PCT       85

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

static const char *TAG = "claude_meter";

/* ------------------------------------------------------------------ */
/* Shared usage state                                                   */
/* ------------------------------------------------------------------ */
/* Result of a poll attempt — lets the UI tell "fix your token" apart from
 * a transient network/API failure. */
typedef enum { POLL_OK = 0, POLL_AUTH, POLL_NET } poll_result_t;

typedef struct {
    int           session_pct;
    long          session_reset_epoch;
    int           weekly_pct;
    long          weekly_reset_epoch;
    char          status[24];
    bool          ok;
    poll_result_t err_kind;   /* valid when !ok */
} claude_usage_t;

static claude_usage_t    g_usage       = {0};
static SemaphoreHandle_t g_usage_mutex = NULL;   /* guards g_usage AND g_token */
static volatile bool     g_force_poll  = false;
static volatile bool     g_beep_button = false;
static volatile bool     g_usage_dirty = false;
static volatile bool     g_screen_wake = false;  /* request to wake screen */
static volatile int      g_batt_pct    = -1;     /* latest sampled %, rendered by main loop */
static volatile bool     g_on_external_power = false; /* USB host or wall charger */
static char              g_token[TOKEN_MAX] = {0};

/* ------------------------------------------------------------------ */
/* LED  (WS2812, 1 pixel)                                              */
/* ------------------------------------------------------------------ */
static led_strip_handle_t g_led = NULL;
static volatile uint8_t g_led_pending_r = 0, g_led_pending_g = 0, g_led_pending_b = 1;
static volatile bool g_led_dirty = false;

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

static void led_set_pending(uint8_t r, uint8_t g, uint8_t b)
{
    g_led_pending_r = r;
    g_led_pending_g = g;
    g_led_pending_b = b;
    g_led_dirty = true;
}

static void led_update_from_pct(int a, int b)
{
    int w = (a > b) ? a : b;
    if      (w >= USAGE_RED_PCT)   led_set_pending(1, 0, 0);
    else if (w >= USAGE_AMBER_PCT) led_set_pending(1, 1, 0);
    else                           led_set_pending(0, 1, 0);
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
    if (pct >= USAGE_RED_PCT)   return lv_color_make(  0, 255,   0);  // red on screen
    if (pct >= USAGE_AMBER_PCT) return lv_color_make(  0, 200, 200);  // amber on screen
    return              lv_color_make(  0,   0, 255);  // green on screen
}

static void fmt_countdown(char *buf, size_t n, long epoch)
{
    time_t now  = time(NULL);
    if (now < 1600000000 || epoch == 0) { snprintf(buf, n, "resets --"); return; }
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

    g_usage_dirty = false;

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

    /* status — only visible on error; distinguish a rejected token from a
     * transient network/API failure so the user knows to push a fresh token */
    if (!u.ok) {
        lv_label_set_text(g_lbl_status,
                          u.err_kind == POLL_AUTH ? "TOKEN?" : "ERR");
        lv_obj_clear_flag(g_lbl_status, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_lbl_status, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Render the battery label. LVGL 8 is single-threaded, so all label access
 * must come from the main loop — batt_update() (poll task) only samples the
 * percentage into g_batt_pct; this turns it into pixels. Visibility tracks
 * the power source; gated so it only touches LVGL on an actual change. */
static void ui_render_battery(void)
{
    static int last_ext = -1;
    static int last_pct = -2;
    if (!g_lbl_battery) return;

    int ext = g_on_external_power ? 1 : 0;
    if (ext != last_ext) {
        last_ext = ext;
        if (ext) lv_obj_add_flag(g_lbl_battery, LV_OBJ_FLAG_HIDDEN);
        else     lv_obj_clear_flag(g_lbl_battery, LV_OBJ_FLAG_HIDDEN);
    }

    int pct = g_batt_pct;
    if (!ext && pct != last_pct) {
        last_pct = pct;
        char buf[16];
        snprintf(buf, sizeof(buf), "BATT %d%%", pct);
        lv_label_set_text(g_lbl_battery, buf);
    }
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
    lv_obj_set_style_pad_all(g_bar_session, 1, LV_PART_MAIN);   /* inset indicator inside border */
    lv_obj_set_style_bg_color(g_bar_session, lv_color_make(40, 170, 40), LV_PART_INDICATOR);
    lv_obj_set_style_radius(g_bar_session, 2, LV_PART_INDICATOR);
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
    lv_obj_set_style_pad_all(g_bar_weekly, 1, LV_PART_MAIN);   /* inset indicator inside border */
    lv_obj_set_style_bg_color(g_bar_weekly, lv_color_make(40, 170, 40), LV_PART_INDICATOR);
    lv_obj_set_style_radius(g_bar_weekly, 2, LV_PART_INDICATOR);
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
        .duty       = BL_DUTY_BATT,   /* start dim; power_eval adjusts */
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&bl_ch));

    /* Battery ADC init */
    adc_oneshot_unit_init_cfg_t adc_cfg = { .unit_id = ADC_UNIT_1 };
    adc_oneshot_new_unit(&adc_cfg, &g_batt_adc);
    adc_oneshot_chan_cfg_t adc_ch = {
        .atten    = ADC_ATTEN_DB_12,
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
static bool g_wifi_ever_connected = false;

#define NOTIFY_HOST_URL "http://shadowtrooper.local:5555/notify"

static void notify_host_online_task(void *arg)
{
    (void)arg;
    esp_http_client_config_t cfg = {
        .url = NOTIFY_HOST_URL,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGW(TAG, "Failed to init HTTP client for host notify");
        vTaskDelete(NULL);
        return;
    }
    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Host notify failed: %s", esp_err_to_name(err));
    } else {
        int code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "Host notified: HTTP %d", code);
    }
    esp_http_client_cleanup(client);
    vTaskDelete(NULL);
}

static void notify_host_online(void)
{
    xTaskCreate(notify_host_online_task, "notify_host", 4096, NULL, 2, NULL);
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (g_wifi_ever_connected || g_wifi_retries < WIFI_RETRY_MAX) {
            esp_wifi_connect();
            g_wifi_retries++;
            ESP_LOGW(TAG, "Wi-Fi retry %d%s", g_wifi_retries,
                     g_wifi_ever_connected ? " (reconnecting)" : "");
        } else {
            xEventGroupSetBits(g_wifi_eg, WIFI_FAILED_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        g_wifi_retries = 0;
        g_wifi_ever_connected = true;
        xEventGroupSetBits(g_wifi_eg, WIFI_CONNECTED_BIT);
        notify_host_online();
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
        ctx->session_pct = (int)(atof(v) * 100.0 + 0.5);
        ctx->got_5h = true;
    } else if (strcasecmp(k, "anthropic-ratelimit-unified-5h-reset") == 0) {
        ctx->session_reset = atol(v);
    } else if (strcasecmp(k, "anthropic-ratelimit-unified-7d-utilization") == 0) {
        ctx->weekly_pct = (int)(atof(v) * 100.0 + 0.5);
        ctx->got_7d = true;
    } else if (strcasecmp(k, "anthropic-ratelimit-unified-7d-reset") == 0) {
        ctx->weekly_reset = atol(v);
    } else if (strcasecmp(k, "anthropic-ratelimit-unified-status") == 0) {
        strncpy(ctx->status, v, sizeof(ctx->status) - 1);
    }
    return ESP_OK;
}

static poll_result_t do_poll(void)
{
    /* Snapshot the token under the mutex — it can be rewritten concurrently
     * by the config server's POST handler (nvs_save_token). */
    char token[TOKEN_MAX];
    xSemaphoreTake(g_usage_mutex, portMAX_DELAY);
    strncpy(token, g_token, sizeof(token) - 1);
    token[sizeof(token) - 1] = '\0';
    xSemaphoreGive(g_usage_mutex);

    char auth[TOKEN_MAX + 16];
    snprintf(auth, sizeof(auth), "Bearer %s", token);

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
    if (!client) return POLL_NET;

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
        return POLL_NET;
    }
    if (code == 401 || code == 403) {
        ESP_LOGE(TAG, "HTTP %d - token rejected", code);
        return POLL_AUTH;
    }
    if (code != 200 && code != 429) {
        ESP_LOGE(TAG, "HTTP %d", code);
        return POLL_NET;
    }
    if (!ctx.got_5h || !ctx.got_7d) {
        ESP_LOGE(TAG, "Missing unified headers (HTTP %d) - token may be stale", code);
        return POLL_AUTH;
    }

    xSemaphoreTake(g_usage_mutex, portMAX_DELAY);
    g_usage.session_pct         = ctx.session_pct;
    g_usage.session_reset_epoch = ctx.session_reset;
    g_usage.weekly_pct          = ctx.weekly_pct;
    g_usage.weekly_reset_epoch  = ctx.weekly_reset;
    strncpy(g_usage.status, ctx.status, sizeof(g_usage.status) - 1);
    g_usage.ok       = true;
    g_usage.err_kind = POLL_OK;
    xSemaphoreGive(g_usage_mutex);

    ESP_LOGI(TAG, "session=%d%% weekly=%d%% status=%s",
             ctx.session_pct, ctx.weekly_pct, ctx.status);

    led_update_from_pct(ctx.session_pct, ctx.weekly_pct);
    g_usage_dirty = true;
    return POLL_OK;
}

/* ------------------------------------------------------------------ */
/* External-power detection — catches USB host + wall charger            */
/* ------------------------------------------------------------------ */
static int  g_prev_batt_raw    = 0;
static int  g_batt_avg         = 0;     /* exponential moving average */
static bool g_charger_trend    = false; /* wall-charger detected via voltage */
/* g_on_external_power lives in the globals section (read by the main loop) */

/* Apply power-dependent settings for the current power state.
 *
 * The USB-host signal (SOF packets) is fast and reliable, so it is sampled
 * here and combined with the slow voltage-trend wall-charger heuristic that
 * batt_update() maintains. This runs both per-poll and every few seconds in
 * the wait loop so plugging/unplugging USB is reflected within ~2s instead
 * of waiting for the next poll.
 *
 * On battery we trade snappiness for runtime: dimmer backlight, deeper
 * Wi-Fi modem sleep, and a longer poll interval (see the poll wait loop).
 * Backlight level + screen on/off are owned by the main loop's screen
 * manager (which reads g_on_external_power), to keep all panel/LEDC access
 * in one task. */
static void power_eval(void)
{
    static int applied = -1;   /* last applied state; -1 forces first apply */
    bool ext = usb_serial_jtag_is_connected() || g_charger_trend;
    g_on_external_power = ext;
    if ((int)ext == applied) return;   /* only act on transitions */
    applied = ext;

    /* Snappy on USB, deeper modem sleep on battery.
     * The battery-label visibility is handled by ui_render_battery() on the
     * main loop (LVGL is single-threaded; this runs on the poll task). */
    esp_wifi_set_ps(ext ? WIFI_PS_MIN_MODEM : WIFI_PS_MAX_MODEM);
}

/* ------------------------------------------------------------------ */
/* Screen manager — backlight + panel on/off, all from the main loop    */
/* ------------------------------------------------------------------ */
static volatile bool g_screen_on  = true;   /* read by the button ISR */
static int64_t g_last_activity_us = 0;

/* Set backlight duty for the current screen + power state. Gated so it
 * only touches the LEDC on an actual change. */
static void apply_backlight(void)
{
    static int last = -1;
    int duty = !g_screen_on ? 0
             : (g_on_external_power ? BL_DUTY_USB : BL_DUTY_BATT);
    if (duty == last) return;
    last = duty;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void screen_set(bool on)
{
    if (on == g_screen_on) return;
    g_screen_on = on;
    ESP_LOGI(TAG, "screen %s", on ? "on" : "off");
    if (on) {
        esp_lcd_panel_disp_on_off(g_panel, true);
        apply_backlight();                       /* light up */
    } else {
        apply_backlight();                       /* backlight off first */
        esp_lcd_panel_disp_on_off(g_panel, false);
    }
}

/* Run each main-loop iteration: honor wake requests and enforce the idle
 * timeout. Applies on both USB and battery; only the on-brightness differs
 * (see apply_backlight). */
static void screen_tick(void)
{
    int64_t now = esp_timer_get_time();

    if (g_screen_wake) {
        g_screen_wake = false;
        g_last_activity_us = now;
        screen_set(true);
    }

    if (g_screen_on &&
        now - g_last_activity_us > (int64_t)SCREEN_TIMEOUT_S * 1000000) {
        screen_set(false);
    }

    apply_backlight();      /* pick up brightness changes while on */
}

/* ------------------------------------------------------------------ */
/* Battery reading                                                       */
/* ------------------------------------------------------------------ */
static void batt_update(void)
{
    if (!g_batt_adc) return;

    int raw = 0;
    if (adc_oneshot_read(g_batt_adc, ADC_CHANNEL_1, &raw) != ESP_OK) return;

    /* Exponential moving average — smooths low-battery voltage noise
     * that could otherwise look like a "rising" charge curve. */
    if (g_batt_avg == 0) {
        g_batt_avg = raw;
    } else {
        g_batt_avg = (g_batt_avg * 3 + raw) / 4;
    }

    /* Wall-charger detection from the voltage trend (the fast USB-host
     * signal is handled in power_eval): averaged voltage rising, or pinned
     * at absolute max (a resting battery drops >2 pts/cycle and exits). */
    bool charging   = (g_prev_batt_raw > 0) && (g_batt_avg > g_prev_batt_raw + 8);
    bool pinned_max = (g_batt_avg >= 1975) && (g_batt_avg >= g_prev_batt_raw - 2);
    g_charger_trend = charging || pinned_max;
    g_prev_batt_raw = g_batt_avg;

    /* Lookup table from xiaozhi-esp32 AIPI-Lite: ADC → pct (label shown
     * only on battery — power_eval toggles its visibility) */
    static const int table[][2] = {
        {1480, 0}, {1581, 20}, {1663, 40}, {1750, 60}, {1840, 80}, {1980,100}
    };
    int pct = 0;
    for (int i = 0; i < 5; i++) {
        if (g_batt_avg <= table[i+1][0]) {
            int d_raw = table[i+1][0] - table[i][0];
            int d_pct = table[i+1][1] - table[i][1];
            pct = table[i][1] + (g_batt_avg - table[i][0]) * d_pct / d_raw;
            break;
        }
        pct = 100;
    }
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    /* Hand the value to the main loop; ui_render_battery() draws it. */
    g_batt_pct = pct;
}

/* ------------------------------------------------------------------ */
/* Poll task                                                            */
/* ------------------------------------------------------------------ */
static void poll_task(void *arg)
{
    (void)arg;
    /* small delay to let display paint first */
    vTaskDelay(pdMS_TO_TICKS(500));

    /* threshold-crossing tracking */
    int prev_session = 0, prev_weekly = 0;
    poll_result_t prev_res = POLL_OK;

    while (1) {
        batt_update();
        power_eval();   /* backlight + batt-label visibility */

        /* button-tap feedback */
        if (g_beep_button) {
            g_beep_button = false;
            audio_play_melody(MELODY_BUTTON);
        }

        led_set_pending(1, 1, 0);
        poll_result_t res = do_poll();
        if (res != POLL_OK) {
            xSemaphoreTake(g_usage_mutex, portMAX_DELAY);
            g_usage.ok       = false;
            g_usage.err_kind = res;
            xSemaphoreGive(g_usage_mutex);
            led_set_pending(1, 0, 0);
            g_usage_dirty = true;
            /* Only wake/beep/notify on the transition into (or change of)
             * error state — an error that persists for hours on battery
             * must not re-wake the screen and beep every poll. */
            if (res != prev_res) {
                g_screen_wake = true;   /* show the error */
                if (res == POLL_AUTH) {
                    notify_host_online();
                }
                audio_play_melody(MELODY_ERROR);
            }
        } else {
            if (prev_res != POLL_OK) {
                g_screen_wake = true;   /* recovered - let the user see it */
            }
            /* threshold-crossing alerts */
            xSemaphoreTake(g_usage_mutex, portMAX_DELAY);
            int sp = g_usage.session_pct;
            int wp = g_usage.weekly_pct;
            xSemaphoreGive(g_usage_mutex);

            int worse = (sp > wp) ? sp : wp;
            int prev  = (prev_session > prev_weekly) ? prev_session : prev_weekly;

            if (worse >= USAGE_RED_PCT && prev < USAGE_RED_PCT) {
                audio_play_melody(MELODY_THRESHOLD_85);
            } else if (worse >= USAGE_AMBER_PCT && prev < USAGE_AMBER_PCT) {
                audio_play_melody(MELODY_THRESHOLD_60);
            }

            /* wake the screen on any change in usage */
            if (sp != prev_session || wp != prev_weekly) {
                g_screen_wake = true;
            }

            prev_session = sp;
            prev_weekly  = wp;
        }
        prev_res = res;

        /* Wait the poll interval (longer on battery), waking early on
         * g_force_poll. Re-check power state every ~2s so USB plug/unplug
         * is reflected quickly, and so the interval adapts mid-wait. */
        int waited = 0;
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(100));
            waited++;
            if (g_force_poll) {
                g_force_poll = false;
                break;
            }
            if (waited % 20 == 0) power_eval();
            int limit = (g_on_external_power ? POLL_INTERVAL_USB_S
                                             : POLL_INTERVAL_BATT_S) * 10;
            if (waited >= limit) break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* NVS token persistence                                                */
/* ------------------------------------------------------------------ */
static void nvs_save_token(const char *tok);

static void nvs_load_token(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "NVS empty - seeding from secrets.h");
        strncpy(g_token, CLAUDE_TOKEN, TOKEN_MAX - 1);
        nvs_save_token(g_token);
        return;
    }
    size_t len = TOKEN_MAX;
    if (nvs_get_str(h, NVS_KEY_TOKEN, g_token, &len) == ESP_OK) {
        ESP_LOGI(TAG, "Loaded token from NVS");
        nvs_close(h);
    } else {
        ESP_LOGI(TAG, "No NVS token - seeding from secrets.h");
        strncpy(g_token, CLAUDE_TOKEN, TOKEN_MAX - 1);
        nvs_close(h);
        nvs_save_token(g_token);
    }
}

static void nvs_save_token(const char *tok)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed");
        return;
    }
    esp_err_t e1 = nvs_set_str(h, NVS_KEY_TOKEN, tok);
    esp_err_t e2 = nvs_commit(h);
    if (e1 != ESP_OK || e2 != ESP_OK)
        ESP_LOGE(TAG, "NVS write failed: set=%s commit=%s",
                 esp_err_to_name(e1), esp_err_to_name(e2));
    nvs_close(h);
    /* g_token is read by the poll task; guard the swap. */
    xSemaphoreTake(g_usage_mutex, portMAX_DELAY);
    strncpy(g_token, tok, TOKEN_MAX - 1);
    g_token[TOKEN_MAX - 1] = '\0';
    xSemaphoreGive(g_usage_mutex);
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

/* Optional shared-secret auth on mutating endpoints (POST / and /ota).
 * Define CFG_AUTH_SECRET in secrets.h to require an "X-Auth: <secret>"
 * header. Left undefined, the endpoints are open (legacy behavior). */
static bool auth_ok(httpd_req_t *req)
{
#ifdef CFG_AUTH_SECRET
    char hdr[96];
    if (httpd_req_get_hdr_value_str(req, "X-Auth", hdr, sizeof(hdr)) != ESP_OK)
        return false;
    return strcmp(hdr, CFG_AUTH_SECRET) == 0;
#else
    (void)req;
    return true;
#endif
}

/* Locate the value of an x-www-form-urlencoded field by exact name.
 * Returns a pointer into `body` at the start of the (still-encoded) value,
 * and writes its length (up to the next '&' or end) to *out_len. */
static const char *form_field(const char *body, const char *name, size_t *out_len)
{
    size_t nl = strlen(name);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, name, nl) == 0 && p[nl] == '=') {
            const char *v   = p + nl + 1;
            const char *amp = strchr(v, '&');
            *out_len = amp ? (size_t)(amp - v) : strlen(v);
            return v;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return NULL;
}

/* Short, human-readable OTA state of the running slot (shown on the stats
 * page so the rollback handshake is verifiable over HTTP). */
static const char *ota_state_str(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) != ESP_OK) return "?";
    switch (st) {
        case ESP_OTA_IMG_NEW:            return "new";
        case ESP_OTA_IMG_PENDING_VERIFY: return "pending";
        case ESP_OTA_IMG_VALID:          return "valid";
        case ESP_OTA_IMG_INVALID:        return "invalid";
        case ESP_OTA_IMG_ABORTED:        return "aborted";
        default:                         return "ok";   /* UNDEFINED: e.g. USB-flashed */
    }
}

/* Confirm the running image so the bootloader won't roll it back. Only acts
 * while the slot is PENDING_VERIFY (an OTA image on its trial boot); a no-op
 * for normal/USB-flashed images. Called once the device has run long enough
 * to prove the image isn't crash-looping (see the main loop). */
static void ota_confirm_image(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) != ESP_OK) return;
    if (st != ESP_OTA_IMG_PENDING_VERIFY) return;

    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
        ESP_LOGW(TAG, "OTA image on %s confirmed valid — rollback cancelled", run->label);
    else
        ESP_LOGE(TAG, "esp_ota_mark_app_valid_cancel_rollback failed");
}

static esp_err_t cfg_get(httpd_req_t *req)
{
    xSemaphoreTake(g_usage_mutex, portMAX_DELAY);
    int sp = g_usage.session_pct;
    int wp = g_usage.weekly_pct;
    bool ok = g_usage.ok;
    xSemaphoreGive(g_usage_mutex);

    const esp_app_desc_t  *app = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();

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
        "<p style='color:#888;font-size:12px'>fw %s &middot; %s &middot; %s</p>"
        "<form method='POST'>"
        "<label>Paste OAuth access token (from ~/.claude/.credentials.json):</label>"
        "<textarea name='token' placeholder='sk-ant-oat01-...'></textarea>"
        "<input type='submit' value='Save &amp; Poll'>"
        "</form></body></html>",
        ok ? "OK" : "ERROR", sp, wp,
        app ? app->version : "?", run ? run->label : "?", ota_state_str());

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t cfg_post(httpd_req_t *req)
{
    if (!auth_ok(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Auth required");
        return ESP_FAIL;
    }

    /* Read the whole body — httpd_req_recv may return short counts, so loop
     * until content_len (or the buffer) is satisfied. */
    char body[TOKEN_MAX + 64];
    int  total = 0;
    int  timeouts = 0;
    while (total < (int)sizeof(body) - 1) {
        int r = httpd_req_recv(req, body + total, sizeof(body) - 1 - total);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > 5) {
                httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "recv timeout");
                return ESP_FAIL;
            }
            continue;
        }
        timeouts = 0;
        if (r < 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv error");
            return ESP_FAIL;
        }
        if (r == 0) break;
        total += r;
        if (req->content_len && total >= (int)req->content_len) break;
    }
    if (total <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty");
        return ESP_FAIL;
    }
    body[total] = '\0';

    size_t vlen = 0;
    const char *val = form_field(body, "token", &vlen);
    if (!val) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No token field");
        return ESP_FAIL;
    }

    /* Copy just this field's value, then URL-decode it. */
    char enc[TOKEN_MAX];
    if (vlen >= sizeof(enc)) vlen = sizeof(enc) - 1;
    memcpy(enc, val, vlen);
    enc[vlen] = '\0';

    char decoded[TOKEN_MAX];
    url_decode(decoded, enc, sizeof(decoded));

    if (strlen(decoded) < 20) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Token too short");
        return ESP_FAIL;
    }

    nvs_save_token(decoded);
    g_force_poll = true;
    audio_play_melody(MELODY_TOKEN_SAVED);

    static const char *ok_page =
        "<!DOCTYPE html><html><body style='font-family:sans-serif;"
        "background:#111;color:#eee;padding:2em'>"
        "<p style='color:#6c6'>Token saved. Polling now...</p>"
        "<a href='/' style='color:#c96'>Back</a></body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, ok_page, HTTPD_RESP_USE_STRLEN);
}

/* OTA firmware upload: stream the raw .bin body straight into the inactive
 * OTA slot, then flip the boot partition and reboot. No multipart framing —
 * push with e.g.  curl --data-binary @build/claude_meter_v2.bin \
 *                      -H 'X-Auth: <secret>' http://claude-meter.local/ota */
static esp_err_t ota_post(httpd_req_t *req)
{
    if (!auth_ok(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Auth required");
        return ESP_FAIL;
    }

    if (req->content_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content-Length required");
        return ESP_FAIL;
    }

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OTA: receiving %d bytes -> %s",
             req->content_len, part->label);

    esp_ota_handle_t ota = 0;
    if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_begin failed");
        return ESP_FAIL;
    }

    static char buf[1024];   /* httpd handlers are serialized -> static is safe */
    int remaining = req->content_len;
    int timeouts  = 0;
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf, MIN(remaining, (int)sizeof(buf)));
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > 5) {
                esp_ota_abort(ota);
                httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "recv timeout");
                return ESP_FAIL;
            }
            continue;
        }
        timeouts = 0;
        if (r <= 0) {
            esp_ota_abort(ota);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv error");
            return ESP_FAIL;
        }
        if (esp_ota_write(ota, buf, r) != ESP_OK) {
            esp_ota_abort(ota);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "flash write failed");
            return ESP_FAIL;
        }
        remaining -= r;
    }

    if (esp_ota_end(ota) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "image invalid");
        return ESP_FAIL;
    }
    if (esp_ota_set_boot_partition(part) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set_boot failed");
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "OTA OK — rebooting\n");
    ESP_LOGW(TAG, "OTA complete, booting %s", part->label);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static void config_server_start(void)
{
    httpd_config_t cfg  = HTTPD_DEFAULT_CONFIG();
    cfg.server_port     = 80;
    cfg.stack_size      = 10240;
    cfg.lru_purge_enable = true;
    httpd_handle_t srv  = NULL;
    ESP_ERROR_CHECK(httpd_start(&srv, &cfg));

    static const httpd_uri_t get_uri  = {"/",    HTTP_GET,  cfg_get,  NULL};
    static const httpd_uri_t post_uri = {"/",    HTTP_POST, cfg_post, NULL};
    static const httpd_uri_t ota_uri  = {"/ota", HTTP_POST, ota_post, NULL};
    httpd_register_uri_handler(srv, &get_uri);
    httpd_register_uri_handler(srv, &post_uri);
    httpd_register_uri_handler(srv, &ota_uri);
    ESP_LOGI(TAG, "Config server on :80 (GET / POST / /ota)");
}

/* ------------------------------------------------------------------ */
/* Button  (GPIO42, active-low, force-poll on press)                    */
/* ------------------------------------------------------------------ */
static void IRAM_ATTR btn_isr_handler(void *arg)
{
    static int64_t last_press_us = 0;
    int64_t now = esp_timer_get_time();
    if (now - last_press_us < 200000) return;
    last_press_us = now;

    if (!g_screen_on) {
        g_screen_wake = true;
        return;
    }
    g_force_poll  = true;
    g_beep_button = true;
    g_screen_wake = true;   /* also resets the idle timer */
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

    /* Audio (ES8311 + I2S) — non-fatal if it fails */
    if (audio_init() != ESP_OK) {
        ESP_LOGW(TAG, "audio init failed — continuing without sound");
    }

    /* Wi-Fi */
    if (!wifi_init()) {
        ESP_LOGE(TAG, "Wi-Fi failed - offline mode");
        lv_label_set_text(g_lbl_status, "no wifi");
        LED_RED();
        /* Deliberately do NOT confirm the OTA image here: if a bad update
         * breaks Wi-Fi, leaving the slot PENDING_VERIFY means the next reset
         * auto-rolls-back to the last working image. An image is only trusted
         * once it reaches the online main loop. */
        /* Spin LVGL, periodically retrying the connection so a router that
         * comes back late (e.g. 30s after a power cut) is still caught —
         * throttled to every ~5s so the radio isn't hammered in a tight
         * loop. On success, restart cleanly through the normal path rather
         * than trying to bring up SNTP/mDNS/server/poll-task from here. */
        int wifi_retry_ticks = 0;
        while (1) {
            lv_task_handler();
            if (xEventGroupGetBits(g_wifi_eg) & WIFI_CONNECTED_BIT) {
                ESP_LOGW(TAG, "Wi-Fi came up late - restarting");
                esp_restart();
            }
            if (++wifi_retry_ticks >= 500) {   /* ~5s at the 10ms tick below */
                wifi_retry_ticks = 0;
                esp_wifi_connect();
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    /* SNTP */
    sntp_sync();

    /* Boot melody — device is alive and connected */
    audio_play_melody(MELODY_BOOT);

    /* mDNS */
    mdns_setup();

    /* Config web server */
    config_server_start();

    /* Button */
    button_init();

    /* Poll task */
    xTaskCreate(poll_task, "poll", 8192, NULL, 5, NULL);

    /* Main loop */
    g_last_activity_us  = esp_timer_get_time();   /* keep screen on at boot */
    int64_t loop_start   = esp_timer_get_time();
    bool    ota_checked  = false;
    int64_t last_minute_us = loop_start;
    while (1) {
        lv_task_handler();
        if (g_led_dirty) {
            g_led_dirty = false;
            led_set(g_led_pending_r, g_led_pending_g, g_led_pending_b);
        }

        /* Recompute countdown labels once a minute even without a poll, so
         * "resets 5m" doesn't sit stale for up to POLL_INTERVAL_BATT_S.
         * Skip while the screen is off - nothing to redraw. */
        int64_t now_us = esp_timer_get_time();
        if (g_screen_on && now_us - last_minute_us > 60 * 1000000LL) {
            last_minute_us = now_us;
            g_usage_dirty = true;
        }

        if (g_usage_dirty) {
            ui_update();
        }
        ui_render_battery();
        screen_tick();

        /* Auto-rollback handshake: once we've reached the main loop and run
         * stably for OTA_SELFTEST_S, the image clearly boots — confirm it so
         * the bootloader keeps it. A crash/hang before this point leaves the
         * slot PENDING_VERIFY, and the next reset rolls back automatically. */
        if (!ota_checked &&
            esp_timer_get_time() - loop_start > (int64_t)OTA_SELFTEST_S * 1000000) {
            ota_confirm_image();
            ota_checked = true;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
