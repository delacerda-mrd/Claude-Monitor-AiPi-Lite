/*
 * app.h  --  shared types and cross-module state for Claude Meter.
 *
 * Threading model (unchanged from v1, and load-bearing):
 *   - main loop  : the ONLY task that touches LVGL, the panel, the backlight
 *                  and the LED strip.
 *   - poll task  : samples battery/power, polls the API, plays alert sounds.
 *   - net task   : brings Wi-Fi / SNTP / mDNS / web server up, then exits.
 *   - httpd task : web handlers; reads/writes settings, never touches LVGL.
 * Cross-task data goes through usage_get()/settings_get() (mutex-guarded
 * snapshots) or the volatile flags in g_sys.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "audio.h"

/* Usage thresholds (percent) shared by LED, ring color, Clawd mood, alerts */
#define USAGE_AMBER_PCT     60
#define USAGE_RED_PCT       85

#define WINDOW_5H_S         (5 * 3600)
#define WINDOW_7D_S         (7 * 24 * 3600)

typedef enum {
    BOOT_START = 0,     /* splash, nothing up yet                   */
    BOOT_WIFI,          /* joining Wi-Fi                            */
    BOOT_TIME,          /* SNTP                                     */
    BOOT_FETCH,         /* online, waiting for the first poll       */
    BOOT_READY,         /* first poll done (ok or not)              */
    BOOT_SETUP,         /* no Wi-Fi: setup access point is up       */
} boot_stage_t;

typedef struct {
    volatile boot_stage_t boot;
    volatile bool   wifi_up;
    volatile int    rssi;               /* dBm, 0 = unknown                 */
    volatile int    batt_pct;           /* -1 = unknown                     */
    volatile bool   ext_power;          /* USB host or wall charger         */
    char            ip[16];
    char            ssid[33];
    char            ap_ssid[33];        /* setup AP name when BOOT_SETUP    */
} sys_state_t;

extern sys_state_t g_sys;

/* Remote button (web API): 1 = tap, 2 = hold. Consumed by the main loop. */
extern volatile int g_btn_sim;

/* Play a sound unless muted / inside quiet hours. Non-blocking. */
void app_sound(melody_type_t m);

/* Speak a voice script (voice.h format) unless muted / quiet; falls back to
 * the tone when talking is off or no voice pack is installed. Non-blocking. */
void app_say(const char *script, melody_type_t fallback);

/* Screenshot for the web API (httpd task). Blocks until the main loop has
 * re-rendered a full frame into malloc'd horizontal strips of lv_color_t
 * (16-bit, LVGL byte order), *rows rows each. Caller frees every strip.
 * false on timeout/OOM. */
bool app_capture_frame(uint16_t **strips, int *nstrips, int *rows);
