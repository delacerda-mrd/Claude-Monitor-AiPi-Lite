/*
 * clawd.h  --  animated Clawd (20x20 pixel art scaled up) on an LVGL canvas.
 * Main loop / LVGL context only.
 */
#pragma once

#include "lvgl.h"
#include "clawd_anims.h"

/* Creates a (20*scale)^2 canvas; the pixel buffer is freed with the object. */
lv_obj_t *clawd_create(lv_obj_t *parent, int scale, lv_color_t bg);
void      clawd_play(lv_obj_t *clawd, clawd_anim_id_t id);
/* Draw it as a hologram: every colour becomes its brightness in `tint`. */
void      clawd_set_holo(lv_obj_t *clawd, lv_color_t tint);
