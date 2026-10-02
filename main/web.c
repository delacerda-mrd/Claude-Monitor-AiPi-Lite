/*
 * web.c  --  config/dashboard server at http://claude-meter.local/
 *
 *   GET  /              dashboard (main/web/index.html, embedded)
 *   GET  /api/status    live JSON for the dashboard
 *   POST /              token=<oauth token>  (form-encoded; v1 contract,
 *                       used by push_claude_token.py) -- also records the
 *                       pusher's IP as the online-notify host
 *   POST /api/settings  JSON settings patch
 *   POST /api/wifi      JSON {ssid, pass}; saves and reboots
 *   POST /api/poll      force a poll now
 *   POST /api/button    {"long":bool} simulate the button (tap = next page)
 *   POST /api/say       {"status":true} read the numbers out, or {"script":"..."}
 *   POST /voice         raw voice pack (tools/make_voice.py) -> "voice" partition
 *   POST /ota           raw firmware .bin -> inactive slot, reboot (v1)
 *   GET  /api/screen.bmp  what the panel shows right now (3x, 24-bit BMP)
 *   GET  <anything else>  -> dashboard (captive portal in setup)
 *
 * Every POST honors CFG_AUTH_SECRET (X-Auth header) when it is defined.
 * Handlers never touch LVGL.
 */
#include "web.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "cJSON.h"

#include "lvgl.h"

#include "app.h"
#include "board.h"
#include "net.h"
#include "secrets_compat.h"
#include "settings.h"
#include "listen.h"
#include "ui.h"
#include "usage.h"
#include "voice.h"

static const char *TAG = "web";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");

static httpd_handle_t s_srv;

#ifdef CFG_AUTH_SECRET
#define AUTH_REQUIRED true
#else
#define AUTH_REQUIRED false
#endif

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */
static bool auth_ok(httpd_req_t *req)
{
#ifdef CFG_AUTH_SECRET
    char hdr[96];
    if (httpd_req_get_hdr_value_str(req, "X-Auth", hdr, sizeof(hdr)) != ESP_OK)
        return false;
    return strcmp(hdr, CFG_AUTH_SECRET) == 0;
#else
    (void)req;
    return true;
#endif
}

static esp_err_t deny(httpd_req_t *req)
{
    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Auth required");
    return ESP_FAIL;
}

/* Read the whole body (bounded). Returns length or -1 after sending an error. */
static int read_body(httpd_req_t *req, char *buf, size_t max)
{
    int total = 0, timeouts = 0;
    while (total < (int)max - 1) {
        int r = httpd_req_recv(req, buf + total, max - 1 - total);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > 5) {
                httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "recv timeout");
                return -1;
            }
            continue;
        }
        timeouts = 0;
        if (r < 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv error");
            return -1;
        }
        if (r == 0) break;
        total += r;
        if (req->content_len && total >= (int)req->content_len) break;
    }
    buf[total] = '\0';
    return total;
}

static void url_decode(char *dst, const char *src, size_t maxlen)
{
    size_t i = 0;
    while (*src && i < maxlen - 1) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = {src[1], src[2], '\0'};
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
        i++;
    }
    *dst = '\0';
}

static const char *form_field(const char *body, const char *name, size_t *out_len)
{
    size_t nl = strlen(name);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, name, nl) == 0 && p[nl] == '=') {
            const char *v = p + nl + 1;
            const char *amp = strchr(v, '&');
            *out_len = amp ? (size_t)(amp - v) : strlen(v);
            return v;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return NULL;
}

/* IPv4 address of the peer (httpd sockets are IPv6, peers IPv4-mapped). */
static bool peer_ipv4(httpd_req_t *req, char *ip, size_t n)
{
    struct sockaddr_storage ss;
    socklen_t sl = sizeof(ss);
    if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&ss, &sl) != 0) return false;
    if (ss.ss_family == AF_INET) {
        struct sockaddr_in *a = (struct sockaddr_in *)&ss;
        return inet_ntop(AF_INET, &a->sin_addr, ip, n) != NULL;
    }
    if (ss.ss_family == AF_INET6) {
        const uint8_t *b = (const uint8_t *)&((struct sockaddr_in6 *)&ss)->sin6_addr;
        if (b[10] != 0xff || b[11] != 0xff) return false;
        snprintf(ip, n, "%u.%u.%u.%u", b[12], b[13], b[14], b[15]);
        return true;
    }
    return false;
}

static const char *ota_state_str(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) != ESP_OK) return "ok";
    switch (st) {
    case ESP_OTA_IMG_NEW:            return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending";
    case ESP_OTA_IMG_VALID:          return "valid";
    case ESP_OTA_IMG_INVALID:        return "invalid";
    case ESP_OTA_IMG_ABORTED:        return "aborted";
    default:                         return "ok";   /* USB-flashed */
    }
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_sendstr(req, s);
    free(s);
    return e;
}

static esp_err_t send_ok(httpd_req_t *req, const char *msg)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    if (msg) cJSON_AddStringToObject(r, "msg", msg);
    return send_json(req, r);
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

/* ------------------------------------------------------------------ */
/* GET                                                                 */
/* ------------------------------------------------------------------ */
static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t status_get(httpd_req_t *req)
{
    usage_t u;
    usage_hist_t h;
    settings_t c;
    usage_get(&u);
    usage_hist_get(&h);
    settings_get(&c);
    const esp_app_desc_t  *app = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();
    int64_t now_us = esp_timer_get_time();

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "fw", app ? app->version : "?");
    cJSON_AddStringToObject(r, "slot", run ? run->label : "?");
    cJSON_AddStringToObject(r, "ota", ota_state_str());
    cJSON_AddStringToObject(r, "mode", g_sys.boot == BOOT_SETUP ? "setup" : "sta");
    cJSON_AddBoolToObject(r, "auth", AUTH_REQUIRED);
    cJSON_AddNumberToObject(r, "now", (double)time(NULL));
    cJSON_AddNumberToObject(r, "up", (double)(now_us / 1000000));
    cJSON_AddNumberToObject(r, "page", ui_page());

    cJSON *us = cJSON_AddObjectToObject(r, "usage");
    cJSON_AddBoolToObject(us, "have", u.have_data);
    cJSON_AddBoolToObject(us, "ok", u.ok);
    cJSON_AddStringToObject(us, "err", u.ok ? "" : usage_err_str(u.err));
    cJSON_AddStringToObject(us, "src", usage_src_str(u.src));
    cJSON_AddNumberToObject(us, "s", u.session_pct);
    cJSON_AddNumberToObject(us, "sr", (double)u.session_reset);
    cJSON_AddNumberToObject(us, "w", u.weekly_pct);
    cJSON_AddNumberToObject(us, "wr", (double)u.weekly_reset);
    cJSON_AddNumberToObject(us, "age", u.last_ok_us ? (double)((now_us - u.last_ok_us) / 1000000) : -1);
    cJSON_AddBoolToObject(us, "token", settings_has_token());
    int hs[HIST_N], hw[HIST_N];
    for (int i = 0; i < h.count; i++) { hs[i] = h.s[i]; hw[i] = h.w[i]; }
    cJSON_AddItemToObject(us, "hs", cJSON_CreateIntArray(hs, h.count));
    cJSON_AddItemToObject(us, "hw", cJSON_CreateIntArray(hw, h.count));
    cJSON_AddNumberToObject(us, "step", HIST_STEP_S);

    cJSON *dv = cJSON_AddObjectToObject(r, "dev");
    cJSON_AddNumberToObject(dv, "batt", g_sys.batt_pct);
    cJSON_AddBoolToObject(dv, "ext", g_sys.ext_power);
    cJSON_AddNumberToObject(dv, "rssi", g_sys.rssi);
    cJSON_AddStringToObject(dv, "ip", g_sys.ip);
    cJSON_AddStringToObject(dv, "ssid", g_sys.ssid);
    cJSON_AddStringToObject(dv, "notify", c.notify_ip[0] ? c.notify_ip : NOTIFY_HOST_DEFAULT);
    cJSON_AddNumberToObject(dv, "heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(dv, "heap_min", esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(dv, "heap_blk", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    cJSON *hwj = cJSON_AddObjectToObject(r, "hw");
    cJSON_AddNumberToObject(hwj, "psram", heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(hwj, "psram_free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(hwj, "internal_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(hwj, "gpio1", gpio_get_level(PIN_BTN_PWR));
    cJSON_AddNumberToObject(hwj, "gpio8", gpio_get_level(PIN_VBUS));
    cJSON_AddNumberToObject(hwj, "gpio21", gpio_get_level(PIN_CHRG));
    cJSON_AddNumberToObject(hwj, "gpio42", gpio_get_level(PIN_BTN));

    cJSON *st = cJSON_AddObjectToObject(r, "set");
    cJSON_AddStringToObject(st, "tz", c.tz);
    cJSON_AddNumberToObject(st, "vol", c.volume);
    cJSON_AddBoolToObject(st, "mute", c.mute);
    cJSON_AddBoolToObject(st, "quiet", c.quiet);
    cJSON_AddNumberToObject(st, "qf", c.quiet_from);
    cJSON_AddNumberToObject(st, "qt", c.quiet_to);
    cJSON_AddNumberToObject(st, "blu", c.bl_usb);
    cJSON_AddNumberToObject(st, "blb", c.bl_batt);
    cJSON_AddNumberToObject(st, "blank", c.blank_s);
    cJSON_AddBoolToObject(st, "h24", c.h24);
    cJSON_AddBoolToObject(st, "talk", c.talk);
    cJSON_AddBoolToObject(st, "listen", c.listen);
    cJSON_AddBoolToObject(st, "listen_batt", c.listen_batt);
    static const char *LS[] = { "off", "idle", "prompt", "command" };
    cJSON *li = cJSON_AddObjectToObject(r, "listen");
    cJSON_AddStringToObject(li, "state", LS[listen_state()]);
    cJSON_AddStringToObject(li, "heard", listen_heard());
    cJSON_AddNumberToObject(li, "level_db", (int)listen_level_db());
    cJSON *vo = cJSON_AddObjectToObject(r, "voice");
    cJSON_AddStringToObject(vo, "name", voice_name());
    cJSON_AddNumberToObject(vo, "clips", voice_clip_count());
    return send_json(req, r);
}

/* The raw mic audio of the last voice-command attempt, as a WAV. */
static esp_err_t rec_get(httpd_req_t *req)
{
    int n = 0;
    const int16_t *pcm = listen_last_take(&n);
    if (!pcm || n <= 0) { httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no recording yet"); return ESP_FAIL; }
    uint32_t data = n * 2, riff = 36 + data, rate = 16000, brate = 32000;
    uint8_t h[44] = {'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,1,0,
                     0,0,0,0,0,0,0,0,2,0,16,0,'d','a','t','a',0,0,0,0};
    memcpy(h + 4, &riff, 4); memcpy(h + 24, &rate, 4); memcpy(h + 28, &brate, 4); memcpy(h + 40, &data, 4);
    httpd_resp_set_type(req, "audio/wav");
    httpd_resp_send_chunk(req, (const char *)h, sizeof(h));
    for (int off = 0; off < n; off += 2048) {
        int k = n - off < 2048 ? n - off : 2048;
        if (httpd_resp_send_chunk(req, (const char *)(pcm + off), k * 2) != ESP_OK) break;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* Screenshot: exactly what was sent to the panel, upscaled 3x. */
#define SHOT_SCALE 3
static esp_err_t screen_get(httpd_req_t *req)
{
    uint16_t *strip[8] = {0};
    int nstrips = 0, rows = 0;
    if (!app_capture_frame(strip, &nstrips, &rows)) return httpd_resp_send_500(req);

    const int W = LCD_W * SHOT_SCALE, H = LCD_H * SHOT_SCALE, row = W * 3;
    uint8_t hdr[54] = {'B', 'M'};
    uint32_t img = (uint32_t)row * H, file = 54 + img;
    memcpy(hdr + 2, &file, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &W, 4);
    memcpy(hdr + 22, &H, 4);
    hdr[26] = 1; hdr[28] = 24;
    memcpy(hdr + 34, &img, 4);

    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send_chunk(req, (const char *)hdr, sizeof(hdr));
    uint8_t *line = malloc(row);
    if (line) {
        for (int y = H - 1; y >= 0; y--) {               /* BMP is bottom-up */
            int sy = y / SHOT_SCALE;
            const uint16_t *src = &strip[sy / rows][(sy % rows) * LCD_W];
            for (int x = 0; x < W; x++) {
                lv_color_t c;
                c.full = src[x / SHOT_SCALE];
                uint32_t c32 = lv_color_to32(c);         /* 0xAARRGGBB */
                line[x * 3 + 0] = c32 & 0xFF;
                line[x * 3 + 1] = (c32 >> 8) & 0xFF;
                line[x * 3 + 2] = (c32 >> 16) & 0xFF;
            }
            if (httpd_resp_send_chunk(req, (const char *)line, row) != ESP_OK) break;
        }
        free(line);
    }
    for (int i = 0; i < nstrips; i++) free(strip[i]);
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* ------------------------------------------------------------------ */
/* POST                                                                */
/* ------------------------------------------------------------------ */
static esp_err_t token_post(httpd_req_t *req)
{
    if (!auth_ok(req)) return deny(req);

    char body[TOKEN_MAX + 64];
    if (read_body(req, body, sizeof(body)) < 0) return ESP_FAIL;

    size_t vlen = 0;
    const char *val = form_field(body, "token", &vlen);
    if (!val) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No token field");
        return ESP_FAIL;
    }
    char enc[TOKEN_MAX];
    if (vlen >= sizeof(enc)) vlen = sizeof(enc) - 1;
    memcpy(enc, val, vlen);
    enc[vlen] = '\0';
    char tok[TOKEN_MAX];
    url_decode(tok, enc, sizeof(tok));
    /* strip whitespace a paste might carry */
    char *e = tok + strlen(tok);
    while (e > tok && (e[-1] == ' ' || e[-1] == '\n' || e[-1] == '\r')) *--e = '\0';
    if (strlen(tok) < 20) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Token too short");
        return ESP_FAIL;
    }

    settings_set_token(tok);
    usage_poll_now();
    app_say("token_new", MELODY_TOKEN_SAVED);

    /* Remember who feeds us tokens: that host gets the online-notify ping. */
    char ip[16];
    if (g_sys.boot != BOOT_SETUP && peer_ipv4(req, ip, sizeof(ip)) &&
        strcmp(ip, "127.0.0.1") != 0) {
        settings_set_notify_ip(ip);
    }

    /* Plain-text reply keeps the v1 script's expectations (HTTP 200). */
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "Token saved. Polling now.\n");
}

static esp_err_t settings_post(httpd_req_t *req)
{
    if (!auth_ok(req)) return deny(req);
    char body[512];
    if (read_body(req, body, sizeof(body)) < 0) return ESP_FAIL;
    cJSON *j = cJSON_Parse(body);
    if (!j) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad JSON");
        return ESP_FAIL;
    }
    settings_t c;
    settings_get(&c);
    cJSON *v;
    if (cJSON_IsString(v = cJSON_GetObjectItem(j, "tz")) && strlen(v->valuestring) < sizeof(c.tz))
        strcpy(c.tz, v->valuestring);
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "vol")))   c.volume     = (uint8_t)v->valueint;
    if (cJSON_IsBool(v = cJSON_GetObjectItem(j, "mute")))    c.mute       = cJSON_IsTrue(v);
    if (cJSON_IsBool(v = cJSON_GetObjectItem(j, "quiet")))   c.quiet      = cJSON_IsTrue(v);
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "qf")))    c.quiet_from = (uint8_t)v->valueint;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "qt")))    c.quiet_to   = (uint8_t)v->valueint;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "blu")))   c.bl_usb     = (uint8_t)v->valueint;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "blb")))   c.bl_batt    = (uint8_t)v->valueint;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "blank"))) c.blank_s    = (uint16_t)v->valueint;
    if (cJSON_IsBool(v = cJSON_GetObjectItem(j, "h24")))     c.h24        = cJSON_IsTrue(v);
    if (cJSON_IsBool(v = cJSON_GetObjectItem(j, "talk")))    c.talk       = cJSON_IsTrue(v);
    if (cJSON_IsBool(v = cJSON_GetObjectItem(j, "listen")))  c.listen     = cJSON_IsTrue(v);
    if (cJSON_IsBool(v = cJSON_GetObjectItem(j, "listen_batt"))) c.listen_batt = cJSON_IsTrue(v);
    bool test = cJSON_IsTrue(cJSON_GetObjectItem(j, "test"));
    cJSON_Delete(j);
    settings_put(&c);
    /* volume preview ignores mute/quiet: the user just asked for it */
    if (test && !(c.talk && audio_say_async("test"))) audio_play_async(MELODY_BUTTON);
    return send_ok(req, "saved");
}

static esp_err_t wifi_post(httpd_req_t *req)
{
    if (!auth_ok(req)) return deny(req);
    char body[256];
    if (read_body(req, body, sizeof(body)) < 0) return ESP_FAIL;
    cJSON *j = cJSON_Parse(body);
    cJSON *ss = j ? cJSON_GetObjectItem(j, "ssid") : NULL;
    cJSON *pw = j ? cJSON_GetObjectItem(j, "pass") : NULL;
    bool ok = cJSON_IsString(ss) &&
              net_set_wifi(ss->valuestring, cJSON_IsString(pw) ? pw->valuestring : "");
    cJSON_Delete(j);
    if (!ok) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad ssid/password");
        return ESP_FAIL;
    }
    send_ok(req, "saved - rebooting");
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

/* Remote button: {"long":true} = hold (refresh), else tap (next page). */
static esp_err_t button_post(httpd_req_t *req)
{
    if (!auth_ok(req)) return deny(req);
    char body[64];
    if (read_body(req, body, sizeof(body)) < 0) return ESP_FAIL;
    cJSON *j = cJSON_Parse(body);
    g_btn_sim = (j && cJSON_IsTrue(cJSON_GetObjectItem(j, "long"))) ? 2 : 1;
    cJSON_Delete(j);
    return send_ok(req, "pressed");
}

/* Speak: {"status":true} reads the numbers, {"script":"..."} anything else. */
static esp_err_t say_post(httpd_req_t *req)
{
    if (!auth_ok(req)) return deny(req);
    char body[192];
    if (read_body(req, body, sizeof(body)) < 0) return ESP_FAIL;
    cJSON *j = cJSON_Parse(body);
    cJSON *sc = j ? cJSON_GetObjectItem(j, "script") : NULL;
    bool ok = true;
    if (cJSON_IsTrue(cJSON_GetObjectItem(j, "tone"))) audio_play_async(MELODY_BOOT);  /* diagnostics */
    else if (cJSON_IsTrue(cJSON_GetObjectItem(j, "listen"))) listen_wake_now();      /* skip the wake word */
    else if (cJSON_IsString(sc)) ok = audio_say_async(sc->valuestring);
    else usage_say_status();
    cJSON_Delete(j);
    if (!ok) { httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no voice pack"); return ESP_FAIL; }
    return send_ok(req, "speaking");
}

/* Voice pack upload: raw body straight into the "voice" partition. */
static esp_err_t voice_post(httpd_req_t *req)
{
    if (!auth_ok(req)) return deny(req);
    if (req->content_len <= 0 || !voice_write_begin(req->content_len)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length or no voice partition");
        return ESP_FAIL;
    }
    static char buf[2048];          /* handlers are serialized -> static is safe */
    int remaining = req->content_len, timeouts = 0;
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf, MIN(remaining, (int)sizeof(buf)));
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > 5) { httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "recv timeout"); return ESP_FAIL; }
            continue;
        }
        timeouts = 0;
        if (r <= 0 || !voice_write(buf, r)) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
            return ESP_FAIL;
        }
        remaining -= r;
    }
    if (!voice_write_end()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "not a voice pack");
        return ESP_FAIL;
    }
    char msg[64];
    snprintf(msg, sizeof(msg), "voice \"%s\" installed, %d clips\n", voice_name(), voice_clip_count());
    httpd_resp_sendstr(req, msg);
    audio_say_async("test");
    return ESP_OK;
}

static esp_err_t poll_post(httpd_req_t *req)
{
    if (!auth_ok(req)) return deny(req);
    usage_poll_now();
    return send_ok(req, "polling");
}

/* Stream the raw .bin into the inactive OTA slot (v1 contract). */
static esp_err_t ota_post(httpd_req_t *req)
{
    if (!auth_ok(req)) return deny(req);
    if (req->content_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content-Length required");
        return ESP_FAIL;
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OTA: receiving %d bytes -> %s", req->content_len, part->label);

    esp_ota_handle_t ota = 0;
    if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_begin failed");
        return ESP_FAIL;
    }
    static char buf[1024];          /* handlers are serialized -> static is safe */
    int remaining = req->content_len, timeouts = 0;
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf, MIN(remaining, (int)sizeof(buf)));
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > 5) {
                esp_ota_abort(ota);
                httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "recv timeout");
                return ESP_FAIL;
            }
            continue;
        }
        timeouts = 0;
        if (r <= 0) {
            esp_ota_abort(ota);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv error");
            return ESP_FAIL;
        }
        if (esp_ota_write(ota, buf, r) != ESP_OK) {
            esp_ota_abort(ota);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "flash write failed");
            return ESP_FAIL;
        }
        remaining -= r;
    }
    if (esp_ota_end(ota) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "image invalid");
        return ESP_FAIL;
    }
    if (esp_ota_set_boot_partition(part) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set_boot failed");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "OTA OK - rebooting\n");
    ESP_LOGW(TAG, "OTA complete, booting %s", part->label);
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

void web_start(void)
{
    if (s_srv) return;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.stack_size       = 10240;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 16;
    cfg.uri_match_fn     = httpd_uri_match_wildcard;
    if (httpd_start(&s_srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        s_srv = NULL;
        return;
    }
    const httpd_uri_t uris[] = {
        { "/",             HTTP_GET,  page_get,      NULL },
        { "/api/status",   HTTP_GET,  status_get,    NULL },
        { "/api/screen.bmp", HTTP_GET, screen_get,   NULL },
        { "/api/rec.wav",  HTTP_GET,  rec_get,       NULL },
        { "/",             HTTP_POST, token_post,    NULL },
        { "/api/settings", HTTP_POST, settings_post, NULL },
        { "/api/wifi",     HTTP_POST, wifi_post,     NULL },
        { "/api/poll",     HTTP_POST, poll_post,     NULL },
        { "/api/button",   HTTP_POST, button_post,   NULL },
        { "/api/say",      HTTP_POST, say_post,      NULL },
        { "/voice",        HTTP_POST, voice_post,    NULL },
        { "/ota",          HTTP_POST, ota_post,      NULL },
        { "/*",            HTTP_GET,  page_get,      NULL },   /* captive portal */
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++)
        httpd_register_uri_handler(s_srv, &uris[i]);
    ESP_LOGI(TAG, "web server on :80");
}
