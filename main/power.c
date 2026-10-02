/*
 * power.c  --  battery ADC (GPIO2 / ADC1_CH1), external-power detection,
 * backlight PWM (GPIO3, LEDC) and the screen-blank manager.
 *
 * The battery table and the wall-charger voltage-trend heuristic are carried
 * over unchanged from v1 (calibrated on this unit -- see BOARD_REFERENCE §8).
 */
#include "power.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "driver/ledc.h"
#include "driver/usb_serial_jtag.h"
#include "esp_adc/adc_oneshot.h"

#include "app.h"
#include "board.h"
#include "settings.h"

static const char *TAG = "power";

/* ------------------------------------------------------------------ */
/* Battery + power source (poll task)                                  */
/* ------------------------------------------------------------------ */
static adc_oneshot_unit_handle_t s_adc;
static int  s_prev_avg;
static int  s_avg;
static bool s_charger_trend;

void power_init(void)
{
    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = ADC_UNIT_1 };
    if (adc_oneshot_new_unit(&ucfg, &s_adc) == ESP_OK) {
        adc_oneshot_chan_cfg_t ch = {
            .atten    = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_12,
        };
        adc_oneshot_config_channel(s_adc, ADC_CHANNEL_1, &ch);
    } else {
        ESP_LOGW(TAG, "battery ADC init failed");
        s_adc = NULL;
    }

    ledc_timer_config_t tmr = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = 5000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&tmr));
    ledc_channel_config_t chn = {
        .gpio_num   = PIN_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 0,            /* dark until the splash has painted */
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&chn));
}

static void batt_update(void)
{
    if (!s_adc) return;
    int raw = 0;
    if (adc_oneshot_read(s_adc, ADC_CHANNEL_1, &raw) != ESP_OK) return;

    /* EMA smooths low-battery noise that could look like a charge curve */
    s_avg = s_avg == 0 ? raw : (s_avg * 3 + raw) / 4;

    /* Wall-charger detection from the voltage trend: rising, or pinned at
     * the top (a resting battery drops >2 pts/cycle and exits this). */
    bool charging   = (s_prev_avg > 0) && (s_avg > s_prev_avg + 8);
    bool pinned_max = (s_avg >= 1975) && (s_avg >= s_prev_avg - 2);
    s_charger_trend = charging || pinned_max;
    s_prev_avg = s_avg;

    /* {adc_raw, pct} — calibrated on this AIPI-Lite unit */
    static const int table[][2] = {
        {1480, 0}, {1581, 20}, {1663, 40}, {1750, 60}, {1840, 80}, {1980, 100}
    };
    int pct = 0;
    for (int i = 0; i < 5; i++) {
        if (s_avg <= table[i + 1][0]) {
            int d_raw = table[i + 1][0] - table[i][0];
            int d_pct = table[i + 1][1] - table[i][1];
            pct = table[i][1] + (s_avg - table[i][0]) * d_pct / d_raw;
            break;
        }
        pct = 100;
    }
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    g_sys.batt_pct = pct;
}

/* USB-host (SOF) detection is fast and reliable; combine with the slow
 * charger trend. Applies Wi-Fi power save on transitions only. */
void power_eval(void)
{
    static int applied = -1;
    bool ext = usb_serial_jtag_is_connected() || s_charger_trend;
    g_sys.ext_power = ext;
    if ((int)ext == applied) return;
    applied = ext;
    if (g_sys.wifi_up)
        esp_wifi_set_ps(ext ? WIFI_PS_MIN_MODEM : WIFI_PS_MAX_MODEM);
    ESP_LOGI(TAG, "power: %s", ext ? "external" : "battery");
}

void power_sample(void)
{
    batt_update();
    power_eval();
}

/* ------------------------------------------------------------------ */
/* Screen manager (main loop only)                                     */
/* ------------------------------------------------------------------ */
static esp_lcd_panel_handle_t s_panel;
static volatile bool s_on = true;
static volatile bool s_wake_req;
static int64_t       s_last_activity_us;
static int           s_dim_override = -1;

void screen_init(esp_lcd_panel_handle_t panel)
{
    s_panel = panel;
    s_last_activity_us = esp_timer_get_time();
}

bool screen_is_on(void) { return s_on; }
void screen_wake(void)  { s_wake_req = true; }
void screen_set_dim_override(int pct) { s_dim_override = pct; }

static void apply_backlight(void)
{
    static int last = -1;
    settings_t c;
    settings_get(&c);
    int pct = !s_on ? 0
            : s_dim_override >= 0 ? s_dim_override
            : (g_sys.ext_power ? c.bl_usb : c.bl_batt);
    int duty = pct * 255 / 100;
    if (duty == last) return;
    last = duty;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void screen_set(bool on)
{
    if (on == s_on) return;
    s_on = on;
    ESP_LOGI(TAG, "screen %s", on ? "on" : "off");
    if (on) {
        esp_lcd_panel_disp_on_off(s_panel, true);
        apply_backlight();
    } else {
        apply_backlight();                      /* backlight off first */
        esp_lcd_panel_disp_on_off(s_panel, false);
    }
}

void screen_tick(void)
{
    int64_t now = esp_timer_get_time();
    if (s_wake_req) {
        s_wake_req = false;
        s_last_activity_us = now;
        screen_set(true);
    }
    settings_t c;
    settings_get(&c);
    if (s_on && c.blank_s > 0 &&
        now - s_last_activity_us > (int64_t)c.blank_s * 1000000) {
        screen_set(false);
    }
    apply_backlight();
}
