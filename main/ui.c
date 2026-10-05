/*
 * ui.c  --  Claude Meter v2 interface for a 128x128 ST7735.
 *
 *   splash  : R2-D2 + boot stage, until the first poll lands
 *   setup   : Wi-Fi setup AP: join-QR + password (no network)
 *   pages   : COMMAND -> PACE -> TREND -> CLAWD -> SYSTEMS (short press cycles)
 *   top bar : clock . page ticks . sync spinner, Wi-Fi, charge, battery
 *             (on lv_layer_top so it stays put while pages slide)
 *
 * Look: R2-D2 (user, 2026-10-05) -- the ES3C28P's cockpit layout (gauge,
 * targeting tapes, HUD panels, holo-pad) scaled to 128 px and recoloured:
 * white data, R2 blue, silver; R2's red logic light for critical.
 * Splash and setup keep the Anthropic palette.
 * LVGL is single-threaded: everything here runs on the main loop.
 */
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "lvgl.h"

#include "app.h"
#include "board.h"
#include "brain.h"
#include "clawd.h"
#include "listen.h"
#include "net.h"
#include "power.h"
#include "settings.h"
#include "usage.h"

LV_FONT_DECLARE(font_jbm_bold_26);
LV_FONT_DECLARE(font_jbm_bold_16);
LV_FONT_DECLARE(font_jbm_medium_11);
LV_FONT_DECLARE(font_styrene_12);
LV_FONT_DECLARE(font_styrene_14);

#define F_BIG    (&font_jbm_bold_26)    /* digits, % - only */
#define F_NUM    (&font_jbm_bold_16)    /* digits, % + - . : d h m s only */
#define F_MONO   (&font_jbm_medium_11)
#define F_SMALL  (&font_styrene_12)
#define F_TITLE  (&font_styrene_14)
#define F_ICON   (&lv_font_montserrat_10)

/* ------------------------------------------------------------------ */
/* Palette                                                             */
/* ------------------------------------------------------------------ */
/* Anthropic palette: splash, setup, goodbye */
#define C_BG      0x000000
#define C_TEXT    0xFAF9F5
#define C_DIM     0xB0AEA5
#define C_FAINT   0x6B6A64

/* R2-D2: the pages */
#define R_PANEL   0x050B16      /* panel fill                        */
#define R_OFF     0x10203A      /* unlit tick / segment              */
#define R_FRAME   0x1F3F73      /* panel borders, hairlines          */
#define R_CAPTION 0x4F7FC8      /* small-caps captions               */
#define R_BLUE    0x2B6CE8      /* R2 blue: nominal, accents         */
#define R_PALE    0x9CC2FF      /* elevated                          */
#define R_RED     0xFF3B3B      /* critical: R2's red logic light    */
#define R_TEXT    0xF4F7FF      /* data (white)                      */
#define R_DIM     0x9AA6B8      /* secondary data (dome silver)      */
#define R_STALE   0x5A6475
#define R_HOLO    0x4FA8FF      /* hologram blue                     */

#define HEX(c)    lv_color_hex(c)

typedef enum { PAGE_RINGS, PAGE_PACE, PAGE_TREND, PAGE_CLAWD, PAGE_SYSTEM, PAGE_COUNT } page_t;
typedef enum { MODE_SPLASH, MODE_PAGES, MODE_SETUP } ui_mode_t;

#define SPLASH_MIN_MS     1800
#define SPLASH_MAX_MS     30000
#define PAGE_RETURN_S     60        /* non-home pages fall back to COMMAND */
#define DANCE_S           300       /* Clawd celebrates a reset this long */
#define STATUS_H          14        /* top bar; page bodies start at y 15 */

static ui_mode_t s_mode = MODE_SPLASH;
static int64_t   s_splash_t0;
static lv_obj_t *s_scr[PAGE_COUNT];
static page_t    s_page;
static int64_t   s_page_t0;
static volatile ui_req_t s_req;
static volatile int64_t  s_input_us;     /* last button press */
static uint32_t  s_seq = UINT32_MAX;
static int64_t   s_last_sec_us;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */
static uint32_t r2_level(int pct)
{
    if (pct >= USAGE_RED_PCT)   return R_RED;
    if (pct >= USAGE_AMBER_PCT) return R_PALE;
    return R_BLUE;
}

static lv_obj_t *mk_label(lv_obj_t *p, const lv_font_t *f, uint32_t color, const char *txt)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, HEX(color), 0);
    lv_label_set_text(l, txt);
    return l;
}

/* Only touch LVGL when the text actually changes (avoids needless redraws). */
static void set_text(lv_obj_t *l, const char *t)
{
    if (strcmp(lv_label_get_text(l), t) != 0) lv_label_set_text(l, t);
}

static void set_color(lv_obj_t *l, lv_color_t c)
{
    if (lv_obj_get_style_text_color(l, 0).full != c.full)
        lv_obj_set_style_text_color(l, c, 0);
}

static lv_obj_t *mk_screen(void)
{
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_remove_style_all(s);
    lv_obj_set_style_bg_color(s, HEX(C_BG), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    return s;
}

static lv_obj_t *mk_box(lv_obj_t *p, int x, int y, int w, int h, uint32_t color, int radius)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, HEX(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

/* A label that spans the screen width, text centred, at y. */
static lv_obj_t *mk_centered(lv_obj_t *p, const lv_font_t *f, uint32_t color, int x, int w, int y)
{
    lv_obj_t *l = mk_label(p, f, color, "");
    lv_obj_set_width(l, w);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

static bool clock_ok(time_t now) { return now > 1600000000; }

/* "2h05m" / "3d4h" / "12m" / "now" -- compact for a 128 px screen */
static void fmt_dur(char *b, size_t n, long s)
{
    if (s <= 0) { snprintf(b, n, "now"); return; }
    long d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60;
    if (d > 0)      snprintf(b, n, "%ldd%ldh", d, h);
    else if (h > 0) snprintf(b, n, "%ldh%02ldm", h, m);
    else            snprintf(b, n, "%ldm", m < 1 ? 1 : m);
}

static void fmt_clock(char *b, size_t n, time_t t, bool h24)
{
    struct tm tm;
    localtime_r(&t, &tm);
    if (h24) snprintf(b, n, "%d:%02d", tm.tm_hour, tm.tm_min);
    else     snprintf(b, n, "%d:%02d", tm.tm_hour % 12 ? tm.tm_hour % 12 : 12, tm.tm_min);
}

static void fmt_reset(char *b, size_t n, time_t reset)
{
    time_t now = time(NULL);
    if (!reset || !clock_ok(now)) snprintf(b, n, "--");
    else fmt_dur(b, n, (long)(reset - now));
}

/* Plain "4h38m" -- the "T-" prefix was dropped (user, 2026-10-05). */
static void countdown(char *b, size_t n, time_t reset)
{
    fmt_reset(b, n, reset);
}

/* Elapsed share of a window, from its reset time. false if unknown. */
static bool window_elapsed(time_t reset, long window, long *elapsed_s)
{
    time_t now = time(NULL);
    if (!reset || !clock_ok(now)) return false;
    long left = (long)(reset - now);
    if (left < 0) left = 0;
    if (left > window) left = window;
    *elapsed_s = window - left;
    return true;
}

/* ------------------------------------------------------------------ */
/* HUD kit (from the ES3C28P cockpit, scaled for 128 px)               */
/* ------------------------------------------------------------------ */
static lv_obj_t *caption(lv_obj_t *p, const char *txt, int x, int y)
{
    lv_obj_t *l = mk_label(p, F_SMALL, R_CAPTION, txt);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

/* HUD panel: dark fill, 1 px frame, R2-blue corner notches, optional title. */
static lv_obj_t *panel(lv_obj_t *s, int x, int y, int w, int h, const char *title)
{
    lv_obj_t *p = mk_box(s, x, y, w, h, R_PANEL, 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_border_color(p, HEX(R_FRAME), 0);
    lv_obj_set_style_border_opa(p, LV_OPA_COVER, 0);
    const int L = 4;
    mk_box(p, 0, 0, L, 1, R_BLUE, 0);          mk_box(p, 0, 0, 1, L, R_BLUE, 0);
    mk_box(p, w - L, 0, L, 1, R_BLUE, 0);      mk_box(p, w - 1, 0, 1, L, R_BLUE, 0);
    mk_box(p, 0, h - 1, L, 1, R_BLUE, 0);      mk_box(p, 0, h - L, 1, L, R_BLUE, 0);
    mk_box(p, w - L, h - 1, L, 1, R_BLUE, 0);  mk_box(p, w - 1, h - L, 1, L, R_BLUE, 0);
    if (title) caption(p, title, 4, 1);
    return p;
}

/* Segmented "targeting tape": n segments, lit up to pct, plus an
 * even-pace marker. Recolours only segments that change. */
#define TAPE_MAX 24
typedef struct {
    lv_obj_t *seg[TAPE_MAX], *mark;
    int n, x, w, lit;
    uint32_t col;
} tape_t;

static void tape_build(lv_obj_t *p, tape_t *t, int x, int y, int w, int h, int n)
{
    const int gap = 1;
    if (n > TAPE_MAX) n = TAPE_MAX;
    int sw = (w - (n - 1) * gap) / n;
    t->n = n; t->x = x; t->w = n * sw + (n - 1) * gap; t->lit = 0; t->col = R_OFF;
    for (int i = 0; i < n; i++) t->seg[i] = mk_box(p, x + i * (sw + gap), y, sw, h, R_OFF, 0);
    t->mark = mk_box(p, x, y - 3, 2, h + 6, R_TEXT, 0);
    lv_obj_add_flag(t->mark, LV_OBJ_FLAG_HIDDEN);
}

static void tape_set(tape_t *t, int pct, uint32_t col, int even_pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int lit = (pct * t->n + 50) / 100;
    if (pct > 0 && lit == 0) lit = 1;
    for (int i = 0; i < t->n; i++) {
        bool was = i < t->lit, now = i < lit;
        if (was != now || (now && col != t->col))
            lv_obj_set_style_bg_color(t->seg[i], HEX(now ? col : R_OFF), 0);
    }
    t->lit = lit; t->col = col;
    if (even_pct < 0) { lv_obj_add_flag(t->mark, LV_OBJ_FLAG_HIDDEN); return; }
    if (even_pct > 100) even_pct = 100;
    lv_coord_t mx = t->x + even_pct * (t->w - 2) / 100;
    if (lv_obj_get_x(t->mark) != mx) lv_obj_set_x(t->mark, mx);
    lv_obj_clear_flag(t->mark, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *mk_hud_arc(lv_obj_t *p, int cx, int cy, int d, int w, int rot, int sweep,
                            uint32_t color, lv_opa_t opa)
{
    lv_obj_t *a = lv_arc_create(p);
    lv_obj_remove_style_all(a);
    lv_obj_set_size(a, d, d);
    lv_obj_set_pos(a, cx - d / 2, cy - d / 2);
    lv_arc_set_rotation(a, rot);
    lv_arc_set_bg_angles(a, 0, sweep);
    lv_arc_set_range(a, 0, 100);
    lv_arc_set_value(a, 0);
    lv_obj_set_style_arc_width(a, w, LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, HEX(color), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(a, opa, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, w, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
    return a;
}

/* Pace of a window: where an even burn would be, burn rate, time to 100 %. */
typedef struct {
    bool known;         /* window timing known                         */
    int  even;          /* % where an even burn would be now           */
    int  delta;         /* pct - even                                  */
    long rate_x100;     /* burn rate x100 per hour (5 h) or per day (7 d) */
    long limit_s;       /* seconds to 100 % at this rate, -1 = clear   */
} pace_t;

static pace_t pace_of(int pct, time_t reset, long window, long unit_s)
{
    pace_t p = { 0 };
    long el;
    if (!window_elapsed(reset, window, &el)) return p;
    p.known = true;
    p.even = (int)(el * 100 / window);
    p.delta = pct - p.even;
    p.rate_x100 = el > 0 ? (long)((int64_t)pct * 100 * unit_s / el) : 0;
    p.limit_s = -1;
    if (pct > 0 && pct < 100 && el > 0) {
        long to_full = (long)((int64_t)(100 - pct) * el / pct);
        if (to_full < window - el) p.limit_s = to_full;
    } else if (pct >= 100) {
        p.limit_s = 0;
    }
    return p;
}

/* "+12 AHEAD" / "ON PACE" / "8 UNDER" */
static void verdict(char *b, size_t n, const pace_t *p, uint32_t *col)
{
    if (!p->known)           { snprintf(b, n, "--");                 *col = R_DIM; }
    else if (p->delta > 10)  { snprintf(b, n, "+%d AHEAD", p->delta); *col = p->delta > 25 ? R_RED : R_PALE; }
    else if (p->delta < -10) { snprintf(b, n, "%d UNDER", -p->delta); *col = R_DIM; }
    else                     { snprintf(b, n, "ON PACE");            *col = R_BLUE; }
}

/* Soft area fill under the chart's lead series (the event user data);
 * grid: top division line = the 100 % cap in R2 blue, bottom one hidden. */
static void chart_draw_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
    lv_chart_series_t *fill = lv_event_get_user_data(e);
    if (dsc->type == LV_CHART_DRAW_PART_DIV_LINE_HOR && dsc->line_dsc) {
        /* LVGL reuses one line_dsc for every division line: set both ways */
        dsc->line_dsc->opa = dsc->id == 4 ? LV_OPA_TRANSP : LV_OPA_COVER;    /* 5 lines: hide 0 % */
        dsc->line_dsc->color = HEX(dsc->id == 0 ? R_BLUE : R_OFF);
        return;
    }
    if (dsc->part != LV_PART_ITEMS || dsc->sub_part_ptr != fill) return;
    if (!dsc->p1 || !dsc->p2 || !dsc->line_dsc) return;

    lv_draw_mask_line_param_t line_mask;
    lv_draw_mask_line_points_init(&line_mask, dsc->p1->x, dsc->p1->y, dsc->p2->x, dsc->p2->y,
                                  LV_DRAW_MASK_LINE_SIDE_BOTTOM);
    int16_t line_id = lv_draw_mask_add(&line_mask, NULL);
    lv_draw_mask_fade_param_t fade;
    lv_draw_mask_fade_init(&fade, &obj->coords, LV_OPA_COVER, obj->coords.y1,
                           LV_OPA_TRANSP, obj->coords.y2);
    int16_t fade_id = lv_draw_mask_add(&fade, NULL);

    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_opa = LV_OPA_40;
    r.bg_color = dsc->line_dsc->color;
    lv_area_t a = {
        .x1 = dsc->p1->x, .x2 = dsc->p2->x - 1,
        .y1 = LV_MIN(dsc->p1->y, dsc->p2->y), .y2 = obj->coords.y2,
    };
    lv_draw_rect(dsc->draw_ctx, &r, &a);

    lv_draw_mask_free_param(&line_mask);
    lv_draw_mask_free_param(&fade);
    lv_draw_mask_remove_id(line_id);
    lv_draw_mask_remove_id(fade_id);
}

/* ------------------------------------------------------------------ */
/* Status bar (top layer)                                              */
/* ------------------------------------------------------------------ */
static struct {
    lv_obj_t *bar, *clock, *pos[PAGE_COUNT], *spin, *wifi, *bolt, *batt, *batt_fill, *nub;
} s_sb;

static void sb_build(void)
{
    lv_obj_t *top = lv_layer_top();
    s_sb.bar = mk_box(top, 0, 0, LCD_W, STATUS_H, C_BG, 0);
    mk_box(s_sb.bar, 0, STATUS_H - 1, LCD_W, 1, R_FRAME, 0);

    s_sb.clock = mk_label(s_sb.bar, F_MONO, R_TEXT, "--:--");
    lv_obj_set_pos(s_sb.clock, 3, 0);

    for (int i = 0; i < PAGE_COUNT; i++)            /* page position: 5 ticks */
        s_sb.pos[i] = mk_box(s_sb.bar, 42 + i * 6, 5, 4, 3, R_OFF, 0);

    /* right cluster, laid out right-to-left in sb_layout() */
    s_sb.spin = lv_spinner_create(s_sb.bar, 900, 90);
    lv_obj_remove_style_all(s_sb.spin);
    lv_obj_set_size(s_sb.spin, 9, 9);
    lv_obj_set_style_arc_width(s_sb.spin, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_sb.spin, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_sb.spin, 2, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_sb.spin, HEX(R_BLUE), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_sb.spin, true, LV_PART_INDICATOR);
    lv_obj_add_flag(s_sb.spin, LV_OBJ_FLAG_HIDDEN);

    s_sb.wifi = mk_label(s_sb.bar, F_ICON, R_DIM, LV_SYMBOL_WIFI);
    s_sb.bolt = mk_label(s_sb.bar, F_ICON, R_BLUE, LV_SYMBOL_CHARGE);

    /* battery: 15x8 body + 2x4 nub */
    s_sb.batt = lv_obj_create(s_sb.bar);
    lv_obj_remove_style_all(s_sb.batt);
    lv_obj_set_size(s_sb.batt, 17, 8);
    lv_obj_set_pos(s_sb.batt, LCD_W - 3 - 17, 2);
    lv_obj_t *body = lv_obj_create(s_sb.batt);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, 15, 8);
    lv_obj_set_style_border_width(body, 1, 0);
    lv_obj_set_style_border_color(body, HEX(R_DIM), 0);
    lv_obj_set_style_radius(body, 2, 0);
    s_sb.batt_fill = mk_box(s_sb.batt, 2, 2, 11, 4, R_TEXT, 1);
    s_sb.nub = mk_box(s_sb.batt, 15, 2, 2, 4, R_DIM, 1);

    lv_obj_add_flag(s_sb.bar, LV_OBJ_FLAG_HIDDEN);
}

/* Right cluster, right-to-left: battery | bolt (on external power) | wifi |
 * spinner (while polling). Icons are 10 px montserrat symbols. */
static void sb_layout(void)
{
    lv_coord_t x = LCD_W - 3 - 17 - 3;          /* left edge of the battery - gap */
    lv_obj_t *seq[] = { s_sb.bolt, s_sb.wifi, s_sb.spin };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *o = seq[i];
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) continue;
        lv_coord_t w = o == s_sb.bolt ? lv_obj_get_width(o) : o == s_sb.wifi ? 13 : 9;
        x -= w;
        lv_obj_set_pos(o, x, o == s_sb.spin ? 1 : 0);
        x -= 3;
    }
}

static void sb_set_page(page_t p)
{
    for (int i = 0; i < PAGE_COUNT; i++)
        lv_obj_set_style_bg_color(s_sb.pos[i], HEX((int)p == i ? R_BLUE : R_OFF), 0);
}

static void sb_update(bool polling)
{
    settings_t c;
    settings_get(&c);
    char b[16];
    time_t now = time(NULL);
    if (clock_ok(now)) fmt_clock(b, sizeof(b), now, c.h24);
    else snprintf(b, sizeof(b), "--:--");
    set_text(s_sb.clock, b);

    if (polling) lv_obj_clear_flag(s_sb.spin, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag(s_sb.spin, LV_OBJ_FLAG_HIDDEN);

    int rssi = g_sys.rssi;
    uint32_t wc = !g_sys.wifi_up ? R_RED : (rssi && rssi < -78) ? R_PALE : R_DIM;
    set_color(s_sb.wifi, HEX(wc));

    /* bolt while charging, plug when on USB with a full battery */
    if (g_sys.ext_power) {
        set_text(s_sb.bolt, g_sys.charging ? LV_SYMBOL_CHARGE : LV_SYMBOL_USB);
        lv_obj_clear_flag(s_sb.bolt, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_sb.bolt, LV_OBJ_FLAG_HIDDEN);
    }

    sb_layout();

    int pct = g_sys.batt_pct;
    int w = pct < 0 ? 0 : (pct * 11 + 50) / 100;
    if (w < 1) w = 1;
    lv_obj_set_width(s_sb.batt_fill, w);
    uint32_t fc = g_sys.charging ? R_BLUE : pct <= 15 ? R_RED : pct <= 30 ? R_PALE : R_TEXT;
    lv_obj_set_style_bg_color(s_sb.batt_fill, HEX(fc), 0);
}

/* ------------------------------------------------------------------ */
/* Listening pill (top layer)                                          */
/* ------------------------------------------------------------------ */
static struct { lv_obj_t *box, *dot, *bell, *lbl; int shown; } s_lb;

/* R2's front logic light: the dot flips blue <-> red while listening */
static void psi_anim_cb(void *o, int32_t v)
{
    lv_color_t c = HEX(v < 500 ? R_BLUE : R_RED);
    if (lv_obj_get_style_bg_color(o, 0).full != c.full) lv_obj_set_style_bg_color(o, c, 0);
}

static void lb_build(void)
{
    s_lb.box = mk_box(lv_layer_top(), 4, LCD_H - 22, LCD_W - 8, 18, R_PANEL, 0);
    lv_obj_set_style_border_width(s_lb.box, 1, 0);
    lv_obj_set_style_border_color(s_lb.box, HEX(R_BLUE), 0);
    lv_obj_set_style_border_opa(s_lb.box, LV_OPA_COVER, 0);
    mk_box(s_lb.box, 0, 0, 3, 18, R_BLUE, 0);                  /* comm-channel tab */
    s_lb.dot = mk_box(s_lb.box, 8, 6, 6, 6, R_BLUE, LV_RADIUS_CIRCLE);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_lb.dot);
    lv_anim_set_values(&a, 0, 999);
    lv_anim_set_time(&a, 700);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&a, psi_anim_cb);
    lv_anim_start(&a);
    s_lb.bell = mk_label(s_lb.box, F_ICON, R_PALE, LV_SYMBOL_BELL);
    lv_obj_set_pos(s_lb.bell, 6, 3);
    s_lb.lbl = mk_label(s_lb.box, F_MONO, R_TEXT, "");
    lv_obj_set_size(s_lb.lbl, LCD_W - 8 - 24, 12);
    lv_obj_set_pos(s_lb.lbl, 19, 2);
    lv_obj_add_flag(s_lb.box, LV_OBJ_FLAG_HIDDEN);
    s_lb.shown = -1;
}

/* 0 hidden, 1 listening, 2 showing what was heard (2.5 s), 3 brain working,
 * 4 a notice the Mac pushed (a reminder): stays up until a button press after
 * it arrived, at most 5 minutes -- the backup for a missed calendar toast */
#define NOTICE_MAX_US (300LL * 1000000)
static void lb_update(void)
{
    if (!s_lb.box) return;
    listen_state_t st = listen_state();
    int64_t now = esp_timer_get_time(), heard = listen_heard_us(), noticed;
    const char *notice = brain_notice(&noticed);
    int want = (st == LISTEN_PROMPT || st == LISTEN_COMMAND) ? 1
             : st == LISTEN_THINKING ? 3
             : (noticed && notice[0] && now - noticed < NOTICE_MAX_US &&
                s_input_us < noticed) ? 4
             : (heard && now - heard < 2500000) ? 2 : 0;
    if (want == s_lb.shown) return;
    s_lb.shown = want;
    if (!want) { lv_obj_add_flag(s_lb.box, LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_clear_flag(s_lb.box, LV_OBJ_FLAG_HIDDEN);
    if (want == 4) {                        /* long reminders scroll */
        lv_label_set_long_mode(s_lb.lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
        set_text(s_lb.lbl, notice);
        set_color(s_lb.lbl, HEX(R_TEXT));
        lv_obj_add_flag(s_lb.dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_lb.bell, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_long_mode(s_lb.lbl, LV_LABEL_LONG_DOT);
    lv_obj_add_flag(s_lb.bell, LV_OBJ_FLAG_HIDDEN);
    if (want == 1 || want == 3) {
        set_text(s_lb.lbl, want == 1 ? "Listening..." : "Thinking...");
        lv_obj_clear_flag(s_lb.dot, LV_OBJ_FLAG_HIDDEN);
        set_color(s_lb.lbl, HEX(R_TEXT));
    } else {
        char b[56];
        snprintf(b, sizeof(b), "\"%s\"", listen_heard());
        set_text(s_lb.lbl, b);
        lv_obj_add_flag(s_lb.dot, LV_OBJ_FLAG_HIDDEN);
        set_color(s_lb.lbl, HEX(R_PALE));
    }
}

/* ------------------------------------------------------------------ */
/* Page 1: COMMAND (home)                                              */
/*                                                                     */
/* The gauge: a 31-tick 280 deg meter for the 5 h window (lit ticks R2 */
/* blue -> white along the scale) over a breathing glow band, an inner */
/* arc for 7 d, session % and countdown inside, the status word in the */
/* gap; 7 d value and its reset along the bottom.                      */
/* ------------------------------------------------------------------ */
#define G_CX    64
#define G_CY    71      /* centred in the body (y 14..127) */
#define WRAP_PCT BOARD_DANGER_PCT   /* "WRAP UP n%" in red from here (board.h) */
#define G_D     92
#define G_ROT   130
#define G_SWEEP 280

static struct {
    lv_obj_t *meter, *glow, *week, *num, *pct, *sub, *w_rst, *threat;
    lv_meter_indicator_t *lit;
    int shown, lit_val, level;
    lv_opa_t glow_opa;
} s_r;

static void lit_anim_cb(void *var, int32_t v)
{
    (void)var;
    lv_meter_set_indicator_end_value(s_r.meter, s_r.lit, v);
    lv_arc_set_value(s_r.glow, (int16_t)v);
}

static void arc_anim_cb(void *arc, int32_t v) { lv_arc_set_value(arc, (int16_t)v); }

static void num_anim_cb(void *var, int32_t v)
{
    char b[8];
    snprintf(b, sizeof(b), "%d", (int)v);
    lv_label_set_text(var, b);
}

/* The glow breathes: ~4 s cycle (1.6 s when critical), 100 ms steps, only
 * while COMMAND is on a lit screen. */
static void breathe_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_r.glow || !screen_is_on() || lv_obj_get_screen(s_r.glow) != lv_scr_act()) return;
    int period = s_r.level >= 2 ? 1600 : 4000;
    int ph = (int)((esp_timer_get_time() / 1000) % period);
    int inhale = period * 2 / 5;
    int v = ph < inhale ? ph * 255 / inhale : (period - ph) * 255 / (period - inhale);
    int lo = s_r.level >= 2 ? 70 : 25, hi = s_r.level >= 2 ? 170 : 110;
    lv_opa_t opa = (lv_opa_t)(lo + (hi - lo) * v / 255);
    if (abs((int)opa - (int)s_r.glow_opa) < 6) return;
    s_r.glow_opa = opa;
    lv_obj_set_style_arc_opa(s_r.glow, opa, LV_PART_INDICATOR);
}

static void command_build(lv_obj_t *s)
{
    mk_hud_arc(s, G_CX, G_CY, G_D + 6, 1, G_ROT, G_SWEEP, R_FRAME, LV_OPA_COVER);
    s_r.glow = mk_hud_arc(s, G_CX, G_CY, G_D, 7, G_ROT, G_SWEEP, R_BLUE, LV_OPA_TRANSP);
    lv_obj_set_style_arc_color(s_r.glow, HEX(R_BLUE), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_r.glow, 40, LV_PART_INDICATOR);
    s_r.glow_opa = 40;

    s_r.meter = lv_meter_create(s);
    lv_obj_remove_style_all(s_r.meter);
    lv_obj_set_size(s_r.meter, G_D, G_D);
    lv_obj_set_pos(s_r.meter, G_CX - G_D / 2, G_CY - G_D / 2);
    lv_obj_clear_flag(s_r.meter, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_opa(s_r.meter, LV_OPA_TRANSP, LV_PART_TICKS);      /* no scale labels */
    lv_meter_scale_t *sc = lv_meter_add_scale(s_r.meter);
    lv_meter_set_scale_ticks(s_r.meter, sc, 31, 2, 7, HEX(R_OFF));
    lv_meter_set_scale_major_ticks(s_r.meter, sc, 6, 2, 10, HEX(R_OFF), 20);
    lv_meter_set_scale_range(s_r.meter, sc, 0, 100, G_SWEEP, G_ROT);
    s_r.lit = lv_meter_add_scale_lines(s_r.meter, sc, HEX(R_BLUE), HEX(0xE8F0FF), false, 0);
    lv_meter_set_indicator_start_value(s_r.meter, s_r.lit, 0);
    lv_meter_set_indicator_end_value(s_r.meter, s_r.lit, 0);

    /* 7 d: inner arc */
    s_r.week = mk_hud_arc(s, G_CX, G_CY, 66, 3, G_ROT, G_SWEEP, R_OFF, LV_OPA_COVER);
    lv_obj_set_style_arc_opa(s_r.week, LV_OPA_80, LV_PART_INDICATOR);

    /* "42" + "%" as one centred row; flex re-centres as digits change.
     * Bottom-aligned, the % lifted by the fonts' descent difference. */
    lv_obj_t *pair = lv_obj_create(s);
    lv_obj_remove_style_all(pair);
    lv_obj_set_size(pair, LCD_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(pair, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pair, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(pair, 1, 0);
    lv_obj_set_pos(pair, 0, G_CY - 13);
    s_r.num = mk_label(pair, F_BIG, R_TEXT, "--");
    s_r.pct = mk_label(pair, F_TITLE, R_BLUE, "%");
    lv_obj_set_style_pad_bottom(s_r.pct, F_BIG->base_line - F_TITLE->base_line, 0);
    s_r.shown = -1;

    /* session countdown, then the 7 d reset right above the status word
     * (the bottom 7D row is gone -- user, 2026-10-05) */
    s_r.sub    = mk_centered(s, F_MONO, R_DIM, 0, LCD_W, G_CY + 9);
    s_r.w_rst  = mk_centered(s, F_MONO, R_DIM, 0, LCD_W, G_CY + 23);
    s_r.threat = mk_centered(s, F_SMALL, R_CAPTION, 0, LCD_W, G_CY + 36);
    lv_obj_set_style_text_letter_space(s_r.threat, 1, 0);

    lv_timer_create(breathe_cb, 100, NULL);
}

static void command_update(const usage_t *u, bool changed)
{
    char b[32];
    bool stale = !u->ok && u->have_data;

    if (changed && u->have_data) {
        int v = u->session_pct < 0 ? 0 : u->session_pct > 100 ? 100 : u->session_pct;
        if (v != s_r.lit_val) {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, s_r.meter);
            lv_anim_set_values(&a, s_r.lit_val, v);
            lv_anim_set_time(&a, 900);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
            lv_anim_set_exec_cb(&a, lit_anim_cb);
            lv_anim_start(&a);
            s_r.lit_val = v;
        }
        lv_obj_set_style_arc_color(s_r.glow, HEX(r2_level(u->session_pct)), LV_PART_INDICATOR);

        int w = u->weekly_pct < 0 ? 0 : u->weekly_pct > 100 ? 100 : u->weekly_pct;
        lv_obj_set_style_arc_color(s_r.week, HEX(stale ? R_STALE : r2_level(u->weekly_pct)), LV_PART_INDICATOR);
        int from = lv_arc_get_value(s_r.week);
        if (from != w) {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, s_r.week);
            lv_anim_set_values(&a, from, w);
            lv_anim_set_time(&a, 900);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
            lv_anim_set_exec_cb(&a, arc_anim_cb);
            lv_anim_start(&a);
        }

        if (u->session_pct != s_r.shown) {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, s_r.num);
            lv_anim_set_values(&a, s_r.shown < 0 ? 0 : s_r.shown, u->session_pct);
            lv_anim_set_time(&a, 900);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
            lv_anim_set_exec_cb(&a, num_anim_cb);
            lv_anim_start(&a);
            s_r.shown = u->session_pct;
        }
        bool wrap = u->session_pct >= WRAP_PCT;
        set_color(s_r.num, HEX(stale ? R_STALE : wrap ? R_RED : R_TEXT));
        set_color(s_r.pct, HEX(r2_level(u->session_pct)));

        int worst = u->session_pct > u->weekly_pct ? u->session_pct : u->weekly_pct;
        s_r.level = worst >= 100 ? 3 : worst >= USAGE_RED_PCT ? 2 : worst >= USAGE_AMBER_PCT ? 1 : 0;
        static const char *const word[] = { "NOMINAL", "ELEVATED", "CRITICAL", "LOCKED" };
        static const uint32_t wcol[] = { R_CAPTION, R_PALE, R_RED, R_RED };
        if (wrap && !stale && s_r.level < 3) {
            /* <100 % on both: tell me to wrap up, in the ring's red */
            snprintf(b, sizeof(b), "WRAP UP %d%%", u->session_pct);
            set_text(s_r.threat, b);
            set_color(s_r.threat, HEX(R_RED));
            lv_obj_set_style_text_letter_space(s_r.threat, 0, 0);
        } else {
            set_text(s_r.threat, stale ? "STALE" : word[s_r.level]);
            set_color(s_r.threat, HEX(stale ? R_STALE : wcol[s_r.level]));
            lv_obj_set_style_text_letter_space(s_r.threat, 1, 0);
        }
    }
    if (!u->have_data) {
        set_text(s_r.threat, u->err == POLL_OK ? "ACQUIRING" : "NO SIGNAL");
        set_color(s_r.threat, HEX(u->err == POLL_OK ? R_CAPTION : R_RED));
    }

    /* per second: countdowns, or what's wrong */
    if (!u->ok && (u->err == POLL_AUTH || !u->have_data)) {
        bool waiting = u->err == POLL_OK;   /* no poll finished yet */
        set_text(s_r.sub, waiting ? "loading" : u->err == POLL_AUTH ? "token?" : "offline");
        set_color(s_r.sub, HEX(waiting ? R_DIM : R_RED));
    } else if (!u->ok) {
        set_text(s_r.sub, usage_err_str(u->err));
        set_color(s_r.sub, HEX(R_PALE));
    } else {
        countdown(b, sizeof(b), u->session_reset);
        set_text(s_r.sub, b);
        set_color(s_r.sub, HEX(R_DIM));
    }
    if (u->have_data) {
        countdown(b, sizeof(b), u->weekly_reset);
        set_text(s_r.w_rst, b);
    }
}

/* ------------------------------------------------------------------ */
/* Page 2: PACE -- each window as a targeting tape                      */
/* ------------------------------------------------------------------ */
typedef struct {
    lv_obj_t *val, *verdict, *reset, *burn, *limit;
    tape_t tape;
} pacecard_t;
static pacecard_t s_pc[2];

static void pace_card(lv_obj_t *s, pacecard_t *c, int y, const char *title)
{
    lv_obj_t *p = panel(s, 2, y, LCD_W - 4, 54, title);
    c->verdict = mk_label(p, F_MONO, R_DIM, "");
    lv_obj_align(c->verdict, LV_ALIGN_TOP_RIGHT, -4, 3);
    c->val = mk_label(p, F_NUM, R_TEXT, "--");
    lv_obj_set_pos(c->val, 4, 17);
    c->reset = mk_label(p, F_MONO, R_DIM, "--");
    lv_obj_align(c->reset, LV_ALIGN_TOP_RIGHT, -4, 18);
    tape_build(p, &c->tape, 4, 32, LCD_W - 4 - 8, 5, 23);
    c->burn = mk_label(p, F_MONO, R_DIM, "");
    lv_obj_set_pos(c->burn, 4, 40);
    c->limit = mk_label(p, F_MONO, R_DIM, "");
    lv_obj_align(c->limit, LV_ALIGN_TOP_RIGHT, -4, 40);
}

static void pace_build(lv_obj_t *s)
{
    pace_card(s, &s_pc[0], 16, "5H");
    pace_card(s, &s_pc[1], 73, "7D");
}

static void pace_card_update(pacecard_t *c, int pct, time_t reset, long window, long unit_s,
                             char unit, bool have, bool changed)
{
    char b[32];
    if (!have) return;
    pace_t p = pace_of(pct, reset, window, unit_s);
    if (changed) {
        snprintf(b, sizeof(b), "%d%%", pct);
        set_text(c->val, b);
        set_color(c->val, HEX(pct >= USAGE_AMBER_PCT ? r2_level(pct) : R_TEXT));
    }
    tape_set(&c->tape, pct, r2_level(pct), p.known ? p.even : -1);
    uint32_t vc;
    verdict(b, sizeof(b), &p, &vc);
    set_text(c->verdict, b);
    set_color(c->verdict, HEX(vc));
    if (p.known) {
        snprintf(b, sizeof(b), "%ld.%ld%%/%c", p.rate_x100 / 100, (p.rate_x100 % 100) / 10, unit);
        set_text(c->burn, b);
        if (p.limit_s < 0) { set_text(c->limit, "CLEAR"); set_color(c->limit, HEX(R_CAPTION)); }
        else if (p.limit_s == 0) { set_text(c->limit, "LOCKED"); set_color(c->limit, HEX(R_RED)); }
        else {
            char d[16];
            fmt_dur(d, sizeof(d), p.limit_s);
            snprintf(b, sizeof(b), "LIM %s", d);
            set_text(c->limit, b);
            set_color(c->limit, HEX(R_RED));
        }
    } else {
        set_text(c->burn, "");
        set_text(c->limit, "");
    }
    countdown(b, sizeof(b), reset);
    set_text(c->reset, b);
}

static void pace_update(const usage_t *u, bool changed)
{
    pace_card_update(&s_pc[0], u->session_pct, u->session_reset, WINDOW_5H_S, 3600, 'h', u->have_data, changed);
    pace_card_update(&s_pc[1], u->weekly_pct, u->weekly_reset, WINDOW_7D_S, 86400, 'd', u->have_data, changed);
}

/* ------------------------------------------------------------------ */
/* Page 3: TREND -- the last 5 h, the cap, and where it ends           */
/* ------------------------------------------------------------------ */
static struct {
    lv_obj_t *chart, *note, *t, *now, *at_reset, *limit;
    lv_chart_series_t *ss, *sw;
} s_t;

static lv_obj_t *stat_tile(lv_obj_t *s, int x, int w, const char *cap)
{
    lv_obj_t *p = panel(s, x, 103, w, 24, NULL);
    lv_obj_set_pos(mk_label(p, F_MONO, R_CAPTION, cap), 4, 1);
    lv_obj_t *v = mk_label(p, F_MONO, R_TEXT, "--");
    lv_obj_align(v, LV_ALIGN_TOP_RIGHT, -3, 11);
    return v;
}

static void trend_build(lv_obj_t *s)
{
    mk_box(s, 3, 22, 8, 2, R_BLUE, 0);
    caption(s, "5H", 14, 15);
    mk_box(s, 40, 22, 8, 2, R_FRAME, 0);
    caption(s, "7D", 51, 15);
    s_t.t = mk_label(s, F_MONO, R_DIM, "");
    lv_obj_align(s_t.t, LV_ALIGN_TOP_RIGHT, -3, 17);

    lv_obj_t *p = panel(s, 2, 31, LCD_W - 4, 58, NULL);
    s_t.chart = lv_chart_create(p);
    lv_obj_remove_style_all(s_t.chart);
    lv_obj_set_pos(s_t.chart, 2, 3);
    lv_obj_set_size(s_t.chart, LCD_W - 4 - 4, 52);
    lv_chart_set_type(s_t.chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_t.chart, HIST_N + 1);
    lv_chart_set_range(s_t.chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_div_line_count(s_t.chart, 5, 0);       /* 100 = cap, 25/50/75 grid, 0 hidden */
    lv_obj_set_style_line_color(s_t.chart, HEX(R_OFF), LV_PART_MAIN);
    lv_obj_set_style_line_width(s_t.chart, 1, LV_PART_MAIN);
    lv_obj_set_style_line_width(s_t.chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_line_rounded(s_t.chart, true, LV_PART_ITEMS);
    lv_obj_set_style_size(s_t.chart, 0, LV_PART_INDICATOR);
    s_t.sw = lv_chart_add_series(s_t.chart, HEX(R_FRAME), LV_CHART_AXIS_PRIMARY_Y);
    s_t.ss = lv_chart_add_series(s_t.chart, HEX(R_BLUE), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(s_t.chart, s_t.sw, LV_CHART_POINT_NONE);
    lv_chart_set_all_value(s_t.chart, s_t.ss, LV_CHART_POINT_NONE);
    lv_obj_add_event_cb(s_t.chart, chart_draw_cb, LV_EVENT_DRAW_PART_BEGIN, s_t.ss);
    lv_obj_clear_flag(s_t.chart, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    s_t.note = mk_label(p, F_MONO, R_CAPTION, "collecting...");
    lv_obj_center(s_t.note);

    caption(s, "-5H", 2, 89);
    lv_obj_t *l = caption(s, "NOW", 0, 89);
    lv_obj_align(l, LV_ALIGN_TOP_RIGHT, -2, 89);

    s_t.now      = stat_tile(s, 2, 37, "NOW");
    s_t.at_reset = stat_tile(s, 41, 37, "PROJ");
    s_t.limit    = stat_tile(s, 80, 46, "LIMIT");
}

static void trend_update(const usage_t *u, bool changed)
{
    char b[24];
    if (changed) {
        usage_hist_t h;
        usage_hist_get(&h);
        /* history + the live value as the right-most point */
        int n = h.count + (u->have_data ? 1 : 0);
        for (int i = 0; i < HIST_N + 1; i++) {
            int k = i - (HIST_N + 1 - n);
            lv_coord_t vs = LV_CHART_POINT_NONE, vw = LV_CHART_POINT_NONE;
            if (k >= 0 && k < h.count) { vs = h.s[k]; vw = h.w[k]; }
            else if (k == h.count && u->have_data) {
                vs = u->session_pct > 100 ? 100 : u->session_pct;
                vw = u->weekly_pct > 100 ? 100 : u->weekly_pct;
            }
            lv_chart_set_value_by_id(s_t.chart, s_t.ss, i, vs);
            lv_chart_set_value_by_id(s_t.chart, s_t.sw, i, vw);
        }
        lv_chart_refresh(s_t.chart);
        if (n >= 2) lv_obj_add_flag(s_t.note, LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_clear_flag(s_t.note, LV_OBJ_FLAG_HIDDEN);
    }
    countdown(b, sizeof(b), u->session_reset);
    set_text(s_t.t, u->have_data ? b : "");
    if (!u->have_data) return;
    snprintf(b, sizeof(b), "%d%%", u->session_pct);
    set_text(s_t.now, b);
    set_color(s_t.now, HEX(u->session_pct >= USAGE_AMBER_PCT ? r2_level(u->session_pct) : R_TEXT));

    /* projection of the session window at the current burn rate */
    long el;
    if (window_elapsed(u->session_reset, WINDOW_5H_S, &el) && el >= WINDOW_5H_S / 10 && u->session_pct > 0) {
        long proj = (long)u->session_pct * WINDOW_5H_S / el;
        if (proj > 999) proj = 999;
        snprintf(b, sizeof(b), "%ld%%", proj);
        set_text(s_t.at_reset, b);
        set_color(s_t.at_reset, HEX(proj >= USAGE_AMBER_PCT ? r2_level((int)proj) : R_TEXT));
        long to_full = (long)(100 - u->session_pct) * el / u->session_pct;
        if (u->session_pct >= 100) { set_text(s_t.limit, "LOCK"); set_color(s_t.limit, HEX(R_RED)); }
        else if (to_full < WINDOW_5H_S - el) {
            settings_t c;
            settings_get(&c);
            fmt_clock(b, sizeof(b), time(NULL) + to_full, c.h24);
            set_text(s_t.limit, b);
            set_color(s_t.limit, HEX(R_RED));
        } else {
            set_text(s_t.limit, "CLEAR");
            set_color(s_t.limit, HEX(R_CAPTION));
        }
    } else {
        set_text(s_t.at_reset, "--");
        set_text(s_t.limit, "--");
        set_color(s_t.limit, HEX(R_TEXT));
    }
}

/* ------------------------------------------------------------------ */
/* Page 4: CLAWD -- a hologram on R2's holo-pad                        */
/* ------------------------------------------------------------------ */
static struct {
    lv_obj_t *clawd, *mood, *sub;
    uint32_t  resets_seen;
    int64_t   dance_until;
} s_c;

static void holo_corner(lv_obj_t *s, int x, int y, int dx, int dy)
{
    const int L = 8, T = 2;
    mk_box(s, dx > 0 ? x : x - L + 1, dy > 0 ? y : y - T + 1, L, T, R_BLUE, 0);
    mk_box(s, dx > 0 ? x : x - T + 1, dy > 0 ? y : y - L + 1, T, L, R_BLUE, 0);
}

static void clawd_page_build(lv_obj_t *s)
{
    holo_corner(s, 19, 16, 1, 1);
    holo_corner(s, 108, 16, -1, 1);
    holo_corner(s, 19, 97, 1, -1);
    holo_corner(s, 108, 97, -1, -1);
    s_c.clawd = clawd_create(s, 4, HEX(C_BG));          /* 80 px, buffer in PSRAM */
    if (s_c.clawd) {
        lv_obj_set_pos(s_c.clawd, 24, 17);
        clawd_set_holo(s_c.clawd, HEX(R_HOLO));
    }
    for (int y = 21; y < 97; y += 6) {                  /* holo scan lines */
        lv_obj_t *l = mk_box(s, 24, y, 80, 1, R_HOLO, 0);
        lv_obj_set_style_bg_opa(l, 40, 0);
    }
    s_c.mood = mk_centered(s, F_TITLE, R_TEXT, 0, LCD_W, 99);
    s_c.sub  = mk_centered(s, F_MONO, R_DIM, 0, LCD_W, 115);
}

static void clawd_page_update(const usage_t *u)
{
    int64_t now_us = esp_timer_get_time();
    if (u->resets != s_c.resets_seen) {
        if (s_c.resets_seen != 0 || u->resets > 0) s_c.dance_until = now_us + (int64_t)DANCE_S * 1000000;
        s_c.resets_seen = u->resets;
    }
    int worst = u->session_pct > u->weekly_pct ? u->session_pct : u->weekly_pct;
    char sub[32];
    snprintf(sub, sizeof(sub), "5h %d%%  7d %d%%", u->session_pct, u->weekly_pct);
    const char *mood;
    clawd_anim_id_t anim;

    if (!u->ok && u->err == POLL_AUTH) {
        anim = CLAWD_LOOK_AROUND; mood = "Need a token";
        snprintf(sub, sizeof(sub), "waiting for the Mac");
    } else if (!u->ok && u->err == POLL_NET) {
        anim = CLAWD_LOOK_AROUND; mood = "Offline";
        snprintf(sub, sizeof(sub), "retrying...");
    } else if (!u->have_data) {
        anim = CLAWD_THINK; mood = "Checking...";
        sub[0] = '\0';
    } else if (now_us < s_c.dance_until) {
        anim = CLAWD_DANCE_BOUNCE; mood = "Fresh window!";
    } else if (worst >= 100) {
        anim = CLAWD_SLEEP; mood = "Rate limited";
        char d[12];
        fmt_reset(d, sizeof(d), u->session_pct >= 100 ? u->session_reset : u->weekly_reset);
        snprintf(sub, sizeof(sub), "back in %s", d);
    } else if (worst >= USAGE_RED_PCT) {
        anim = CLAWD_SURPRISE; mood = "Whoa there";
    } else if (worst >= USAGE_AMBER_PCT) {
        anim = CLAWD_THINK; mood = "Pace yourself";
    } else if (worst >= 25) {
        anim = CLAWD_CODING; mood = "Cooking";
    } else {
        anim = CLAWD_DANCE_SWAY; mood = "All clear";
    }
    if (s_c.clawd) clawd_play(s_c.clawd, anim);
    set_text(s_c.mood, mood);
    set_color(s_c.mood, HEX(worst >= USAGE_AMBER_PCT ? r2_level(worst) : R_TEXT));
    set_text(s_c.sub, sub);
}

/* ------------------------------------------------------------------ */
/* Page 5: SYSTEMS -- readouts, dashboard QR, signal                    */
/* ------------------------------------------------------------------ */
enum { Y_NET, Y_BAT, Y_MEM, Y_CPU, Y_LINK, Y_TOK, Y_UP, Y_FW, Y_N };
static const char *const ROW_CAP[Y_N] = { "NET", "BAT", "MEM", "CPU", "API", "TOK", "UP", "FW" };
static struct {
    lv_obj_t *cap[Y_N], *val[Y_N], *bars[4], *ip, *qr;
    char qr_url[32];
} s_y;

static void systems_build(lv_obj_t *s)
{
    lv_obj_t *p = panel(s, 2, 16, 68, 95, NULL);
    for (int i = 0; i < Y_N; i++) {
        s_y.cap[i] = mk_label(p, F_MONO, R_CAPTION, ROW_CAP[i]);
        lv_obj_set_pos(s_y.cap[i], 4, 2 + i * 11);
        s_y.val[i] = mk_label(p, F_MONO, R_TEXT, "--");
        lv_obj_align(s_y.val[i], LV_ALIGN_TOP_RIGHT, -4, 2 + i * 11);
    }

    s_y.qr = lv_qrcode_create(s, 50, HEX(C_BG), HEX(R_TEXT));
    lv_obj_set_style_border_color(s_y.qr, HEX(R_TEXT), 0);
    lv_obj_set_style_border_width(s_y.qr, 2, 0);
    lv_obj_set_pos(s_y.qr, 72, 16);
    lv_obj_add_flag(s_y.qr, LV_OBJ_FLAG_HIDDEN);

    caption(s, "SIGNAL", 74, 72);
    for (int k = 0; k < 4; k++) s_y.bars[k] = mk_box(s, 84 + k * 7, 106 - (4 + k * 4), 5, 4 + k * 4, R_OFF, 0);

    caption(s, "IP", 2, 113);
    s_y.ip = mk_label(s, F_MONO, R_TEXT, "--");
    lv_obj_set_pos(s_y.ip, 20, 115);
}

static void systems_update(const usage_t *u)
{
    char b[48];
    int rssi = g_sys.rssi;
    if (g_sys.wifi_up && rssi) snprintf(b, sizeof(b), "%ddB", rssi); else snprintf(b, sizeof(b), "--");
    set_text(s_y.val[Y_NET], b);
    int bars = !g_sys.wifi_up || !rssi ? 0 : rssi > -55 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : 1;
    for (int k = 0; k < 4; k++)
        lv_obj_set_style_bg_color(s_y.bars[k], HEX(k < bars ? R_BLUE : R_OFF), 0);

    if (g_sys.batt_pct >= 0) snprintf(b, sizeof(b), "%d%%%s", g_sys.batt_pct, g_sys.ext_power ? "+" : "");
    else                     snprintf(b, sizeof(b), "--");
    set_text(s_y.val[Y_BAT], b);

    /* internal RAM: the budget that matters here */
    snprintf(b, sizeof(b), "%uKB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    set_text(s_y.val[Y_MEM], b);

    int t = power_temp_c();
    if (t != INT32_MIN) snprintf(b, sizeof(b), "%dC", t); else snprintf(b, sizeof(b), "--");
    set_text(s_y.val[Y_CPU], b);
    set_color(s_y.val[Y_CPU], HEX(t != INT32_MIN && t >= 70 ? R_RED : R_TEXT));

    set_text(s_y.cap[Y_LINK], u->src == SRC_HEADERS ? "HDR" : "API");
    if (u->last_ok_us) {
        long age = (long)((esp_timer_get_time() - u->last_ok_us) / 1000000);
        if (age < 60) snprintf(b, sizeof(b), "%lds", age);
        else          fmt_dur(b, sizeof(b), age);
        set_color(s_y.val[Y_LINK], HEX(!u->ok || age > 300 ? R_RED : R_TEXT));
    } else {
        snprintf(b, sizeof(b), "--");
    }
    set_text(s_y.val[Y_LINK], b);

    bool rejected = !u->ok && u->err == POLL_AUTH;
    set_text(s_y.val[Y_TOK], rejected ? "NO" : u->ok ? "OK" : "--");
    set_color(s_y.val[Y_TOK], HEX(rejected ? R_RED : R_TEXT));

    fmt_dur(b, sizeof(b), (long)(esp_timer_get_time() / 1000000));
    set_text(s_y.val[Y_UP], b);
    const esp_app_desc_t *app = esp_app_get_description();
    set_text(s_y.val[Y_FW], app ? app->version : "?");

    set_text(s_y.ip, g_sys.ip[0] ? g_sys.ip : "--");
    if (g_sys.ip[0]) {
        char url[32];
        snprintf(url, sizeof(url), "http://%s/", g_sys.ip);
        if (strcmp(url, s_y.qr_url) != 0) {
            strcpy(s_y.qr_url, url);
            lv_qrcode_update(s_y.qr, url, strlen(url));
            lv_obj_clear_flag(s_y.qr, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Splash + setup                                                      */
/* ------------------------------------------------------------------ */
static struct { lv_obj_t *scr, *clawd, *status; } s_s;

static lv_obj_t *mk_row(lv_obj_t *s, int x, int y)
{
    lv_obj_t *l = mk_label(s, F_MONO, C_TEXT, "");
    lv_label_set_recolor(l, true);
    lv_obj_set_pos(l, x, y);
    return l;
}

static void splash_build(void)
{
    s_s.scr = mk_screen();
    s_s.clawd = clawd_create(s_s.scr, 4, HEX(C_BG));
    if (s_s.clawd) {
        lv_obj_align(s_s.clawd, LV_ALIGN_TOP_MID, 0, 6);
        clawd_play(s_s.clawd, CLAWD_R2_IDLE);     /* R2 on the splash (user, 2026-10-05) */
    }
    lv_obj_t *t = mk_label(s_s.scr, F_TITLE, C_TEXT, "Claude Meter");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 90);
    s_s.status = mk_label(s_s.scr, F_MONO, C_FAINT, "starting");
    lv_obj_align(s_s.status, LV_ALIGN_TOP_MID, 0, 110);
    lv_scr_load(s_s.scr);
}

static void splash_update(void)
{
    const char *t = "starting";
    switch (g_sys.boot) {
    case BOOT_START: t = "starting";        break;
    case BOOT_WIFI:  t = "joining wi-fi";   break;
    case BOOT_TIME:  t = "syncing clock";   break;
    case BOOT_FETCH: t = "fetching usage";  break;
    case BOOT_READY: t = "ready";           break;
    case BOOT_SETUP: t = "setup";           break;
    }
    set_text(s_s.status, t);
    lv_obj_align(s_s.status, LV_ALIGN_TOP_MID, 0, 110);
    if (s_s.clawd)
        clawd_play(s_s.clawd, g_sys.boot >= BOOT_FETCH ? CLAWD_R2_THINK : CLAWD_R2_IDLE);
}

static void setup_show(void)
{
    lv_obj_t *s = mk_screen();
    lv_obj_t *t = mk_label(s, F_TITLE, C_TEXT, "Wi-Fi setup");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 2);

    char join[96];
    snprintf(join, sizeof(join), "WIFI:T:WPA;S:%s;P:%s;;", g_sys.ap_ssid, net_ap_password());
    lv_obj_t *qr = lv_qrcode_create(s, 62, HEX(C_BG), HEX(C_TEXT));
    lv_qrcode_update(qr, join, strlen(join));
    lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 21);

    char b[64];
    lv_obj_t *l = mk_row(s, 4, 87);
    snprintf(b, sizeof(b), "#6B6A64 NET# %s", g_sys.ap_ssid);
    lv_label_set_text(l, b);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(l, 124);
    l = mk_row(s, 4, 100);
    snprintf(b, sizeof(b), "#6B6A64 PASS# %s", net_ap_password());
    lv_label_set_text(l, b);
    l = mk_label(s, F_MONO, C_FAINT, "then open 192.168.4.1");
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 114);

    lv_scr_load_anim(s, LV_SCR_LOAD_ANIM_FADE_ON, 300, 0, true);
    s_mode = MODE_SETUP;
}

/* ------------------------------------------------------------------ */
/* Pages lifecycle                                                     */
/* ------------------------------------------------------------------ */
static void pages_build(void)
{
    for (int i = 0; i < PAGE_COUNT; i++) s_scr[i] = mk_screen();
    command_build(s_scr[PAGE_RINGS]);
    pace_build(s_scr[PAGE_PACE]);
    trend_build(s_scr[PAGE_TREND]);
    clawd_page_build(s_scr[PAGE_CLAWD]);
    systems_build(s_scr[PAGE_SYSTEM]);
    sb_build();
    lb_build();
}

static void go_page(page_t p, lv_scr_load_anim_t anim)
{
    s_page = p;
    s_page_t0 = esp_timer_get_time();
    lv_scr_load_anim(s_scr[p], anim, 260, 0, false);
    sb_set_page(p);
}

static void enter_pages(void)
{
    pages_build();
    s_mode = MODE_PAGES;
    s_seq = UINT32_MAX;                     /* force a full refresh */
    s_page = PAGE_RINGS;
    s_page_t0 = esp_timer_get_time();
    sb_set_page(PAGE_RINGS);
    lv_obj_clear_flag(s_sb.bar, LV_OBJ_FLAG_HIDDEN);
    lv_scr_load_anim(s_scr[PAGE_RINGS], LV_SCR_LOAD_ANIM_FADE_ON, 400, 0, true);  /* frees splash */
    s_s.clawd = NULL;
}

/* ------------------------------------------------------------------ */
/* Public                                                              */
/* ------------------------------------------------------------------ */
void ui_request(ui_req_t r) { s_req = r; }
void ui_input(void) { s_input_us = esp_timer_get_time(); }
int  ui_page(void) { return s_mode == MODE_PAGES ? (int)s_page : -1; }

void ui_init(void)
{
    s_splash_t0 = esp_timer_get_time();
    splash_build();
}

void ui_button_short(void)
{
    if (s_mode != MODE_PAGES) return;
    go_page((s_page + 1) % PAGE_COUNT, LV_SCR_LOAD_ANIM_MOVE_LEFT);
}

void ui_button_prev(void)
{
    if (s_mode != MODE_PAGES) return;
    go_page((s_page + PAGE_COUNT - 1) % PAGE_COUNT, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
}

void ui_goodbye(void)
{
    lv_obj_t *s = mk_screen();
    lv_obj_t *c = clawd_create(s, 4, HEX(C_BG));
    if (c) { lv_obj_align(c, LV_ALIGN_TOP_MID, 0, 14); clawd_play(c, CLAWD_SLEEP); }
    lv_obj_t *t = mk_label(s, F_TITLE, C_TEXT, "Goodbye");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 100);
    if (s_sb.bar) lv_obj_add_flag(s_sb.bar, LV_OBJ_FLAG_HIDDEN);
    lv_scr_load(s);
    lv_refr_now(NULL);
}

void ui_button_long(void)
{
    if (s_mode != MODE_PAGES) return;
    if (s_page != PAGE_RINGS) go_page(PAGE_RINGS, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
    lv_obj_clear_flag(s_sb.spin, LV_OBJ_FLAG_HIDDEN);   /* instant feedback */
    sb_layout();
}

void ui_tick(void)
{
    int64_t now = esp_timer_get_time();

    if (s_mode == MODE_SPLASH) {
        splash_update();
        int64_t ms = (now - s_splash_t0) / 1000;
        if (g_sys.boot == BOOT_SETUP && ms > 800) {
            setup_show();
        } else if (ms > SPLASH_MIN_MS &&
                   (g_sys.boot == BOOT_READY ||
                    (ms > SPLASH_MAX_MS && g_sys.boot >= BOOT_FETCH))) {
            enter_pages();
        }
        s_req = UI_REQ_NONE;            /* page requests mean nothing before the pages exist */
        return;
    }
    if (s_mode != MODE_PAGES) { s_req = UI_REQ_NONE; return; }

    lb_update();
    ui_req_t req = s_req;
    if (req != UI_REQ_NONE) {
        s_req = UI_REQ_NONE;
        if (req >= UI_REQ_PAGE_RINGS && req <= UI_REQ_PAGE_SYSTEM) {
            page_t p = (page_t)(req - UI_REQ_PAGE_RINGS);
            if (p != s_page) go_page(p, p > s_page ? LV_SCR_LOAD_ANIM_MOVE_LEFT : LV_SCR_LOAD_ANIM_MOVE_RIGHT);
        } else if (req == UI_REQ_NEXT) {
            ui_button_short();
        } else if (req == UI_REQ_PREV) {
            ui_button_prev();
        } else if (req == UI_REQ_SCREEN_OFF) {
            screen_sleep();
        }
    }

    usage_t u;
    usage_get(&u);
    bool changed = u.seq != s_seq;
    bool second = now - s_last_sec_us >= 1000000;
    if (!changed && !second) return;
    s_seq = u.seq;
    if (second) s_last_sec_us = now;

    sb_update(u.polling);
    command_update(&u, changed);
    pace_update(&u, changed);
    trend_update(&u, changed);
    clawd_page_update(&u);
    systems_update(&u);

    if (s_page != PAGE_RINGS && s_page != PAGE_CLAWD &&
        now - s_page_t0 > (int64_t)PAGE_RETURN_S * 1000000) {
        go_page(PAGE_RINGS, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
    }
}
