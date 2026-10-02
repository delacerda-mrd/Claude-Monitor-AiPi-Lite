/*
 * settings.c  --  NVS-backed settings + token. Namespace "cfg"; the token key
 * ("token") is unchanged from v1 so a token saved by older firmware survives.
 */
#include "settings.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs.h"

#include "audio.h"
#include "secrets_compat.h"

static const char *TAG = "settings";

#define NS "cfg"

static SemaphoreHandle_t s_mtx;
static settings_t        s_cfg;
static char              s_token[TOKEN_MAX];

static const settings_t DEFAULTS = {
    .tz         = "EST5EDT,M3.2.0,M11.1.0",   /* US Eastern (the Linux box ran EDT) */
    .volume     = 70,
    .mute       = false,
    .quiet      = false,
    .quiet_from = 23,
    .quiet_to   = 7,
    .bl_usb     = 80,
    .bl_batt    = 15,
    .blank_s    = 180,
    .h24        = false,
    .talk       = true,
    .notify_ip  = "",
};

static void lock(void)   { xSemaphoreTake(s_mtx, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mtx); }

static void get_u8(nvs_handle_t h, const char *k, uint8_t *v)
{
    uint8_t t;
    if (nvs_get_u8(h, k, &t) == ESP_OK) *v = t;
}

static void get_bool(nvs_handle_t h, const char *k, bool *v)
{
    uint8_t t;
    if (nvs_get_u8(h, k, &t) == ESP_OK) *v = t != 0;
}

static void get_str(nvs_handle_t h, const char *k, char *v, size_t n)
{
    size_t len = n;
    char tmp[64];
    if (n > sizeof(tmp)) n = sizeof(tmp);
    len = n;
    if (nvs_get_str(h, k, tmp, &len) == ESP_OK) {
        strncpy(v, tmp, n - 1);
        v[n - 1] = '\0';
    }
}

static void apply_side_effects(const settings_t *c)
{
    setenv("TZ", c->tz, 1);
    tzset();
    audio_set_volume(c->volume);
}

static void save_token_nvs(const char *tok)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed");
        return;
    }
    esp_err_t e1 = nvs_set_str(h, "token", tok);
    esp_err_t e2 = nvs_commit(h);
    if (e1 != ESP_OK || e2 != ESP_OK)
        ESP_LOGE(TAG, "token write failed: set=%s commit=%s",
                 esp_err_to_name(e1), esp_err_to_name(e2));
    nvs_close(h);
}

void settings_init(void)
{
    s_mtx = xSemaphoreCreateMutex();
    s_cfg = DEFAULTS;

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        get_str(h, "tz", s_cfg.tz, sizeof(s_cfg.tz));
        get_u8(h, "vol", &s_cfg.volume);
        get_bool(h, "mute", &s_cfg.mute);
        get_bool(h, "quiet", &s_cfg.quiet);
        get_u8(h, "q_from", &s_cfg.quiet_from);
        get_u8(h, "q_to", &s_cfg.quiet_to);
        get_u8(h, "bl_usb", &s_cfg.bl_usb);
        get_u8(h, "bl_batt", &s_cfg.bl_batt);
        uint16_t b;
        if (nvs_get_u16(h, "blank_s", &b) == ESP_OK) s_cfg.blank_s = b;
        get_bool(h, "h24", &s_cfg.h24);
        get_bool(h, "talk", &s_cfg.talk);
        get_str(h, "notify_ip", s_cfg.notify_ip, sizeof(s_cfg.notify_ip));

        size_t len = sizeof(s_token);
        if (nvs_get_str(h, "token", s_token, &len) == ESP_OK)
            ESP_LOGI(TAG, "token loaded from NVS (%u chars)", (unsigned)strlen(s_token));
        nvs_close(h);
    }

    /* First boot with a token compiled in: seed NVS from secrets.h. */
    if (!s_token[0] && SECRETS_HAVE_TOKEN) {
        strncpy(s_token, CLAUDE_TOKEN, sizeof(s_token) - 1);
        save_token_nvs(s_token);
        ESP_LOGI(TAG, "token seeded from secrets.h");
    }
    if (!s_token[0]) ESP_LOGW(TAG, "no token yet - push one to http://claude-meter.local/");

    apply_side_effects(&s_cfg);
}

void settings_get(settings_t *out)
{
    lock();
    *out = s_cfg;
    unlock();
}

void settings_put(const settings_t *in)
{
    settings_t c = *in;
    /* clamp */
    if (c.volume > 100) c.volume = 100;
    if (c.quiet_from > 23) c.quiet_from = 23;
    if (c.quiet_to > 23) c.quiet_to = 23;
    if (c.bl_usb < 5) c.bl_usb = 5;
    if (c.bl_usb > 100) c.bl_usb = 100;
    if (c.bl_batt < 5) c.bl_batt = 5;
    if (c.bl_batt > 100) c.bl_batt = 100;
    c.tz[sizeof(c.tz) - 1] = '\0';
    c.notify_ip[sizeof(c.notify_ip) - 1] = '\0';

    lock();
    s_cfg = c;
    unlock();

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "tz", c.tz);
        nvs_set_u8(h, "vol", c.volume);
        nvs_set_u8(h, "mute", c.mute);
        nvs_set_u8(h, "quiet", c.quiet);
        nvs_set_u8(h, "q_from", c.quiet_from);
        nvs_set_u8(h, "q_to", c.quiet_to);
        nvs_set_u8(h, "bl_usb", c.bl_usb);
        nvs_set_u8(h, "bl_batt", c.bl_batt);
        nvs_set_u16(h, "blank_s", c.blank_s);
        nvs_set_u8(h, "h24", c.h24);
        nvs_set_u8(h, "talk", c.talk);
        nvs_set_str(h, "notify_ip", c.notify_ip);
        nvs_commit(h);
        nvs_close(h);
    }
    apply_side_effects(&c);
}

void settings_get_token(char *buf, size_t n)
{
    lock();
    strncpy(buf, s_token, n - 1);
    buf[n - 1] = '\0';
    unlock();
}

void settings_set_token(const char *tok)
{
    lock();
    strncpy(s_token, tok, sizeof(s_token) - 1);
    s_token[sizeof(s_token) - 1] = '\0';
    unlock();
    save_token_nvs(tok);
    ESP_LOGI(TAG, "token saved (%u chars)", (unsigned)strlen(tok));
}

bool settings_has_token(void)
{
    lock();
    bool has = s_token[0] != '\0';
    unlock();
    return has;
}

void settings_set_notify_ip(const char *ip)
{
    settings_t c;
    settings_get(&c);
    if (strcmp(c.notify_ip, ip) == 0) return;   /* don't wear flash */
    strncpy(c.notify_ip, ip, sizeof(c.notify_ip) - 1);
    c.notify_ip[sizeof(c.notify_ip) - 1] = '\0';
    settings_put(&c);
    ESP_LOGI(TAG, "notify host -> %s", ip);
}

bool settings_quiet_now(void)
{
    settings_t c;
    settings_get(&c);
    if (c.mute) return true;
    if (!c.quiet) return false;
    time_t now = time(NULL);
    if (now < 1600000000) return false;          /* no clock yet */
    struct tm tm;
    localtime_r(&now, &tm);
    int h = tm.tm_hour, a = c.quiet_from, b = c.quiet_to;
    if (a == b) return false;
    return a < b ? (h >= a && h < b) : (h >= a || h < b);   /* wraps midnight */
}
