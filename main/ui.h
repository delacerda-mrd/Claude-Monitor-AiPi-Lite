/*
 * ui.h  --  all LVGL screens. Main loop only.
 */
#pragma once

void ui_init(void);             /* after lv_init + display driver; shows splash */
void ui_tick(void);             /* every main-loop iteration                     */
void ui_button_short(void);     /* next page                                     */
void ui_button_prev(void);      /* previous page (left button)                   */
void ui_goodbye(void);          /* paint a power-off screen (main loop)          */
void ui_button_long(void);      /* refresh feedback                              */
int  ui_page(void);

/* Requests from other tasks (voice commands), consumed by ui_tick(). */
typedef enum {
    UI_REQ_NONE = 0,
    UI_REQ_PAGE_RINGS, UI_REQ_PAGE_PACE, UI_REQ_PAGE_TREND, UI_REQ_PAGE_CLAWD, UI_REQ_PAGE_SYSTEM,
    UI_REQ_NEXT, UI_REQ_PREV, UI_REQ_SCREEN_OFF,
} ui_req_t;
void ui_request(ui_req_t r);             /* current page index, -1 splash/setup (any task) */
