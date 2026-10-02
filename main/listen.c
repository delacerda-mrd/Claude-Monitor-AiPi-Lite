/*
 * listen.c  --  "Jarvis, how's my usage?"  Fully offline.
 *
 *   mic (ES8311 ADC, 16 kHz) -> feed task -> AFE (NS/VAD/AGC + WakeNet9
 *   "Jarvis") -> detect task -> on wake: "Yes?" -> MultiNet7 English command
 *   (6 s window) -> action + spoken answer.
 *
 * Models live in the "model" partition (esp-sr packs srmodels.bin there on
 * USB flash). Everything runs from PSRAM. Detections are ignored while the
 * meter itself is talking, so its own voice can't wake it.
 *
 * Listening runs when enabled in settings and on USB power -- or on battery
 * too if "listen on battery" is set (the mic path costs some battery).
 */
#include "listen.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "esp_afe_sr_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"

#include "app.h"
#include "audio.h"
#include "led.h"
#include "power.h"
#include "settings.h"
#include "ui.h"
#include "usage.h"

static const char *TAG = "listen";

#define COMMAND_WINDOW_MS   6000
#define PROMPT_GAP_MS       60       /* after "Yes?" ends, before listening */
#define SELF_GUARD_MS       400      /* ignore wake words this long after we spoke */

/* ------------------------------------------------------------------ */
/* Command set                                                         */
/* ------------------------------------------------------------------ */
enum {
    CMD_STATUS = 1, CMD_RESET, CMD_WEEKLY, CMD_REFRESH, CMD_PACE,
    CMD_NEXT, CMD_PREV, CMD_HOME, CMD_TREND, CMD_BUDDY, CMD_DEVICE,
    CMD_MUTE, CMD_UNMUTE, CMD_LOUDER, CMD_SOFTER, CMD_SCREEN_OFF,
    CMD_TIME, CMD_THANKS, CMD_WHO, CMD_BATTERY,
};

static const struct { int id; const char *phrase; } COMMANDS[] = {
    { CMD_STATUS,     "how is my usage" },
    { CMD_STATUS,     "what is my usage" },
    { CMD_STATUS,     "status report" },
    { CMD_STATUS,     "usage report" },
    { CMD_RESET,      "when do i reset" },
    { CMD_RESET,      "time to reset" },
    { CMD_WEEKLY,     "what is my weekly usage" },
    { CMD_WEEKLY,     "weekly usage" },
    { CMD_REFRESH,    "refresh" },
    { CMD_REFRESH,    "check again" },
    { CMD_PACE,       "am i on pace" },
    { CMD_PACE,       "how is my pace" },
    { CMD_PACE,       "check my pace" },
    { CMD_PACE,       "pace report" },
    { CMD_PACE,       "show pace" },
    { CMD_NEXT,       "next page" },
    { CMD_PREV,       "previous page" },
    { CMD_PREV,       "go back" },
    { CMD_HOME,       "go home" },
    { CMD_HOME,       "show usage" },
    { CMD_TREND,      "show trend" },
    { CMD_TREND,      "show history" },
    { CMD_BUDDY,      "show my buddy" },
    { CMD_DEVICE,     "show device" },
    { CMD_DEVICE,     "device info" },
    { CMD_MUTE,       "quiet mode" },
    { CMD_MUTE,       "mute" },
    { CMD_UNMUTE,     "sound on" },
    { CMD_UNMUTE,     "unmute" },
    { CMD_LOUDER,     "volume up" },
    { CMD_LOUDER,     "louder" },
    { CMD_SOFTER,     "volume down" },
    { CMD_SOFTER,     "quieter" },
    { CMD_SCREEN_OFF, "screen off" },
    { CMD_SCREEN_OFF, "go to sleep" },
    { CMD_TIME,       "what time is it" },
    { CMD_THANKS,     "thank you" },
    { CMD_THANKS,     "thanks" },
    { CMD_WHO,        "who are you" },
    { CMD_WHO,        "hello" },
    { CMD_BATTERY,    "battery level" },
    { CMD_BATTERY,    "how is my battery" },
};

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */
static const esp_afe_sr_iface_t *s_afe;
static esp_afe_sr_data_t        *s_afe_data;
static esp_mn_iface_t           *s_mn;
static model_iface_data_t       *s_mn_data;
static volatile listen_state_t   s_state = LISTEN_OFF;
static volatile bool             s_force_wake;
static volatile float            s_level_db = -96.0f;
static char                      s_heard[48];

/* Recorder: a PSRAM ring of the raw mic feed, snapshotted per attempt. */
#define REC_SAMPLES   (16000 * 8)            /* 8 s */
static int16_t       *s_ring, *s_take;
static volatile int   s_ring_pos;            /* next write index */
static int            s_take_len;
static volatile int   s_take_from = -1;      /* ring index at wake time */

const int16_t *listen_last_take(int *samples) { *samples = s_take_len; return s_take; }

static void ring_write(const int16_t *d, int n)
{
    if (!s_ring) return;
    int p = s_ring_pos;
    for (int i = 0; i < n; i++) { s_ring[p] = d[i]; p = (p + 1) % REC_SAMPLES; }
    s_ring_pos = p;
}

static void take_snapshot(void)
{
    if (!s_ring || !s_take || s_take_from < 0) return;
    int end = s_ring_pos, from = s_take_from;
    int n = (end - from + REC_SAMPLES) % REC_SAMPLES;
    for (int i = 0; i < n; i++) s_take[i] = s_ring[(from + i) % REC_SAMPLES];
    s_take_len = n;
    s_take_from = -1;
}
static volatile int64_t          s_heard_us;

listen_state_t listen_state(void)   { return s_state; }
const char    *listen_heard(void)   { return s_heard; }
int64_t        listen_heard_us(void){ return s_heard_us; }
float          listen_level_db(void){ return s_level_db; }
void           listen_wake_now(void){ s_force_wake = true; }

static bool enabled_now(void)
{
    settings_t c;
    settings_get(&c);
    return c.listen && (g_sys.ext_power || c.listen_batt) && g_sys.boot != BOOT_SETUP;
}

/* ------------------------------------------------------------------ */
/* Actions                                                             */
/* ------------------------------------------------------------------ */
static void say_reset(const char *intro, time_t reset)
{
    char s[64];
    time_t now = time(NULL);
    if (reset > now && now > 1600000000) {
        snprintf(s, sizeof(s), "%s ~%ld", intro, (long)(reset - now));
        app_say_now(s, MELODY_POLL_OK);
    } else {
        app_say_now("sorry", MELODY_ERROR);
    }
}

static void say_time(void)
{
    time_t now = time(NULL);
    if (now < 1600000000) { app_say_now("sorry", MELODY_ERROR); return; }
    struct tm tm;
    localtime_r(&now, &tm);
    int h = tm.tm_hour % 12 ? tm.tm_hour % 12 : 12;
    char s[64];
    if (tm.tm_min == 0)     snprintf(s, sizeof(s), "its #%d oclock", h);
    else if (tm.tm_min < 10) snprintf(s, sizeof(s), "its #%d oh #%d %s", h, tm.tm_min, tm.tm_hour < 12 ? "am" : "pm");
    else                     snprintf(s, sizeof(s), "its #%d #%d %s", h, tm.tm_min, tm.tm_hour < 12 ? "am" : "pm");
    app_say_now(s, MELODY_POLL_OK);
}

static void say_pace(void)
{
    usage_t u;
    usage_get(&u);
    time_t now = time(NULL);
    if (!u.have_data || !u.session_reset || now < 1600000000) { app_say_now("sorry", MELODY_ERROR); return; }
    long left = (long)(u.session_reset - now);
    if (left < 0) left = 0;
    if (left > WINDOW_5H_S) left = WINDOW_5H_S;
    int d = u.session_pct - (int)((WINDOW_5H_S - left) * 100 / WINDOW_5H_S);
    app_say_now(d > 10 ? "ahead" : d < -10 ? "room" : "on_pace", MELODY_POLL_OK);
}

static void set_volume(int delta)
{
    settings_t c;
    settings_get(&c);
    int v = c.volume + delta;
    c.volume = v < 40 ? 40 : v > 100 ? 100 : v;
    settings_put(&c);
    app_say_now(delta > 0 ? "louder" : "softer", MELODY_BUTTON);
}

static void set_mute(bool on)
{
    settings_t c;
    settings_get(&c);
    if (on) app_say_now("quiet_on", MELODY_NONE);   /* say it before going quiet */
    c.mute = on;
    settings_put(&c);
    if (!on) app_say_now("quiet_off", MELODY_BUTTON);
}

/* Unmatched speech. Offline for now: say so. A future "brain on the Mac"
 * would ship the utterance audio to the host here instead. */
static void handle_unknown(bool heard_speech)
{
    if (heard_speech) app_say_now("sorry", MELODY_ERROR);
}

static void handle_command(int id)
{
    usage_t u;
    usage_get(&u);
    char s[96];
    switch (id) {
    case CMD_STATUS:  usage_say_status(); break;
    case CMD_RESET:   say_reset("session_resets", u.session_reset); break;
    case CMD_WEEKLY:
        snprintf(s, sizeof(s), "weekly_at #%d percent .", u.weekly_pct);
        app_say_now(s, MELODY_POLL_OK);
        say_reset("weekly_resets", u.weekly_reset);
        break;
    case CMD_REFRESH: app_say_now("refreshing", MELODY_BUTTON); usage_announce_next(); usage_poll_now(); break;
    case CMD_PACE:    ui_request(UI_REQ_PAGE_PACE); say_pace(); break;
    case CMD_NEXT:    ui_request(UI_REQ_NEXT); app_sound_now(MELODY_BUTTON); break;
    case CMD_PREV:    ui_request(UI_REQ_PREV); app_sound_now(MELODY_BUTTON); break;
    case CMD_HOME:    ui_request(UI_REQ_PAGE_RINGS); app_sound_now(MELODY_BUTTON); break;
    case CMD_TREND:   ui_request(UI_REQ_PAGE_TREND); app_sound_now(MELODY_BUTTON); break;
    case CMD_BUDDY:   ui_request(UI_REQ_PAGE_CLAWD); app_sound_now(MELODY_BUTTON); break;
    case CMD_DEVICE:  ui_request(UI_REQ_PAGE_SYSTEM); app_sound_now(MELODY_BUTTON); break;
    case CMD_MUTE:    set_mute(true); break;
    case CMD_UNMUTE:  set_mute(false); break;
    case CMD_LOUDER:  set_volume(+10); break;
    case CMD_SOFTER:  set_volume(-10); break;
    case CMD_SCREEN_OFF: app_say_now("night", MELODY_NONE); ui_request(UI_REQ_SCREEN_OFF); break;
    case CMD_TIME:    say_time(); break;
    case CMD_THANKS:  app_say_now("welcome", MELODY_BUTTON); break;
    case CMD_WHO:     app_say_now("intro", MELODY_BUTTON); break;
    case CMD_BATTERY:
        if (g_sys.batt_pct < 0) { app_say_now("sorry", MELODY_ERROR); break; }
        snprintf(s, sizeof(s), "battery_at #%d percent%s", g_sys.batt_pct,
                 g_sys.charging ? " charging_now" : "");
        app_say_now(s, MELODY_POLL_OK);
        break;
    default: handle_unknown(true); break;
    }
}

/* ------------------------------------------------------------------ */
/* Tasks                                                               */
/* ------------------------------------------------------------------ */
static void feed_task(void *arg)
{
    (void)arg;
    int chunk = s_afe->get_feed_chunksize(s_afe_data);
    int nch   = s_afe->get_feed_channel_num(s_afe_data);
    int16_t *buf = heap_caps_malloc(chunk * nch * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    bool mic = false;
    ESP_LOGI(TAG, "feed: %d samples x %d ch", chunk, nch);
    for (;;) {
        bool want = enabled_now();
        if (want != mic) {
            audio_mic_enable(want);
            mic = want;
            s_state = want ? LISTEN_IDLE : LISTEN_OFF;
            led_set_listening(false);
        }
        if (!mic) { vTaskDelay(pdMS_TO_TICKS(300)); continue; }
        int got = 0;
        while (got < chunk) {
            int n = audio_mic_read(buf + got, chunk - got);
            if (n <= 0) break;
            got += n;
        }
        if (got == chunk) {
            ring_write(buf, chunk);
            s_afe->feed(s_afe_data, buf);
        }
    }
}

static void finish(void)
{
    s_afe->enable_wakenet(s_afe_data);
    s_state = enabled_now() ? LISTEN_IDLE : LISTEN_OFF;
    led_set_listening(false);
}

static void detect_task(void *arg)
{
    (void)arg;
    int64_t t_state = 0, last_spoke = 0, first_speech = 0;
    bool heard_speech = false, prompt_played = false;
    float peak_db = -96.0f;
    for (;;) {
        afe_fetch_result_t *res = s_afe->fetch(s_afe_data);
        if (!res || res->ret_value == ESP_FAIL) continue;
        s_level_db = res->data_volume;
        int64_t now = esp_timer_get_time();
        if (audio_is_playing()) last_spoke = now;

        switch (s_state) {
        case LISTEN_OFF:
            break;
        case LISTEN_IDLE: {
            bool woke = res->wakeup_state == WAKENET_DETECTED &&
                        now - last_spoke > SELF_GUARD_MS * 1000;
            if (woke || s_force_wake) {
                s_force_wake = false;
                ESP_LOGI(TAG, "wake (%s)", woke ? "Jarvis" : "manual");
                s_afe->disable_wakenet(s_afe_data);
                /* record from ~1 s before the wake (covers "Jarvis") */
                s_take_from = (s_ring_pos - 16000 + REC_SAMPLES) % REC_SAMPLES;
                s_state = LISTEN_PROMPT;
                t_state = now;
                heard_speech = false;
                prompt_played = false;
                peak_db = -96.0f;
                first_speech = 0;
                s_heard[0] = '\0';
                screen_wake();
                led_set_listening(true);
                app_say_now("yes", MELODY_BUTTON);
            }
            break;
        }
        case LISTEN_PROMPT:
            /* Start listening the moment "Yes?" ends -- people answer fast,
             * and a late start eats the first words. Wait until the prompt
             * has actually started (it's queued) unless we're muted. */
            if (audio_is_playing()) prompt_played = true;
            if ((prompt_played || now - t_state > 250 * 1000) && !audio_is_playing() &&
                now - last_spoke > PROMPT_GAP_MS * 1000) {
                s_mn->clean(s_mn_data);
                s_state = LISTEN_COMMAND;
                t_state = now;
            } else if (now - t_state > 4000 * 1000) {
                finish();
            }
            break;
        case LISTEN_COMMAND: {
            if (res->vad_state == VAD_SPEECH) {
                if (!heard_speech) first_speech = now;
                heard_speech = true;
            }
            if (res->data_volume > peak_db) peak_db = res->data_volume;
            esp_mn_state_t st = s_mn->detect(s_mn_data, res->data);
            if (st == ESP_MN_STATE_DETECTED) {
                esp_mn_results_t *r = s_mn->get_results(s_mn_data);
                int id = r->num > 0 ? r->command_id[0] : 0;
                const char *txt = id ? esp_mn_commands_get_string(r->command_id[0]) : NULL;
                strlcpy(s_heard, txt ? txt : r->string, sizeof(s_heard));
                s_heard_us = now;
                ESP_LOGI(TAG, "command %d \"%s\" (p=%.2f, peak %.0f dBFS)", id, s_heard,
                         r->num ? r->prob[0] : 0.0f, peak_db);
                take_snapshot();
                finish();
                handle_command(id);
            } else if (st == ESP_MN_STATE_TIMEOUT || now - t_state > (COMMAND_WINDOW_MS + 500) * 1000LL) {
                esp_mn_results_t *r = s_mn->get_results(s_mn_data);
                ESP_LOGI(TAG, "no command (speech %s, +%lld ms after listen start, peak %.0f dBFS, best guess \"%s\")",
                         heard_speech ? "heard" : "none",
                         first_speech ? (long long)(first_speech - t_state) / 1000 : -1LL,
                         peak_db, r && r->raw_string[0] ? r->raw_string : "");
                take_snapshot();
                finish();
                handle_unknown(heard_speech);
            }
            break;
        }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */
bool listen_init(void)
{
    srmodel_list_t *models = esp_srmodel_init("model");
    if (!models || models->num == 0) {
        ESP_LOGW(TAG, "no speech models in the \"model\" partition - voice control off");
        return false;
    }
    for (int i = 0; i < models->num; i++) ESP_LOGI(TAG, "model: %s", models->model_name[i]);

    afe_config_t *cfg = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (!cfg) return false;
    cfg->aec_init = false;                      /* one mic, no playback reference */
    cfg->se_init = false;
    cfg->wakenet_model_name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
    const char *wn_name = cfg->wakenet_model_name;  /* points into `models` */
    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    cfg->afe_perferred_core = 1;
    cfg->afe_perferred_priority = 5;
    s_afe = esp_afe_handle_from_config(cfg);
    s_afe_data = s_afe ? s_afe->create_from_config(cfg) : NULL;
    afe_config_free(cfg);
    if (!s_afe_data) { ESP_LOGE(TAG, "AFE create failed"); return false; }

    char *mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_ENGLISH);
    s_mn = mn_name ? esp_mn_handle_from_name(mn_name) : NULL;
    s_mn_data = s_mn ? s_mn->create(mn_name, COMMAND_WINDOW_MS) : NULL;
    if (!s_mn_data) { ESP_LOGE(TAG, "MultiNet create failed"); return false; }

    esp_mn_commands_alloc(s_mn, s_mn_data);
    for (size_t i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); i++)
        esp_mn_commands_add(COMMANDS[i].id, COMMANDS[i].phrase);
    esp_mn_error_t *err = esp_mn_commands_update();
    if (err && err->num)
        for (int i = 0; i < err->num; i++)
            ESP_LOGW(TAG, "command rejected: \"%s\"", err->phrases[i]->string);

    int fetch = s_afe->get_fetch_chunksize(s_afe_data);
    int mnchk = s_mn->get_samp_chunksize(s_mn_data);
    ESP_LOGI(TAG, "ready: %s + %s, fetch %d / mn %d samples",
             wn_name ? wn_name : "?", mn_name, fetch, mnchk);

    s_ring = heap_caps_malloc(REC_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_take = heap_caps_malloc(REC_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    xTaskCreatePinnedToCore(feed_task, "sr_feed", 4096, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(detect_task, "sr_detect", 8192, NULL, 5, NULL, 1);
    return true;
}
