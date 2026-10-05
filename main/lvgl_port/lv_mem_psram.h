/*
 * lv_mem_psram.h  --  LVGL 8 allocator -> PSRAM (ported from the ES3C28P,
 * its WI-22 lever L3). Pulled in by lv_mem.c through
 * CONFIG_LV_MEM_CUSTOM_INCLUDE; the LV_MEM_CUSTOM_ALLOC/FREE/REALLOC names
 * are set on the LVGL library in the root CMakeLists.txt. Draw buffers are
 * not affected: main.c keeps them as static (internal, DMA-able) arrays.
 */
#pragma once
#include <stddef.h>
#include "esp_heap_caps.h"

static inline void *lv_psram_alloc(size_t s)            { return heap_caps_malloc(s, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
static inline void *lv_psram_realloc(void *p, size_t s) { return heap_caps_realloc(p, s, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
static inline void  lv_psram_free(void *p)              { heap_caps_free(p); }
