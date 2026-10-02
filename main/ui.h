/*
 * ui.h  --  all LVGL screens. Main loop only.
 */
#pragma once

void ui_init(void);             /* after lv_init + display driver; shows splash */
void ui_tick(void);             /* every main-loop iteration                     */
void ui_button_short(void);     /* next page                                     */
void ui_button_long(void);      /* refresh feedback                              */
int  ui_page(void);             /* current page index, -1 splash/setup (any task) */
