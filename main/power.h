/*
 * power.h  --  battery sensing, external-power detection, backlight and the
 * screen-blank manager.
 *
 *   poll task : power_sample()  (ADC + power-source detection, Wi-Fi PS)
 *   main loop : screen_*()      (backlight PWM + panel on/off)
 */
#pragma once

#include <stdbool.h>
#include "esp_lcd_panel_ops.h"

void power_init(void);          /* ADC + backlight PWM; call before UI     */
void power_sample(void);        /* poll task: refresh g_sys.batt/ext_power */
void power_eval(void);          /* poll task: cheap re-check of USB state  */

void screen_init(esp_lcd_panel_handle_t panel);
void screen_tick(void);         /* main loop: idle timeout + brightness    */
void screen_wake(void);         /* any task: request screen on + reset idle */
bool screen_is_on(void);
void screen_set_dim_override(int pct);   /* main loop: -1 = none (boot fade) */

/* True power-off on battery: release the GPIO10 latch (stock-firmware
 * method). Returns only if it can't (USB keeps the board powered). */
void power_off(void);
