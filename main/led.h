/*
 * led.h  --  WS2812 status pixel. led_show() may be called from any task;
 * only led_tick() (main loop) touches the RMT strip.
 */
#pragma once

#include <stdbool.h>

typedef enum {
    LED_BOOT,       /* dim blue                                   */
    LED_OK,         /* green: worst usage < amber threshold       */
    LED_WARN,       /* amber                                      */
    LED_HIGH,       /* red                                        */
    LED_LIMIT,      /* red, slow breathe: rate-limited (>=100%)   */
    LED_ERROR,      /* red: poll failing                          */
    LED_SETUP,      /* blue, slow breathe: setup AP waiting       */
    LED_OFF,
} led_state_t;

void led_init(void);
void led_show(led_state_t s);
void led_show_for_pct(int worst_pct);
void led_tick(void);
void led_set_listening(bool on);    /* cyan breathe overlay while listening */
