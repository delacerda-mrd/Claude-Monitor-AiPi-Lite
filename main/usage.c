/*
 * usage.c  --  the poll task.
 *
 * Primary source: GET https://api.anthropic.com/api/oauth/usage -- the same
 * endpoint Claude Code's /usage reads (and the Clawdmeter HY3 daemon uses in
 * production). It returns utilization for the 5h and 7d windows and costs
 * nothing. Fallback: the v1 method, a 1-token POST /v1/messages whose
 * anthropic-ratelimit-unified-* headers carry the same numbers. The fallback
 * only runs when the usage endpoint itself misbehaves (bad status / bad
 * JSON) -- never on auth or network failure, where it would fail too.
 */
#include "usage.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

#include "app.h"
#include "led.h"
#include "net.h"
#include "power.h"
#include "settings.h"

static const char *TAG = "usage";

#define USAGE_URL        "https://api.anthropic.com/api/oauth/usage"
#define MESSAGES_URL     "https://api.anthropic.com/v1/messages"
/* Same identification the HY3 daemon sends; the endpoint is Claude Code's. */
#define USAGE_UA         "claude-code/2.1.5"

#define POLL_USB_S       90      /* free endpoint: snappier than v1's 120 */
#define POLL_BATT_S      300
#define POLL_FALLBACK_S  120     /* header fallback costs a token: v1 pace */

static SemaphoreHandle_t s_mtx;
static usage_t           s_u;
static usage_hist_t      s_hist;

/* History survives esp_restart()/OTA reboots in RTC RAM (no flash wear);
 * a power cycle or a gap longer than the chart drops it. */
#define HIST_MAGIC 0xC1A0D5E5u
typedef struct {
    uint32_t     magic;
    time_t       last_epoch;            /* wall time of the newest sample */
    usage_hist_t h;
    uint32_t     sum;
} hist_rtc_t;
static RTC_NOINIT_ATTR hist_rtc_t s_rtc;
static bool s_hist_restored;

static uint32_t hist_sum(const hist_rtc_t *r)
{
    uint32_t x = r->magic ^ (uint32_t)r->last_epoch ^ (uint32_t)r->h.count;
    for (int i = 0; i < HIST_N; i++) x = x * 31 + r->h.s[i] * 7 + r->h.w[i];
    return x;
}
static volatile bool     s_force;
static volatile bool     s_announce;

static void lock(void)   { xSemaphoreTake(s_mtx, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mtx); }

void usage_init(void) { s_mtx = xSemaphoreCreateMutex(); }

void usage_get(usage_t *out)        { lock(); *out = s_u;    unlock(); }
void usage_hist_get(usage_hist_t *o){ lock(); *o = s_hist;   unlock(); }
void usage_poll_now(void)           { s_force = true; }
void usage_announce_next(void)      { s_announce = true; }

const char *usage_src_str(usage_src_t s)
{
    switch (s) {
    case SRC_USAGE_API: return "usage api";
    case SRC_HEADERS:   return "msg headers";
    default:            return "none";
    }
}

const char *usage_err_str(poll_result_t r)
{
    switch (r) {
    case POLL_OK:   return "ok";
    case POLL_AUTH: return "token";
    case POLL_NET:  return "offline";
    }
    return "?";
}

/* ------------------------------------------------------------------ */
/* ISO-8601 -> epoch ("2026-10-02T05:00:00.123+00:00", "...Z")          */
/* ------------------------------------------------------------------ */
static long days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static time_t parse_iso8601(const char *s)
{
    int Y, M, D, h, m, sec = 0;
    if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &sec) < 5)
        return 0;
    const char *p = strchr(s, 'T');
    if (!p) return 0;
    p++;
    while (*p && (isdigit((unsigned char)*p) || *p == ':' || *p == '.')) p++;
    long off = 0;
    if (*p == '+' || *p == '-') {
        int oh = 0, om = 0;
        sscanf(p + 1, "%d:%d", &oh, &om);
        off = (oh * 3600L + om * 60L) * (*p == '-' ? -1 : 1);
    }
    return (time_t)(days_from_civil(Y, M, D) * 86400L + h * 3600L + m * 60L + sec - off);
}

/* ------------------------------------------------------------------ */
/* Primary: GET /api/oauth/usage                                       */
/* ------------------------------------------------------------------ */
typedef struct {
    int    s_pct, w_pct;
    time_t s_reset, w_reset;
} sample_t;

#define BODY_MAX 4096

static int64_t s_api_holdoff_until;    /* skip the usage API until then (429) */

/* Log the start of an error body -- the API's JSON error says what's wrong. */
static void log_error_body(esp_http_client_handle_t c, int code)
{
    char buf[160];
    int n = esp_http_client_read(c, buf, sizeof(buf) - 1);
    buf[n > 0 ? n : 0] = '\0';
    for (char *p = buf; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
    ESP_LOGW(TAG, "usage api: HTTP %d %s", code, buf);
}

/* Returns POLL_OK, POLL_AUTH, POLL_NET (transport), or -1 for
 * "endpoint misbehaved" (bad status/JSON) -> caller tries the fallback. */
static int poll_usage_api(const char *auth, sample_t *out)
{
    esp_http_client_config_t cfg = {
        .url               = USAGE_URL,
        .method            = HTTP_METHOD_GET,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 15000,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return POLL_NET;
    esp_http_client_set_header(c, "Authorization", auth);
    esp_http_client_set_header(c, "anthropic-beta", "oauth-2025-04-20");
    esp_http_client_set_header(c, "anthropic-version", "2023-06-01");
    esp_http_client_set_header(c, "User-Agent", USAGE_UA);
    esp_http_client_set_header(c, "Accept", "application/json");

    int result = -1;
    char *body = NULL;
    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "usage api: %s", esp_err_to_name(err));
        result = POLL_NET;
        goto out;
    }
    esp_http_client_fetch_headers(c);
    int code = esp_http_client_get_status_code(c);
    if (code == 401 || code == 403) { log_error_body(c, code); result = POLL_AUTH; goto out; }
    if (code == 429) {
        /* Throttled: honor retry-after (default 10 min) and let the caller
         * fall back to the header poll meanwhile. */
        char *ra = NULL;
        int wait_s = 600;
        if (esp_http_client_get_header(c, "retry-after", &ra) == ESP_OK && ra && atoi(ra) > 0)
            wait_s = atoi(ra);
        if (wait_s < 60) wait_s = 60;
        if (wait_s > 3600) wait_s = 3600;
        s_api_holdoff_until = esp_timer_get_time() + (int64_t)wait_s * 1000000;
        log_error_body(c, code);
        ESP_LOGW(TAG, "usage api throttled - header polls for %d s", wait_s);
        goto out;                   /* -1: fall back */
    }
    if (code != 200)                { log_error_body(c, code); goto out; }

    body = malloc(BODY_MAX);
    if (!body) { result = POLL_NET; goto out; }
    int total = 0;
    for (;;) {
        int r = esp_http_client_read(c, body + total, BODY_MAX - 1 - total);
        if (r <= 0) break;
        total += r;
        if (total >= BODY_MAX - 1) break;
    }
    body[total] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) { ESP_LOGW(TAG, "usage api: bad JSON (%d bytes)", total); goto out; }
    cJSON *fh = cJSON_GetObjectItem(root, "five_hour");
    cJSON *sd = cJSON_GetObjectItem(root, "seven_day");
    if (!cJSON_IsObject(fh) && !cJSON_IsObject(sd)) {
        ESP_LOGW(TAG, "usage api: no five_hour/seven_day in response");
        cJSON_Delete(root);
        goto out;
    }
    memset(out, 0, sizeof(*out));
    if (cJSON_IsObject(fh)) {
        cJSON *u = cJSON_GetObjectItem(fh, "utilization");
        cJSON *r = cJSON_GetObjectItem(fh, "resets_at");
        if (cJSON_IsNumber(u)) out->s_pct = (int)(u->valuedouble + 0.5);
        if (cJSON_IsString(r)) out->s_reset = parse_iso8601(r->valuestring);
    }
    if (cJSON_IsObject(sd)) {
        cJSON *u = cJSON_GetObjectItem(sd, "utilization");
        cJSON *r = cJSON_GetObjectItem(sd, "resets_at");
        if (cJSON_IsNumber(u)) out->w_pct = (int)(u->valuedouble + 0.5);
        if (cJSON_IsString(r)) out->w_reset = parse_iso8601(r->valuestring);
    }
    cJSON_Delete(root);
    result = POLL_OK;

out:
    free(body);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return result;
}

/* ------------------------------------------------------------------ */
/* Fallback: 1-token POST /v1/messages, read rate-limit headers (v1)   */
/* ------------------------------------------------------------------ */
typedef struct {
    sample_t smp;
    bool     got_5h, got_7d;
} hdr_ctx_t;

static esp_err_t hdr_evt(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_HEADER) return ESP_OK;
    hdr_ctx_t *x = evt->user_data;
    const char *k = evt->header_key, *v = evt->header_value;
    if (!strcasecmp(k, "anthropic-ratelimit-unified-5h-utilization")) {
        x->smp.s_pct = (int)(atof(v) * 100.0 + 0.5); x->got_5h = true;
    } else if (!strcasecmp(k, "anthropic-ratelimit-unified-5h-reset")) {
        x->smp.s_reset = atol(v);
    } else if (!strcasecmp(k, "anthropic-ratelimit-unified-7d-utilization")) {
        x->smp.w_pct = (int)(atof(v) * 100.0 + 0.5); x->got_7d = true;
    } else if (!strcasecmp(k, "anthropic-ratelimit-unified-7d-reset")) {
        x->smp.w_reset = atol(v);
    }
    return ESP_OK;
}

static int poll_headers(const char *auth, sample_t *out)
{
    static const char body[] =
        "{\"model\":\"claude-haiku-4-5\",\"max_tokens\":1,"
        "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
    hdr_ctx_t x = {0};
    esp_http_client_config_t cfg = {
        .url               = MESSAGES_URL,
        .method            = HTTP_METHOD_POST,
        .event_handler     = hdr_evt,
        .user_data         = &x,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 20000,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return POLL_NET;
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_header(c, "Authorization", auth);
    esp_http_client_set_header(c, "anthropic-version", "2023-06-01");
    esp_http_client_set_header(c, "anthropic-beta", "oauth-2025-04-20");
    esp_http_client_set_post_field(c, body, strlen(body));
    esp_err_t err = esp_http_client_perform(c);
    int code = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);

    if (err != ESP_OK) { ESP_LOGE(TAG, "headers: %s", esp_err_to_name(err)); return POLL_NET; }
    if (code == 401 || code == 403) return POLL_AUTH;
    if (code != 200 && code != 429) { ESP_LOGE(TAG, "headers: HTTP %d", code); return POLL_NET; }
    if (!x.got_5h || !x.got_7d) {
        ESP_LOGE(TAG, "headers: missing unified headers (HTTP %d) - token stale?", code);
        return POLL_AUTH;
    }
    *out = x.smp;
    return POLL_OK;
}

/* ------------------------------------------------------------------ */
/* History                                                             */
/* ------------------------------------------------------------------ */
static uint8_t clamp_pct(int p) { return p < 0 ? 0 : p > 100 ? 100 : (uint8_t)p; }

static void hist_push(int s, int w)
{
    time_t now = time(NULL);
    lock();
    /* First sample with a valid clock: adopt the pre-reboot history if it
     * is intact and recent enough to still be on the chart. */
    if (!s_hist_restored && now > 1600000000) {
        s_hist_restored = true;
        if (s_rtc.magic == HIST_MAGIC && s_rtc.sum == hist_sum(&s_rtc) &&
            s_rtc.h.count > 0 && s_rtc.h.count <= HIST_N &&
            now - s_rtc.last_epoch < (time_t)HIST_N * HIST_STEP_S &&
            now >= s_rtc.last_epoch) {
            s_hist = s_rtc.h;
            ESP_LOGI(TAG, "restored %d history samples from RTC RAM", s_hist.count);
        }
    }
    if (s_hist.count == HIST_N) {
        memmove(s_hist.s, s_hist.s + 1, HIST_N - 1);
        memmove(s_hist.w, s_hist.w + 1, HIST_N - 1);
        s_hist.count--;
    }
    s_hist.s[s_hist.count] = clamp_pct(s);
    s_hist.w[s_hist.count] = clamp_pct(w);
    s_hist.count++;
    if (now > 1600000000) {
        s_rtc.magic = HIST_MAGIC;
        s_rtc.last_epoch = now;
        s_rtc.h = s_hist;
        s_rtc.sum = hist_sum(&s_rtc);
    }
    unlock();
}

/* ------------------------------------------------------------------ */
/* Spoken status                                                       */
/* ------------------------------------------------------------------ */
static const char *window_at(const usage_t *u) { return u->session_pct >= u->weekly_pct ? "session_at" : "weekly_at"; }

static void say_status_of(const usage_t *u)
{
    if (!u->ok && u->err == POLL_AUTH) { app_say("token_bad", MELODY_ERROR); return; }
    if (!u->ok && u->err == POLL_NET)  { app_say("offline", MELODY_ERROR); return; }
    if (!u->have_data) return;
    char s[120];
    time_t now = time(NULL);
    int n = snprintf(s, sizeof(s), "session_at #%d percent .", u->session_pct);
    if (u->session_reset > now && now > 1600000000)
        n += snprintf(s + n, sizeof(s) - n, " resets_in ~%ld .", (long)(u->session_reset - now));
    n += snprintf(s + n, sizeof(s) - n, " weekly_at #%d percent .", u->weekly_pct);
    /* pace verdict for the session window (same rule as the PACE page) */
    if (u->session_reset > now && now > 1600000000) {
        long left = (long)(u->session_reset - now);
        if (left > WINDOW_5H_S) left = WINDOW_5H_S;
        int d = u->session_pct - (int)((WINDOW_5H_S - left) * 100 / WINDOW_5H_S);
        snprintf(s + n, sizeof(s) - n, " %s", d > 10 ? "ahead" : d < -10 ? "room" : "on_pace");
    }
    app_say(s, MELODY_POLL_OK);
}

void usage_say_status(void)
{
    usage_t u;
    usage_get(&u);
    say_status_of(&u);
}

/* ------------------------------------------------------------------ */
/* Poll task                                                           */
/* ------------------------------------------------------------------ */
static void set_polling(bool on)
{
    lock();
    s_u.polling = on;
    s_u.seq++;
    unlock();
}

static poll_result_t do_poll(usage_src_t *src)
{
    char token[TOKEN_MAX];
    settings_get_token(token, sizeof(token));
    if (!token[0]) return POLL_AUTH;

    char *auth = malloc(TOKEN_MAX + 16);
    if (!auth) return POLL_NET;
    snprintf(auth, TOKEN_MAX + 16, "Bearer %s", token);

    sample_t smp;
    int r = -1;
    if (esp_timer_get_time() >= s_api_holdoff_until) {
        r = poll_usage_api(auth, &smp);
        *src = SRC_USAGE_API;
    }
    if (r < 0) {
        r = poll_headers(auth, &smp);
        *src = SRC_HEADERS;
    }
    free(auth);
    if (r != POLL_OK) return (poll_result_t)r;

    lock();
    s_u.session_pct   = smp.s_pct;
    s_u.session_reset = smp.s_reset;
    s_u.weekly_pct    = smp.w_pct;
    s_u.weekly_reset  = smp.w_reset;
    s_u.src           = *src;
    s_u.last_ok_us    = esp_timer_get_time();
    unlock();
    ESP_LOGI(TAG, "session=%d%% weekly=%d%% via %s",
             smp.s_pct, smp.w_pct, usage_src_str(*src));
    return POLL_OK;
}

static void sample_rssi(void)
{
    wifi_ap_record_t ap;
    g_sys.rssi = (g_sys.wifi_up && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
}

static void poll_task(void *arg)
{
    (void)arg;
    int prev_s = -1, prev_w = -1;
    poll_result_t prev_res = POLL_OK;
    int64_t last_hist_us = 0;

    for (;;) {
        power_sample();
        sample_rssi();

        set_polling(true);
        usage_src_t src = SRC_NONE;
        poll_result_t res = do_poll(&src);

        usage_t u;
        lock();
        s_u.polling = false;
        s_u.ok      = (res == POLL_OK);
        s_u.err     = res;
        if (res == POLL_OK) s_u.have_data = true;
        s_u.seq++;
        u = s_u;
        unlock();

        if (g_sys.boot == BOOT_FETCH) g_sys.boot = BOOT_READY;

        if (res == POLL_AUTH || res == POLL_NET) {
            led_show(LED_ERROR);
            /* Edge-triggered: an error that persists for hours on battery
             * must not re-wake the screen and beep every poll. */
            if (res != prev_res && !s_announce) {
                screen_wake();
                if (res == POLL_AUTH) net_notify_host();   /* ask the Mac for a token */
                app_say(res == POLL_AUTH ? "token_bad" : "offline", MELODY_ERROR);
            } else if (res != prev_res) {
                screen_wake();
                if (res == POLL_AUTH) net_notify_host();
            }
        } else if (res == POLL_OK) {
            if (prev_res != POLL_OK) {                     /* recovered */
                screen_wake();
                if (prev_res == POLL_NET && !s_announce) app_say("back", MELODY_NONE);
            }
            int worst = u.session_pct > u.weekly_pct ? u.session_pct : u.weekly_pct;
            int prev  = prev_s > prev_w ? prev_s : prev_w;
            led_show_for_pct(worst);

            if (prev >= 0) {
                char say[96];
                if (worst >= 100 && prev < 100) {
                    time_t rst = u.session_pct >= 100 ? u.session_reset : u.weekly_reset;
                    time_t now = time(NULL);
                    if (rst > now && now > 1600000000)
                        snprintf(say, sizeof(say), "limit . back_in ~%ld", (long)(rst - now));
                    else
                        snprintf(say, sizeof(say), "limit");
                    app_say(say, MELODY_THRESHOLD_85);
                } else if (worst >= USAGE_RED_PCT && prev < USAGE_RED_PCT) {
                    snprintf(say, sizeof(say), "warning %s #%d percent", window_at(&u), worst);
                    app_say(say, MELODY_THRESHOLD_85);
                } else if (worst >= USAGE_AMBER_PCT && prev < USAGE_AMBER_PCT) {
                    snprintf(say, sizeof(say), "heads_up %s #%d percent", window_at(&u), worst);
                    app_say(say, MELODY_THRESHOLD_60);
                }

                /* A window rolled over: usage fell sharply from a real level. */
                bool s_reset = prev_s >= 25 && u.session_pct + 15 < prev_s;
                bool w_reset = prev_w >= 25 && u.weekly_pct + 15 < prev_w;
                if (s_reset || w_reset) {
                    lock(); s_u.resets++; s_u.seq++; unlock();
                    app_say("fresh", MELODY_RESET);
                }
                if (u.session_pct != prev_s || u.weekly_pct != prev_w)
                    screen_wake();
            }
            prev_s = u.session_pct;
            prev_w = u.weekly_pct;
        }
        prev_res = res;

        if (s_announce) {                   /* hold-to-refresh: read it out */
            s_announce = false;
            usage_t now_u;
            usage_get(&now_u);
            say_status_of(&now_u);
        }

        /* History: one sample per HIST_STEP_S of uptime, first one at once. */
        int64_t now = esp_timer_get_time();
        if (u.have_data &&
            (last_hist_us == 0 || now - last_hist_us >= (int64_t)HIST_STEP_S * 1000000)) {
            hist_push(u.session_pct, u.weekly_pct);
            last_hist_us = now;
        }

        /* Wait for the next poll; wake early on a forced poll; re-check the
         * power source every ~2 s so the interval adapts mid-wait. */
        for (int waited = 0;; waited++) {
            vTaskDelay(pdMS_TO_TICKS(100));
            if (s_force) { s_force = false; break; }
            if (waited % 20 == 0) power_eval();
            if (last_hist_us && u.have_data &&
                esp_timer_get_time() - last_hist_us >= (int64_t)HIST_STEP_S * 1000000) {
                usage_t cur; usage_get(&cur);
                hist_push(cur.session_pct, cur.weekly_pct);
                last_hist_us = esp_timer_get_time();
            }
            int limit = src == SRC_HEADERS ? POLL_FALLBACK_S
                      : g_sys.ext_power ? POLL_USB_S : POLL_BATT_S;
            if (!g_sys.ext_power && limit < POLL_BATT_S) limit = POLL_BATT_S;
            if (waited >= limit * 10) break;
        }
    }
}

void usage_start(void)
{
    xTaskCreate(poll_task, "poll", 8192, NULL, 5, NULL);
}
