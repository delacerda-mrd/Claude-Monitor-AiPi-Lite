/*
 * brain.c  --  ship an unrecognized utterance to the Mac and play the answer.
 *
 *   POST http://<notify_ip>:5556/ask
 *     body    raw PCM, 16 kHz mono s16le (what the mic heard after "Yes?")
 *     X-Meter-State  JSON: usage, resets, battery, local time
 *   200 ->  body raw PCM (16 kHz mono s16le) = the spoken answer
 *           X-Heard   what whisper heard      X-Action  optional, see do_action()
 *   204 ->  nothing intelligible was heard
 *
 * One request at a time, on its own task; the answer is queued on the audio
 * task like any other speech. All buffers live in PSRAM.
 */
#include "brain.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "app.h"
#include "audio.h"
#include "settings.h"
#include "ui.h"
#include "usage.h"

static const char *TAG = "brain";

#define MAX_REPLY_SAMPLES  (16000 * 40)     /* 40 s, 1.28 MB of PSRAM */
#define HTTP_TIMEOUT_MS    30000

static TaskHandle_t   s_task;
static int16_t       *s_pcm;
static int            s_samples;
static volatile bool  s_busy;
static char           s_heard[96];
static char           s_action[24];
static char           s_status[32] = "idle";

bool        brain_busy(void)   { return s_busy; }
const char *brain_heard(void)  { return s_heard; }
const char *brain_status(void) { return s_status; }

bool brain_available(void)
{
    settings_t c;
    settings_get(&c);
    return c.brain && c.notify_ip[0] && g_sys.wifi_up && !s_busy;
}

bool brain_ask(int16_t *pcm, int samples)
{
    if (!s_task || !brain_available() || samples <= 0) return false;
    s_pcm = pcm;
    s_samples = samples;
    s_busy = true;
    xTaskNotifyGive(s_task);
    return true;
}

/* ------------------------------------------------------------------ */
static esp_err_t on_event(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_HEADER) {
        if (!strcasecmp(e->header_key, "X-Heard"))  strlcpy(s_heard, e->header_value, sizeof(s_heard));
        if (!strcasecmp(e->header_key, "X-Action")) strlcpy(s_action, e->header_value, sizeof(s_action));
    }
    return ESP_OK;
}

static void state_json(char *b, size_t n)
{
    usage_t u;
    usage_get(&u);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char when[32] = "unknown";
    if (now > 1600000000) strftime(when, sizeof(when), "%a %H:%M", &tm);
    long sl = u.session_reset > now ? (long)(u.session_reset - now) : -1;
    long wl = u.weekly_reset > now ? (long)(u.weekly_reset - now) : -1;
    snprintf(b, n,
             "{\"have\":%s,\"ok\":%s,\"s\":%d,\"s_left\":%ld,\"w\":%d,\"w_left\":%ld,"
             "\"batt\":%d,\"usb\":%s,\"chg\":%s,\"time\":\"%s\"}",
             u.have_data ? "true" : "false", u.ok ? "true" : "false",
             u.session_pct, sl, u.weekly_pct, wl, g_sys.batt_pct,
             g_sys.ext_power ? "true" : "false", g_sys.charging ? "true" : "false", when);
}

static void do_action(const char *a)
{
    if (!a[0]) return;
    ESP_LOGI(TAG, "action: %s", a);
    settings_t c;
    if      (!strcmp(a, "page:rings"))  ui_request(UI_REQ_PAGE_RINGS);
    else if (!strcmp(a, "page:pace"))   ui_request(UI_REQ_PAGE_PACE);
    else if (!strcmp(a, "page:trend"))  ui_request(UI_REQ_PAGE_TREND);
    else if (!strcmp(a, "page:clawd"))  ui_request(UI_REQ_PAGE_CLAWD);
    else if (!strcmp(a, "page:system")) ui_request(UI_REQ_PAGE_SYSTEM);
    else if (!strcmp(a, "screen_off"))  ui_request(UI_REQ_SCREEN_OFF);
    else if (!strcmp(a, "refresh"))     usage_poll_now();
    else if (!strcmp(a, "mute") || !strcmp(a, "unmute")) {
        settings_get(&c); c.mute = !strcmp(a, "mute"); settings_put(&c);
    } else if (!strcmp(a, "louder") || !strcmp(a, "softer")) {
        settings_get(&c);
        int v = c.volume + (!strcmp(a, "louder") ? 10 : -10);
        c.volume = v < 40 ? 40 : v > 100 ? 100 : v;
        settings_put(&c);
    }
}

/* One round trip. Returns the reply (PSRAM, caller frees) or NULL. */
static int16_t *ask(int *reply_samples, int *status)
{
    settings_t c;
    settings_get(&c);
    char url[64], st[256];
    snprintf(url, sizeof(url), "http://%s:%d/ask", c.notify_ip, BRAIN_PORT);
    state_json(st, sizeof(st));

    esp_http_client_config_t cfg = {
        .url = url, .method = HTTP_METHOD_POST, .timeout_ms = HTTP_TIMEOUT_MS,
        .event_handler = on_event, .buffer_size = 1024, .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) return NULL;
    esp_http_client_set_header(h, "Content-Type", "audio/L16; rate=16000; channels=1");
    esp_http_client_set_header(h, "X-Meter-State", st);

    int16_t *reply = NULL;
    int len = s_samples * 2;
    *status = -1;
    if (esp_http_client_open(h, len) != ESP_OK) { strlcpy(s_status, "mac unreachable", sizeof(s_status)); goto out; }
    for (int off = 0; off < len; ) {
        int k = len - off > 4096 ? 4096 : len - off;
        int w = esp_http_client_write(h, (const char *)s_pcm + off, k);
        if (w <= 0) { strlcpy(s_status, "upload failed", sizeof(s_status)); goto out; }
        off += w;
    }
    int64_t clen = esp_http_client_fetch_headers(h);
    *status = esp_http_client_get_status_code(h);
    if (*status != 200 || clen <= 0) {
        snprintf(s_status, sizeof(s_status), "http %d", *status);
        goto out;
    }
    if (clen > MAX_REPLY_SAMPLES * 2) clen = MAX_REPLY_SAMPLES * 2;
    reply = heap_caps_malloc(clen, MALLOC_CAP_SPIRAM);
    if (!reply) { strlcpy(s_status, "no memory", sizeof(s_status)); goto out; }
    int got = 0;
    while (got < clen) {
        int r = esp_http_client_read(h, (char *)reply + got, clen - got);
        if (r <= 0) break;
        got += r;
    }
    *reply_samples = got / 2;
    strlcpy(s_status, "idle", sizeof(s_status));
out:
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return reply;
}

static void brain_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        strlcpy(s_status, "asking", sizeof(s_status));
        s_heard[0] = s_action[0] = '\0';
        int64_t t0 = esp_timer_get_time();
        int n = 0, status = 0;
        int16_t *reply = ask(&n, &status);
        heap_caps_free(s_pcm);
        s_pcm = NULL;
        ESP_LOGI(TAG, "%d samples up, http %d, %d samples back in %lld ms, heard \"%s\"",
                 s_samples, status, n, (esp_timer_get_time() - t0) / 1000, s_heard);
        if (reply && n > 0) {
            do_action(s_action);
            if (settings_muted() || !audio_play_pcm_async(reply, n)) heap_caps_free(reply);
        } else {
            heap_caps_free(reply);
            app_say_now(status == 204 ? "@sorry/sorry" : "@nobrain/sorry", MELODY_ERROR);
        }
        s_busy = false;
    }
}

void brain_init(void)
{
    xTaskCreate(brain_task, "brain", 5120, NULL, 4, &s_task);
}
