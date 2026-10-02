/*
 * clawd.c  --  Clawd pixel-art player. Each frame is 20x20 palette indices
 * (4 bits, packed); cells are blown up to scale x scale blocks directly in
 * the canvas buffer, and an lv_timer advances frames using each frame's own
 * hold time. Frames only advance while the screen is on.
 */
#include "clawd.h"

#include <stdlib.h>
#include "esp_heap_caps.h"

#include "power.h"

typedef struct {
    lv_obj_t       *canvas;
    lv_timer_t     *timer;
    lv_color_t     *buf;
    lv_color_t      bg;
    int             scale;
    clawd_anim_id_t anim;
    int             frame;
} clawd_t;

static void draw_frame(clawd_t *c)
{
    const clawd_anim_t *a = &clawd_anims[c->anim];
    const uint8_t *f = a->frames[c->frame];
    lv_color_t pal[16];
    for (int i = 0; i < a->ncolors && i < 16; i++)
        pal[i] = i == 0 ? c->bg : lv_color_hex(a->palette[i]);

    const int s = c->scale, W = CLAWD_GRID * s;
    for (int cy = 0; cy < CLAWD_GRID; cy++) {
        for (int cx = 0; cx < CLAWD_GRID; cx++) {
            int idx = cy * CLAWD_GRID + cx;
            uint8_t v = f[idx >> 1];
            v = (idx & 1) ? (v & 0x0F) : (v >> 4);
            lv_color_t col = v < a->ncolors ? pal[v] : c->bg;
            lv_color_t *row = c->buf + (cy * s) * W + cx * s;
            for (int dy = 0; dy < s; dy++, row += W)
                for (int dx = 0; dx < s; dx++) row[dx] = col;
        }
    }
    lv_obj_invalidate(c->canvas);
}

static void tick_cb(lv_timer_t *t)
{
    clawd_t *c = t->user_data;
    const clawd_anim_t *a = &clawd_anims[c->anim];
    if (screen_is_on()) {
        c->frame = (c->frame + 1) % a->nframes;
        draw_frame(c);
    }
    uint16_t hold = a->holds[c->frame];
    lv_timer_set_period(t, hold < 40 ? 40 : hold);
}

static void delete_cb(lv_event_t *e)
{
    clawd_t *c = lv_event_get_user_data(e);
    if (c->timer) lv_timer_del(c->timer);
    heap_caps_free(c->buf);
    free(c);
}

lv_obj_t *clawd_create(lv_obj_t *parent, int scale, lv_color_t bg)
{
    clawd_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    int W = CLAWD_GRID * scale;
    c->buf = heap_caps_malloc(LV_CANVAS_BUF_SIZE_TRUE_COLOR(W, W), MALLOC_CAP_INTERNAL);
    if (!c->buf) { free(c); return NULL; }
    c->scale = scale;
    c->bg = bg;
    c->canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(c->canvas, c->buf, W, W, LV_IMG_CF_TRUE_COLOR);
    lv_obj_add_event_cb(c->canvas, delete_cb, LV_EVENT_DELETE, c);
    lv_obj_set_user_data(c->canvas, c);
    c->anim = CLAWD_IDLE_BLINK;
    draw_frame(c);
    c->timer = lv_timer_create(tick_cb, clawd_anims[c->anim].holds[0], c);
    return c->canvas;
}

void clawd_play(lv_obj_t *obj, clawd_anim_id_t id)
{
    if (!obj || id >= CLAWD_COUNT) return;
    clawd_t *c = lv_obj_get_user_data(obj);
    if (!c || c->anim == id) return;
    c->anim = id;
    c->frame = 0;
    draw_frame(c);
    lv_timer_set_period(c->timer, clawd_anims[id].holds[0]);
    lv_timer_reset(c->timer);
}
