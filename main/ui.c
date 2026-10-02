/*
 * ui.c  --  Claude Meter v2 interface for a 128x128 ST7735.
 *
 *   splash  : Clawd + boot stage, until the first poll lands
 *   setup   : Wi-Fi setup AP: join-QR + password (no network)
 *   pages   : RINGS -> PACE -> TREND -> CLAWD -> SYSTEM (short press cycles)
 *   top bar : clock . page dots . sync spinner, Wi-Fi, charge, battery
 *             (on lv_layer_top so it stays put while pages slide)
 *
 * Palette follows the Clawdmeter HY3 theme (Anthropic colors).
 * LVGL is single-threaded: everything here runs on the main loop.
 */
#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "lvgl.h"

#include "app.h"
#include "board.h"
#include "clawd.h"
#include "net.h"
#include "power.h"
#include "settings.h"
#include "usage.h"

LV_FONT_DECLARE(font_jbm_bold_30);
LV_FONT_DECLARE(font_jbm_bold_16);
LV_FONT_DECLARE(font_jbm_medium_11);
LV_FONT_DECLARE(font_styrene_12);
LV_FONT_DECLARE(font_styrene_14);

#define F_BIG    (&font_jbm_bold_30)
#define F_NUM    (&font_jbm_bold_16)
#define F_MONO   (&font_jbm_medium_11)
#define F_TEXT   (&font_styrene_12)
#define F_TITLE  (&font_styrene_14)
#define F_ICON   (&lv_font_montserrat_10)

/* ------------------------------------------------------------------ */
/* Palette                                                             */
/* ------------------------------------------------------------------ */
#define C_BG      0x000000
#define C_CARD    0x1A1917
#define C_TRACK   0x2C2B27
#define C_TEXT    0xFAF9F5
#define C_DIM     0xB0AEA5
#define C_FAINT   0x6B6A64
#define C_ACCENT  0xD97757      /* Claude coral */
#define C_OK      0x82B56C      /* sage   */
#define C_WARN    0xE9A23B      /* amber  */
#define C_HIGH    0xE5604E      /* red    */

#define HEX(c)    lv_color_hex(c)

typedef enum { PAGE_RINGS, PAGE_PACE, PAGE_TREND, PAGE_CLAWD, PAGE_SYSTEM, PAGE_COUNT } page_t;
typedef enum { MODE_SPLASH, MODE_PAGES, MODE_SETUP } ui_mode_t;

#define SPLASH_MIN_MS     1800
#define SPLASH_MAX_MS     30000
#define PAGE_RETURN_S     60        /* non-home pages fall back to RINGS */
#define DANCE_S           300       /* Clawd celebrates a reset this long */

static ui_mode_t s_mode = MODE_SPLASH;
static int64_t   s_splash_t0;
static lv_obj_t *s_scr[PAGE_COUNT];
static page_t    s_page;
static int64_t   s_page_t0;
static uint32_t  s_seq = UINT32_MAX;
static int64_t   s_last_sec_us;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */
static lv_color_t level_color(int pct)
{
    if (pct >= USAGE_RED_PCT)   return HEX(C_HIGH);
    if (pct >= USAGE_AMBER_PCT) return HEX(C_WARN);
    return HEX(C_OK);
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
/* Status bar (top layer)                                              */
/* ------------------------------------------------------------------ */
static struct {
    lv_obj_t *bar, *clock, *dots[PAGE_COUNT], *spin, *wifi, *bolt, *batt, *batt_fill, *nub;
} s_sb;

static void sb_build(void)
{
    lv_obj_t *top = lv_layer_top();
    s_sb.bar = mk_box(top, 0, 0, LCD_W, 14, C_BG, 0);

    s_sb.clock = mk_label(s_sb.bar, F_MONO, C_TEXT, "--:--");
    lv_obj_align(s_sb.clock, LV_ALIGN_LEFT_MID, 4, 0);

    lv_obj_t *dots = lv_obj_create(s_sb.bar);
    lv_obj_remove_style_all(dots);
    lv_obj_set_size(dots, LV_SIZE_CONTENT, 4);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(dots, 3, 0);
    lv_obj_align(dots, LV_ALIGN_CENTER, -2, 0);
    for (int i = 0; i < PAGE_COUNT; i++) {
        s_sb.dots[i] = mk_box(dots, 0, 0, 3, 3, C_FAINT, LV_RADIUS_CIRCLE);
    }

    /* right cluster, laid out right-to-left in sb_layout() */
    lv_obj_t *right = s_sb.bar;
    s_sb.spin = lv_spinner_create(right, 900, 90);
    lv_obj_remove_style_all(s_sb.spin);
    lv_obj_set_size(s_sb.spin, 9, 9);
    lv_obj_set_style_arc_width(s_sb.spin, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_sb.spin, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_sb.spin, 2, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_sb.spin, HEX(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_sb.spin, true, LV_PART_INDICATOR);
    lv_obj_add_flag(s_sb.spin, LV_OBJ_FLAG_HIDDEN);

    s_sb.wifi = mk_label(right, F_ICON, C_TEXT, LV_SYMBOL_WIFI);
    s_sb.bolt = mk_label(right, F_ICON, C_OK, LV_SYMBOL_CHARGE);

    /* battery: 15x8 body + 2x4 nub */
    s_sb.batt = lv_obj_create(right);
    lv_obj_remove_style_all(s_sb.batt);
    lv_obj_set_size(s_sb.batt, 17, 8);
    lv_obj_set_pos(s_sb.batt, LCD_W - 3 - 17, 3);
    lv_obj_t *body = lv_obj_create(s_sb.batt);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, 15, 8);
    lv_obj_set_style_border_width(body, 1, 0);
    lv_obj_set_style_border_color(body, HEX(C_DIM), 0);
    lv_obj_set_style_radius(body, 2, 0);
    s_sb.batt_fill = mk_box(s_sb.batt, 2, 2, 11, 4, C_TEXT, 1);
    s_sb.nub = mk_box(s_sb.batt, 15, 2, 2, 4, C_DIM, 1);

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
        lv_coord_t w = o == s_sb.bolt ? 7 : o == s_sb.wifi ? 13 : 9;
        x -= w;
        lv_obj_set_pos(o, x, o == s_sb.spin ? 2 : 1);
        x -= 3;
    }
}

static void sb_set_page(page_t p)
{
    for (int i = 0; i < PAGE_COUNT; i++) {
        bool on = (int)p == i;
        lv_obj_set_width(s_sb.dots[i], on ? 9 : 3);
        lv_obj_set_style_bg_color(s_sb.dots[i], HEX(on ? C_ACCENT : C_FAINT), 0);
    }
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
    uint32_t wc = !g_sys.wifi_up ? C_HIGH : (rssi && rssi < -78) ? C_WARN : C_TEXT;
    set_color(s_sb.wifi, HEX(wc));

    if (g_sys.ext_power) lv_obj_clear_flag(s_sb.bolt, LV_OBJ_FLAG_HIDDEN);
    else                 lv_obj_add_flag(s_sb.bolt, LV_OBJ_FLAG_HIDDEN);

    sb_layout();

    int pct = g_sys.batt_pct;
    int w = pct < 0 ? 0 : (pct * 11 + 50) / 100;
    if (w < 1) w = 1;
    lv_obj_set_width(s_sb.batt_fill, w);
    uint32_t fc = g_sys.ext_power ? C_OK : pct <= 15 ? C_HIGH : pct <= 30 ? C_WARN : C_TEXT;
    lv_obj_set_style_bg_color(s_sb.batt_fill, HEX(fc), 0);
}

/* ------------------------------------------------------------------ */
/* Page: RINGS                                                         */
/* ------------------------------------------------------------------ */
static struct {
    lv_obj_t *arc_s, *arc_w, *cap, *pair, *num, *pct, *sub, *w_cap, *w_val, *r_icon, *r_val;
    int shown;                  /* value currently displayed by the count-up */
} s_r;

static lv_obj_t *mk_ring(lv_obj_t *p, int size, int width)
{
    lv_obj_t *a = lv_arc_create(p);
    lv_obj_remove_style_all(a);
    lv_obj_set_size(a, size, size);
    lv_arc_set_rotation(a, 270);
    lv_arc_set_bg_angles(a, 0, 360);
    lv_arc_set_range(a, 0, 100);
    lv_arc_set_value(a, 0);
    lv_obj_set_style_arc_width(a, width, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, HEX(C_TRACK), LV_PART_MAIN);
    lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
    return a;
}

static void arc_anim_cb(void *arc, int32_t v) { lv_arc_set_value(arc, (int16_t)v); }

static void ring_set(lv_obj_t *arc, int pct, bool stale)
{
    lv_color_t c = level_color(pct);
    lv_obj_set_style_arc_color(arc, c, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_mix(c, HEX(C_BG), 58), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, stale ? LV_OPA_40 : LV_OPA_COVER, LV_PART_INDICATOR);

    int v = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    int from = lv_arc_get_value(arc);
    if (from == v) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, arc);
    lv_anim_set_values(&a, from, v);
    lv_anim_set_time(&a, 700);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, arc_anim_cb);
    lv_anim_start(&a);
}

static void num_anim_cb(void *var, int32_t v)
{
    char b[8];
    snprintf(b, sizeof(b), "%d", (int)v);
    lv_label_set_text(var, b);
}

static void rings_build(lv_obj_t *s)
{
    /* center (64,70): outer r56/w7, inner r47/w7, 2 px gap */
    s_r.arc_s = mk_ring(s, 112, 7);
    lv_obj_set_pos(s_r.arc_s, 8, 14);
    s_r.arc_w = mk_ring(s, 94, 7);
    lv_obj_set_pos(s_r.arc_w, 17, 23);

    s_r.cap = mk_label(s, F_MONO, C_FAINT, "SESSION");
    lv_obj_set_style_text_letter_space(s_r.cap, 1, 0);
    lv_obj_align(s_r.cap, LV_ALIGN_TOP_MID, 0, 43);

    /* "42" + "%" as one centered row; flex re-centers as digits change.
     * Bottom-aligned, then the % is lifted by the difference in the two
     * fonts' descent so the baselines line up. */
    s_r.pair = lv_obj_create(s);
    lv_obj_remove_style_all(s_r.pair);
    lv_obj_set_size(s_r.pair, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_r.pair, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_r.pair, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(s_r.pair, 1, 0);
    lv_obj_align(s_r.pair, LV_ALIGN_TOP_MID, 0, 50);
    s_r.num = mk_label(s_r.pair, F_BIG, C_TEXT, "--");
    s_r.pct = mk_label(s_r.pair, F_TITLE, C_DIM, "%");
    lv_obj_set_style_pad_bottom(s_r.pct,
        F_BIG->base_line - F_TITLE->base_line, 0);
    s_r.shown = -1;

    s_r.sub = mk_label(s, F_MONO, C_DIM, "");
    lv_obj_align(s_r.sub, LV_ALIGN_TOP_MID, 0, 88);

    /* corners: weekly value (left), weekly reset (right) */
    s_r.w_cap = mk_label(s, F_MONO, C_FAINT, "7D");
    lv_obj_set_pos(s_r.w_cap, 3, 103);
    s_r.w_val = mk_label(s, F_MONO, C_TEXT, "--");
    lv_obj_set_pos(s_r.w_val, 3, 115);
    s_r.r_icon = mk_label(s, F_ICON, C_FAINT, LV_SYMBOL_REFRESH);
    lv_obj_align(s_r.r_icon, LV_ALIGN_TOP_RIGHT, -4, 104);
    s_r.r_val = mk_label(s, F_MONO, C_DIM, "--");
    lv_obj_align(s_r.r_val, LV_ALIGN_TOP_RIGHT, -3, 115);
}

static void rings_update(const usage_t *u, bool data_changed)
{
    char b[24];
    bool stale = !u->ok && u->have_data;

    if (data_changed && u->have_data) {
        ring_set(s_r.arc_s, u->session_pct, stale);
        ring_set(s_r.arc_w, u->weekly_pct, stale);
        int target = u->session_pct;
        if (target != s_r.shown) {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, s_r.num);
            lv_anim_set_values(&a, s_r.shown < 0 ? 0 : s_r.shown, target);
            lv_anim_set_time(&a, 700);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
            lv_anim_set_exec_cb(&a, num_anim_cb);
            lv_anim_start(&a);
            s_r.shown = target;
        }
        snprintf(b, sizeof(b), "%d%%", u->weekly_pct);
        set_text(s_r.w_val, b);
        lv_obj_set_style_text_color(s_r.w_val, level_color(u->weekly_pct), 0);
        lv_obj_set_style_text_color(s_r.num, stale ? HEX(C_DIM) : HEX(C_TEXT), 0);
    }

    /* sub line: countdown, or what's wrong */
    if (!u->ok && (u->err == POLL_AUTH || !u->have_data)) {
        bool waiting = u->err == POLL_OK;   /* no poll finished yet */
        set_text(s_r.sub, waiting ? "loading" : u->err == POLL_AUTH ? "token?" : "offline");
        set_color(s_r.sub, HEX(waiting ? C_DIM : C_HIGH));
    } else if (!u->ok) {
        set_text(s_r.sub, usage_err_str(u->err));
        set_color(s_r.sub, HEX(C_WARN));
    } else {
        fmt_reset(b, sizeof(b), u->session_reset);
        set_text(s_r.sub, b);
        set_color(s_r.sub, HEX(C_DIM));
    }
    if (u->have_data) {
        fmt_reset(b, sizeof(b), u->weekly_reset);
        set_text(s_r.r_val, b);
    }
}

/* ------------------------------------------------------------------ */
/* Page: PACE                                                          */
/* ------------------------------------------------------------------ */
typedef struct {
    lv_obj_t *val, *bar, *mark, *reset, *pace;
} pace_card_t;
static pace_card_t s_p[2];

#define PACE_BAR_X  7
#define PACE_BAR_W  106

static void pace_card_build(lv_obj_t *s, pace_card_t *c, int y, const char *title)
{
    lv_obj_t *card = mk_box(s, 4, y, 120, 52, C_CARD, 8);
    lv_obj_t *t = mk_label(card, F_MONO, C_DIM, title);
    lv_obj_set_style_text_letter_space(t, 1, 0);
    lv_obj_set_pos(t, 7, 6);
    c->val = mk_label(card, F_NUM, C_TEXT, "--");
    lv_obj_align(c->val, LV_ALIGN_TOP_RIGHT, -7, 2);

    c->bar = lv_bar_create(card);
    lv_obj_remove_style_all(c->bar);
    lv_obj_set_pos(c->bar, PACE_BAR_X, 24);
    lv_obj_set_size(c->bar, PACE_BAR_W, 6);
    lv_bar_set_range(c->bar, 0, 100);
    lv_obj_set_style_bg_color(c->bar, HEX(C_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(c->bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(c->bar, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(c->bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(c->bar, 3, LV_PART_INDICATOR);
    lv_obj_set_style_anim_time(c->bar, 700, LV_PART_MAIN);

    /* "even pace" marker: where usage would be if spent evenly */
    c->mark = mk_box(card, PACE_BAR_X, 21, 2, 12, C_TEXT, 1);
    lv_obj_add_flag(c->mark, LV_OBJ_FLAG_HIDDEN);

    c->reset = mk_label(card, F_MONO, C_FAINT, "--");
    lv_obj_set_pos(c->reset, 7, 36);
    c->pace = mk_label(card, F_MONO, C_DIM, "");
    lv_obj_align(c->pace, LV_ALIGN_TOP_RIGHT, -7, 36);
}

static void pace_card_update(pace_card_t *c, int pct, time_t reset, long window,
                             bool have, bool changed)
{
    char b[24];
    if (!have) return;
    if (changed) {
        snprintf(b, sizeof(b), "%d%%", pct);
        set_text(c->val, b);
        set_color(c->val, level_color(pct));
        lv_obj_set_style_bg_color(c->bar, level_color(pct), LV_PART_INDICATOR);
        lv_bar_set_value(c->bar, pct > 100 ? 100 : pct, LV_ANIM_ON);
    }

    char r[12];
    fmt_reset(r, sizeof(r), reset);
    set_text(c->reset, r);

    long el;
    if (window_elapsed(reset, window, &el)) {
        int e = (int)(el * 100 / window);
        lv_obj_clear_flag(c->mark, LV_OBJ_FLAG_HIDDEN);
        lv_coord_t mx = PACE_BAR_X + e * (PACE_BAR_W - 2) / 100;
        if (lv_obj_get_x(c->mark) != mx) lv_obj_set_x(c->mark, mx);
        int d = pct - e;
        uint32_t col;
        if (d > 10)       { snprintf(b, sizeof(b), "+%d ahead", d);  col = d > 25 ? C_HIGH : C_WARN; }
        else if (d < -10) { snprintf(b, sizeof(b), "%d under", -d);  col = C_DIM; }
        else              { snprintf(b, sizeof(b), "on pace");       col = C_OK; }
        set_text(c->pace, b);
        set_color(c->pace, HEX(col));
    } else {
        lv_obj_add_flag(c->mark, LV_OBJ_FLAG_HIDDEN);
        set_text(c->pace, "");
    }
}

static void pace_build(lv_obj_t *s)
{
    pace_card_build(s, &s_p[0], 17, "5-HOUR");
    pace_card_build(s, &s_p[1], 73, "7-DAY");
}

static void pace_update(const usage_t *u, bool changed)
{
    pace_card_update(&s_p[0], u->session_pct, u->session_reset, WINDOW_5H_S, u->have_data, changed);
    pace_card_update(&s_p[1], u->weekly_pct, u->weekly_reset, WINDOW_7D_S, u->have_data, changed);
}

/* ------------------------------------------------------------------ */
/* Page: TREND                                                         */
/* ------------------------------------------------------------------ */
static struct {
    lv_obj_t *chart, *proj, *note;
    lv_chart_series_t *ser_s, *ser_w;
    int last_count;
} s_t;

/* Soft area fill under the session line (LVGL 8 chart has no native fill). */
static void chart_draw_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
    if (dsc->part != LV_PART_ITEMS || dsc->sub_part_ptr != s_t.ser_s) return;
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

static void legend(lv_obj_t *s, int x, uint32_t color, const char *txt)
{
    mk_box(s, x, 121, 8, 2, color, 1);
    lv_obj_t *l = mk_label(s, F_MONO, C_DIM, txt);
    lv_obj_set_pos(l, x + 11, 115);
}

static void trend_build(lv_obj_t *s)
{
    lv_obj_t *t = mk_label(s, F_MONO, C_DIM, "TREND");
    lv_obj_set_style_text_letter_space(t, 1, 0);
    lv_obj_set_pos(t, 4, 17);
    s_t.proj = mk_label(s, F_MONO, C_DIM, "");
    lv_obj_align(s_t.proj, LV_ALIGN_TOP_RIGHT, -4, 17);

    s_t.chart = lv_chart_create(s);
    lv_obj_remove_style_all(s_t.chart);
    lv_obj_set_pos(s_t.chart, 4, 32);
    lv_obj_set_size(s_t.chart, 120, 68);
    lv_chart_set_type(s_t.chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_t.chart, HIST_N + 1);
    lv_chart_set_range(s_t.chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_div_line_count(s_t.chart, 5, 0);
    lv_obj_set_style_line_color(s_t.chart, HEX(C_TRACK), LV_PART_MAIN);
    lv_obj_set_style_line_width(s_t.chart, 1, LV_PART_MAIN);
    lv_obj_set_style_line_width(s_t.chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_line_rounded(s_t.chart, true, LV_PART_ITEMS);
    lv_obj_set_style_size(s_t.chart, 0, LV_PART_INDICATOR);
    s_t.ser_w = lv_chart_add_series(s_t.chart, HEX(C_FAINT), LV_CHART_AXIS_PRIMARY_Y);
    s_t.ser_s = lv_chart_add_series(s_t.chart, HEX(C_ACCENT), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(s_t.chart, s_t.ser_w, LV_CHART_POINT_NONE);
    lv_chart_set_all_value(s_t.chart, s_t.ser_s, LV_CHART_POINT_NONE);
    lv_obj_add_event_cb(s_t.chart, chart_draw_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);

    s_t.note = mk_label(s, F_MONO, C_FAINT, "collecting...");
    lv_obj_align_to(s_t.note, s_t.chart, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *l = mk_label(s, F_MONO, C_FAINT, "-5h");
    lv_obj_set_pos(l, 4, 102);
    l = mk_label(s, F_MONO, C_FAINT, "now");
    lv_obj_align(l, LV_ALIGN_TOP_RIGHT, -4, 102);
    legend(s, 4, C_ACCENT, "5H");
    legend(s, 40, C_FAINT, "7D");
    s_t.last_count = -1;
}

static void trend_update(const usage_t *u)
{
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
        lv_chart_set_value_by_id(s_t.chart, s_t.ser_s, i, vs);
        lv_chart_set_value_by_id(s_t.chart, s_t.ser_w, i, vw);
    }
    lv_chart_refresh(s_t.chart);
    if (n >= 2) lv_obj_add_flag(s_t.note, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_clear_flag(s_t.note, LV_OBJ_FLAG_HIDDEN);

    /* projection of the session window at the current burn rate */
    char b[24];
    long el;
    uint32_t col = C_DIM;
    if (u->have_data && window_elapsed(u->session_reset, WINDOW_5H_S, &el) &&
        el >= WINDOW_5H_S / 10 && u->session_pct > 0) {
        long proj = (long)u->session_pct * WINDOW_5H_S / el;
        long to_full = (long)(100 - u->session_pct) * el / u->session_pct;
        long left = WINDOW_5H_S - el;
        if (u->session_pct < 100 && to_full < left) {
            settings_t c;
            settings_get(&c);
            char t[8];
            fmt_clock(t, sizeof(t), time(NULL) + to_full, c.h24);
            snprintf(b, sizeof(b), "max @%s", t);
            col = C_HIGH;
        } else {
            snprintf(b, sizeof(b), "proj %ld%%", proj > 999 ? 999 : proj);
            col = proj >= USAGE_RED_PCT ? C_WARN : C_OK;
        }
    } else {
        snprintf(b, sizeof(b), "proj --");
    }
    set_text(s_t.proj, b);
    set_color(s_t.proj, HEX(col));
}

/* ------------------------------------------------------------------ */
/* Page: CLAWD (mood)                                                  */
/* ------------------------------------------------------------------ */
static struct {
    lv_obj_t *clawd, *mood, *sub;
    uint32_t  resets_seen;
    int64_t   dance_until;
} s_c;

static void clawd_page_build(lv_obj_t *s)
{
    s_c.clawd = clawd_create(s, 4, HEX(C_BG));
    if (s_c.clawd) lv_obj_align(s_c.clawd, LV_ALIGN_TOP_MID, 0, 16);
    s_c.mood = mk_label(s, F_TITLE, C_TEXT, "");
    lv_obj_align(s_c.mood, LV_ALIGN_TOP_MID, 0, 96);
    s_c.sub = mk_label(s, F_MONO, C_DIM, "");
    lv_obj_align(s_c.sub, LV_ALIGN_TOP_MID, 0, 114);
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
    clawd_play(s_c.clawd, anim);
    set_text(s_c.mood, mood);
    set_text(s_c.sub, sub);
}

/* ------------------------------------------------------------------ */
/* Page: SYSTEM                                                        */
/* ------------------------------------------------------------------ */
static struct {
    lv_obj_t *rows[4], *ip, *src, *host, *qr;
    char qr_url[32];
} s_y;

static lv_obj_t *mk_row(lv_obj_t *s, int x, int y)
{
    lv_obj_t *l = mk_label(s, F_MONO, C_TEXT, "");
    lv_label_set_recolor(l, true);
    lv_obj_set_pos(l, x, y);
    return l;
}

static void system_build(lv_obj_t *s)
{
    lv_obj_t *t = mk_label(s, F_MONO, C_DIM, "DEVICE");
    lv_obj_set_style_text_letter_space(t, 1, 0);
    lv_obj_set_pos(t, 4, 17);
    for (int i = 0; i < 4; i++) s_y.rows[i] = mk_row(s, 4, 31 + i * 12);
    s_y.ip   = mk_row(s, 3, 84);
    s_y.src  = mk_row(s, 3, 98);
    s_y.host = mk_row(s, 3, 111);

    s_y.qr = lv_qrcode_create(s, 50, HEX(C_BG), HEX(C_TEXT));
    lv_obj_set_style_border_color(s_y.qr, HEX(C_TEXT), 0);
    lv_obj_set_style_border_width(s_y.qr, 2, 0);
    lv_obj_set_pos(s_y.qr, 72, 28);
    lv_obj_add_flag(s_y.qr, LV_OBJ_FLAG_HIDDEN);
}

#define K "#6B6A64 "    /* recolor prefix for row keys */

static void system_update(const usage_t *u)
{
    char b[48];
    int64_t up = esp_timer_get_time() / 1000000;

    if (g_sys.wifi_up && g_sys.rssi) snprintf(b, sizeof(b), K "NET# %ddB", g_sys.rssi);
    else                             snprintf(b, sizeof(b), K "NET# --");
    set_text(s_y.rows[0], b);
    if (g_sys.batt_pct >= 0)
        snprintf(b, sizeof(b), K "BAT# %d%%%s", g_sys.batt_pct, g_sys.ext_power ? "+" : "");
    else
        snprintf(b, sizeof(b), K "BAT# --");
    set_text(s_y.rows[1], b);
    char d[24];
    fmt_dur(d, sizeof(d), (long)up);
    snprintf(b, sizeof(b), K "UP#  %s", d);
    set_text(s_y.rows[2], b);
    const esp_app_desc_t *app = esp_app_get_description();
    snprintf(b, sizeof(b), K "FW#  %s", app ? app->version : "?");
    set_text(s_y.rows[3], b);

    snprintf(b, sizeof(b), K "IP# %s", g_sys.ip[0] ? g_sys.ip : "--");
    set_text(s_y.ip, b);
    if (u->last_ok_us) {
        long age = (long)((esp_timer_get_time() - u->last_ok_us) / 1000000);
        if (age < 60) snprintf(d, sizeof(d), "%ds", (int)age);
        else          fmt_dur(d, sizeof(d), age);
        snprintf(b, sizeof(b), K "SRC# %s, %s ago", u->src == SRC_HEADERS ? "hdr" : "api", d);
    } else {
        snprintf(b, sizeof(b), K "SRC# --");
    }
    set_text(s_y.src, b);
    settings_t c;
    settings_get(&c);
    snprintf(b, sizeof(b), K "TOK# %s", c.notify_ip[0] ? c.notify_ip : "ArcTrooper");
    set_text(s_y.host, b);

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

static void splash_build(void)
{
    s_s.scr = mk_screen();
    s_s.clawd = clawd_create(s_s.scr, 4, HEX(C_BG));
    if (s_s.clawd) {
        lv_obj_align(s_s.clawd, LV_ALIGN_TOP_MID, 0, 6);
        clawd_play(s_s.clawd, CLAWD_IDLE_BLINK);
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
        clawd_play(s_s.clawd, g_sys.boot >= BOOT_FETCH ? CLAWD_THINK : CLAWD_IDLE_BLINK);
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
    rings_build(s_scr[PAGE_RINGS]);
    pace_build(s_scr[PAGE_PACE]);
    trend_build(s_scr[PAGE_TREND]);
    clawd_page_build(s_scr[PAGE_CLAWD]);
    system_build(s_scr[PAGE_SYSTEM]);
    sb_build();
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
int ui_page(void) { return s_mode == MODE_PAGES ? (int)s_page : -1; }

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
        return;
    }
    if (s_mode != MODE_PAGES) return;

    usage_t u;
    usage_get(&u);
    bool changed = u.seq != s_seq;
    bool second = now - s_last_sec_us >= 1000000;
    if (!changed && !second) return;
    s_seq = u.seq;
    if (second) s_last_sec_us = now;

    sb_update(u.polling);
    rings_update(&u, changed);
    pace_update(&u, changed);
    clawd_page_update(&u);
    system_update(&u);
    if (changed) trend_update(&u);

    if (s_page != PAGE_RINGS && s_page != PAGE_CLAWD &&
        now - s_page_t0 > (int64_t)PAGE_RETURN_S * 1000000) {
        go_page(PAGE_RINGS, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
    }
}
