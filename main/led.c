/*
 * led.c  --  WS2812 status pixel (GPIO46). Kept very dim: it sits next to
 * the screen and is visible in a dark room.
 */
#include "led.h"

#include "esp_timer.h"
#include "led_strip.h"

#include "app.h"
#include "board.h"

static led_strip_handle_t s_strip;
static volatile led_state_t s_state = LED_BOOT;

void led_init(void)
{
    led_strip_config_t cfg = {
        .strip_gpio_num = PIN_LED,
        .max_leds       = 1,
    };
    led_strip_rmt_config_t rmt = {
        .resolution_hz = 10 * 1000 * 1000,
    };
    if (led_strip_new_rmt_device(&cfg, &rmt, &s_strip) == ESP_OK)
        led_strip_clear(s_strip);
}

void led_show(led_state_t s) { s_state = s; }

void led_show_for_pct(int worst)
{
    if      (worst >= 100)             led_show(LED_LIMIT);
    else if (worst >= USAGE_RED_PCT)   led_show(LED_HIGH);
    else if (worst >= USAGE_AMBER_PCT) led_show(LED_WARN);
    else                               led_show(LED_OK);
}

/* Main loop only. Refreshes the strip on a state change, and at ~20 Hz
 * while a breathing state is active. */
void led_tick(void)
{
    static led_state_t last = (led_state_t)-1;
    static int64_t     last_us;
    if (!s_strip) return;

    led_state_t st = s_state;
    bool breathe = (st == LED_LIMIT || st == LED_SETUP);
    int64_t now = esp_timer_get_time();
    if (st == last && (!breathe || now - last_us < 50000)) return;
    last = st;
    last_us = now;

    uint8_t r = 0, g = 0, b = 0;
    switch (st) {
    case LED_BOOT:  b = 1;        break;
    case LED_OK:    g = 1;        break;
    case LED_WARN:  r = 1; g = 1; break;
    case LED_HIGH:
    case LED_ERROR: r = 1;        break;
    case LED_LIMIT:
    case LED_SETUP: {
        /* triangle wave 0..4..0 over 2.4 s */
        int ph = (int)((now / 1000) % 2400);
        int v = ph < 1200 ? ph * 4 / 1200 : (2400 - ph) * 4 / 1200;
        if (st == LED_LIMIT) r = v; else b = v;
        break;
    }
    case LED_OFF:   break;
    }
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}
