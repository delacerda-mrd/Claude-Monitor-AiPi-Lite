/*
 * claude_meter v2  --  AIPI-Lite edition
 *
 * A desk gadget that shows Claude session (5h) and weekly (7d) usage on a
 * 1.44" 128x128 ST7735, with an RGB status LED, battery, speaker and one
 * button. Behavior + internals: docs/ARCHITECTURE.md.
 *
 * This file: boot order, display + LVGL driver, the main loop (the only task
 * that touches LVGL / panel / backlight / LED), button gestures, the OTA
 * rollback handshake and the screenshot hook.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_st7735.h"
#include "lvgl.h"

#include "app.h"
#include "audio.h"
#include "board.h"
#include "led.h"
#include "net.h"
#include "power.h"
#include "settings.h"
#include "ui.h"
#include "usage.h"
#include "voice.h"

static const char *TAG = "main";

#define LCD_SPI_HOST    SPI2_HOST
#define LCD_PIXEL_CLK   27000000
#define LVGL_BUF_ROWS   32
#define OTA_SELFTEST_S  15          /* stable online this long -> confirm image */
#define LONG_PRESS_MS   600
#define FADE_IN_MS      450

sys_state_t g_sys = { .batt_pct = -1 };
volatile int g_btn_sim;

/* ------------------------------------------------------------------ */
/* Sound policy                                                        */
/* ------------------------------------------------------------------ */
void app_sound(melody_type_t m)
{
    if (settings_quiet_now() || m == MELODY_NONE) return;
    audio_play_async(m);
}

static void say(const char *script, melody_type_t fallback)
{
    settings_t c;
    settings_get(&c);
    if (c.talk && audio_say_async(script)) return;
    if (fallback != MELODY_NONE) audio_play_async(fallback);
}

void app_say(const char *script, melody_type_t fallback)
{
    if (!settings_quiet_now()) say(script, fallback);
}

void app_say_now(const char *script, melody_type_t fallback)
{
    if (!settings_muted()) say(script, fallback);
}

void app_sound_now(melody_type_t m)
{
    if (!settings_muted() && m != MELODY_NONE) audio_play_async(m);
}

/* ------------------------------------------------------------------ */
/* Display + LVGL driver                                               */
/* ------------------------------------------------------------------ */
static esp_lcd_panel_handle_t s_panel;
static lv_disp_draw_buf_t     s_draw_buf;
static lv_disp_drv_t          s_disp_drv;
static lv_color_t             s_buf1[LCD_W * LVGL_BUF_ROWS];
static lv_color_t             s_buf2[LCD_W * LVGL_BUF_ROWS];

/* Screenshot plumbing: the httpd task asks, the main loop renders a full
 * frame while the flush callback copies every area into the capture strips
 * (4 x 8 KB rather than one 32 KB block -- the heap is too fragmented for
 * that once Wi-Fi + TLS are up). */
#define CAP_STRIPS     4
#define CAP_STRIP_ROWS (LCD_H / CAP_STRIPS)
static uint16_t          *s_cap[CAP_STRIPS];
static bool               s_capturing;
static volatile bool      s_cap_req;
static SemaphoreHandle_t  s_cap_done;

/* The ST7735's DMA transfer finished: LVGL may reuse the buffer. */
static bool on_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *ev, void *ctx)
{
    lv_disp_flush_ready((lv_disp_drv_t *)ctx);
    return false;
}

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *a, lv_color_t *px)
{
    if (s_capturing) {
        int w = a->x2 - a->x1 + 1;
        for (int y = a->y1; y <= a->y2; y++)
            memcpy(&s_cap[y / CAP_STRIP_ROWS][(y % CAP_STRIP_ROWS) * LCD_W + a->x1],
                   &px[(y - a->y1) * w], w * sizeof(lv_color_t));
    }
    esp_lcd_panel_draw_bitmap(s_panel, a->x1, a->y1, a->x2 + 1, a->y2 + 1, px);
}

static void display_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num     = PIN_LCD_MOSI,
        .miso_io_num     = -1,
        .sclk_io_num     = PIN_LCD_CLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_W * LVGL_BUF_ROWS * sizeof(lv_color_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    lv_init();
    lv_disp_draw_buf_init(&s_draw_buf, s_buf1, s_buf2, LCD_W * LVGL_BUF_ROWS);
    lv_disp_drv_init(&s_disp_drv);

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num         = PIN_LCD_DC,
        .cs_gpio_num         = PIN_LCD_CS,
        .pclk_hz             = LCD_PIXEL_CLK,
        .lcd_cmd_bits        = 8,
        .lcd_param_bits      = 8,
        .spi_mode            = 0,
        .trans_queue_depth   = 10,
        .on_color_trans_done = on_trans_done,
        .user_ctx            = &s_disp_drv,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &io));

    /* Panel config proven on this unit (BOARD_REFERENCE §3). Byte order is
     * handled by CONFIG_LV_COLOR_16_SWAP -- see DEV_KIT E-10 case B. */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7735(io, &panel_cfg, &s_panel));
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, false);
    esp_lcd_panel_set_gap(s_panel, 0, 0);
    esp_lcd_panel_swap_xy(s_panel, true);
    esp_lcd_panel_mirror(s_panel, true, false);
    esp_lcd_panel_disp_on_off(s_panel, true);

    s_disp_drv.flush_cb = flush_cb;
    s_disp_drv.draw_buf = &s_draw_buf;
    s_disp_drv.hor_res  = LCD_W;
    s_disp_drv.ver_res  = LCD_H;
    lv_disp_drv_register(&s_disp_drv);
}

static void cap_free(void)
{
    for (int i = 0; i < CAP_STRIPS; i++) { free(s_cap[i]); s_cap[i] = NULL; }
}

bool app_capture_frame(uint16_t **strips, int *nstrips, int *rows)
{
    if (!s_cap_done) return false;
    xSemaphoreTake(s_cap_done, 0);              /* clear a stale give */
    s_cap_req = true;
    if (xSemaphoreTake(s_cap_done, pdMS_TO_TICKS(3000)) != pdTRUE || !s_cap[0]) {
        s_cap_req = false;
        return false;
    }
    for (int i = 0; i < CAP_STRIPS; i++) { strips[i] = s_cap[i]; s_cap[i] = NULL; }
    *nstrips = CAP_STRIPS;
    *rows = CAP_STRIP_ROWS;
    return true;
}

static void capture_service(void)
{
    if (!s_cap_req) return;
    s_cap_req = false;
    bool ok = true;
    for (int i = 0; i < CAP_STRIPS; i++)
        ok &= (s_cap[i] = malloc(LCD_W * CAP_STRIP_ROWS * sizeof(uint16_t))) != NULL;
    if (ok) {
        s_capturing = true;
        lv_obj_invalidate(lv_scr_act());
        lv_obj_invalidate(lv_layer_top());
        lv_refr_now(NULL);
        /* Wait for the last DMA so the copy is complete before handing off. */
        lv_disp_t *d = lv_disp_get_default();
        while (d && d->driver->draw_buf->flushing) vTaskDelay(1);
        s_capturing = false;
    } else {
        cap_free();
    }
    xSemaphoreGive(s_cap_done);
}

/* ------------------------------------------------------------------ */
/* Button: tap = next page, hold = refresh; first press only wakes     */
/* ------------------------------------------------------------------ */
static void button_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_BTN,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
}

static void button_poll(void)
{
    static bool    down, long_fired, wake_only;
    static int     stable;
    static int64_t t_down;
    bool pressed = gpio_get_level(PIN_BTN) == 0;
    int64_t now = esp_timer_get_time();

    int sim = g_btn_sim;
    if (sim) {                                  /* remote press from the web API */
        g_btn_sim = 0;
        screen_wake();
        if (sim == 2) { usage_announce_next(); usage_poll_now(); app_sound_now(MELODY_BUTTON); ui_button_long(); }
        else          ui_button_short();
        return;
    }

    /* 3 consecutive samples (~30 ms) to change state */
    if (pressed != down) {
        if (++stable < 3) return;
        stable = 0;
        down = pressed;
        if (down) {
            t_down = now;
            long_fired = false;
            wake_only = !screen_is_on();
            screen_wake();
        } else if (!wake_only && !long_fired) {
            ui_button_short();
        }
        return;
    }
    stable = 0;

    if (down && !wake_only && !long_fired && now - t_down > LONG_PRESS_MS * 1000) {
        long_fired = true;
        ESP_LOGI(TAG, "long press: refresh + announce");
        usage_announce_next();          /* speak the result of this poll */
        usage_poll_now();
        app_sound_now(MELODY_BUTTON);
        ui_button_long();
    }
}

/* ------------------------------------------------------------------ */
/* OTA rollback handshake                                              */
/* ------------------------------------------------------------------ */
static void ota_confirm_image(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) != ESP_OK) return;
    if (st != ESP_OTA_IMG_PENDING_VERIFY) return;
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
        ESP_LOGW(TAG, "OTA image on %s confirmed valid - rollback cancelled", run->label);
    else
        ESP_LOGE(TAG, "esp_ota_mark_app_valid_cancel_rollback failed");
}

/* ------------------------------------------------------------------ */
/* app_main                                                            */
/* ------------------------------------------------------------------ */
void app_main(void)
{
    /* Keep the rails up on battery */
    gpio_reset_pin(PIN_PWR_HOLD);
    gpio_set_direction(PIN_PWR_HOLD, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_PWR_HOLD, 1);

    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs);

    usage_init();
    settings_init();

    /* 40-160 MHz scaling. Light sleep stays OFF: it kills the SPI bus. */
    esp_pm_config_t pm = { .max_freq_mhz = 160, .min_freq_mhz = 40, .light_sleep_enable = false };
    ESP_ERROR_CHECK(esp_pm_configure(&pm));

    led_init();
    led_show(LED_BOOT);
    power_init();                   /* backlight starts at 0 */
    display_init();
    screen_init(s_panel);
    s_cap_done = xSemaphoreCreateBinary();
    ui_init();
    lv_refr_now(NULL);              /* paint the splash while still dark */

    if (audio_init() != ESP_OK)
        ESP_LOGW(TAG, "audio init failed - continuing without sound");
    voice_init();                   /* tones if no pack is installed */
    button_init();
    power_sample();
    net_start();

    int64_t t0 = esp_timer_get_time();
    int64_t online_at = 0;
    bool ota_checked = false;
    bool fading = true;

    for (;;) {
        int64_t now = esp_timer_get_time();

        if (fading) {
            settings_t c;
            settings_get(&c);
            int target = g_sys.ext_power ? c.bl_usb : c.bl_batt;
            int64_t ms = (now - t0) / 1000;
            if (ms >= FADE_IN_MS) {
                screen_set_dim_override(-1);
                fading = false;
            } else {
                screen_set_dim_override((int)(target * ms / FADE_IN_MS));
            }
        }

        /* LVGL tick fed from esp_timer here: no 1 kHz tick interrupt. */
        static int64_t last_tick_us;
        if (last_tick_us) lv_tick_inc((uint32_t)((now - last_tick_us) / 1000));
        last_tick_us = now - (now - last_tick_us) % 1000;

        button_poll();
        ui_tick();
        lv_timer_handler();
        capture_service();
        led_tick();
        screen_tick();

        /* Auto-rollback: an OTA image that reaches the online state and
         * stays up OTA_SELFTEST_S is trusted. Setup mode never confirms. */
        if (!ota_checked && g_sys.boot >= BOOT_FETCH && g_sys.boot != BOOT_SETUP) {
            if (!online_at) online_at = now;
            if (now - online_at > (int64_t)OTA_SELFTEST_S * 1000000) {
                ota_confirm_image();
                ota_checked = true;
            }
        }
        if (g_sys.boot == BOOT_SETUP) led_show(LED_SETUP);

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
