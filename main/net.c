/*
 * net.c  --  network bring-up, run as a one-shot task so the main loop keeps
 * animating the splash while Wi-Fi joins.
 *
 * Credentials: the ESP-IDF Wi-Fi driver persists the STA config in NVS
 * (namespace nvs.net80211). v1 wrote it there on every boot from secrets.h,
 * so a device upgraded from v1 already has it. Order of preference:
 *   1. config already stored in NVS
 *   2. secrets.h WIFI_SSID / WIFI_PASSWORD (factory-fresh device)
 *   3. none -> setup access point
 * If the stored network can't be joined within 20 s the setup AP comes up
 * too, while the STA keeps retrying; whichever wins (a reconnect or new
 * credentials from the setup page) restarts the device cleanly.
 */
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "esp_http_client.h"
#include "lwip/sockets.h"
#include "mdns.h"

#include "app.h"
#include "secrets_compat.h"
#include "settings.h"
#include "usage.h"
#include "web.h"

static const char *TAG = "net";

#define CONNECTED_BIT   BIT0
#define FAILED_BIT      BIT1
#define BOOT_RETRY_MAX  10
#define SETUP_RETRY_S   30

static EventGroupHandle_t s_eg;
static int   s_retries;
static bool  s_ever_connected;
static volatile bool s_setup_mode;
static char  s_ap_pass[12];

const char *net_ap_password(void) { return s_ap_pass; }

/* ------------------------------------------------------------------ */
/* Host notify                                                         */
/* ------------------------------------------------------------------ */
static void notify_task(void *arg)
{
    char *url = arg;
    esp_http_client_config_t cfg = { .url = url, .timeout_ms = 5000 };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c) {
        esp_err_t err = esp_http_client_perform(c);
        if (err == ESP_OK)
            ESP_LOGI(TAG, "host notified (%s): HTTP %d", url, esp_http_client_get_status_code(c));
        else
            ESP_LOGW(TAG, "host notify failed (%s): %s", url, esp_err_to_name(err));
        esp_http_client_cleanup(c);
    }
    free(url);
    vTaskDelete(NULL);
}

void net_notify_host(void)
{
    if (!g_sys.wifi_up) return;
    settings_t c;
    settings_get(&c);
    char *url = malloc(64);
    if (!url) return;
    if (c.notify_ip[0])
        snprintf(url, 64, "http://%s:%d/notify", c.notify_ip, NOTIFY_PORT);
    else
        snprintf(url, 64, "%s", NOTIFY_HOST_DEFAULT);
    if (xTaskCreate(notify_task, "notify", 4096, url, 2, NULL) != pdPASS) free(url);
}

/* ------------------------------------------------------------------ */
/* Wi-Fi events                                                        */
/* ------------------------------------------------------------------ */
static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (!s_setup_mode) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        g_sys.wifi_up = false;
        g_sys.rssi = 0;
        if (s_setup_mode) return;               /* setup loop retries slowly */
        if (s_ever_connected || s_retries < BOOT_RETRY_MAX) {
            s_retries++;
            ESP_LOGW(TAG, "Wi-Fi retry %d%s", s_retries,
                     s_ever_connected ? " (reconnecting)" : "");
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_eg, FAILED_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        snprintf(g_sys.ip, sizeof(g_sys.ip), IPSTR, IP2STR(&ev->ip_info.ip));
        ESP_LOGI(TAG, "IP %s", g_sys.ip);
        s_retries = 0;
        s_ever_connected = true;
        g_sys.wifi_up = true;
        esp_wifi_set_ps(g_sys.ext_power ? WIFI_PS_MIN_MODEM : WIFI_PS_MAX_MODEM);
        xEventGroupSetBits(s_eg, CONNECTED_BIT);
        net_notify_host();                      /* "I'm online" (v1 behavior) */
    }
}

bool net_set_wifi(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0] || strlen(ssid) > 32 || (pass && strlen(pass) > 63)) return false;
    wifi_config_t w = {0};
    strncpy((char *)w.sta.ssid, ssid, sizeof(w.sta.ssid));
    if (pass) strncpy((char *)w.sta.password, pass, sizeof(w.sta.password));
    w.sta.threshold.authmode = (pass && pass[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &w);   /* persisted to NVS */
    ESP_LOGI(TAG, "new Wi-Fi config for \"%s\": %s", ssid, esp_err_to_name(e));
    return e == ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Captive DNS: answer every A query with the AP address               */
/* ------------------------------------------------------------------ */
static void dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { vTaskDelete(NULL); return; }
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(53),
                             .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(sock, (struct sockaddr *)&a, sizeof(a)) < 0) {
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    uint8_t buf[512];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &fl);
        if (n < 12) continue;
        /* find end of the question's QNAME */
        int p = 12;
        while (p < n && buf[p]) p += buf[p] + 1;
        p += 5;                                 /* null + QTYPE + QCLASS */
        if (p > n) continue;
        buf[2] = 0x81; buf[3] = 0x80;           /* response, no error       */
        buf[6] = 0; buf[7] = 1;                 /* ANCOUNT = 1              */
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        static const uint8_t ans[] = {
            0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, /* ptr to name, A, IN       */
            0x00, 0x00, 0x00, 0x3C, 0x00, 0x04, /* TTL 60, RDLENGTH 4       */
            192, 168, 4, 1,
        };
        memcpy(buf + p, ans, sizeof(ans));
        sendto(sock, buf, p + sizeof(ans), 0, (struct sockaddr *)&from, fl);
    }
}

static void start_setup_ap(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(g_sys.ap_ssid, sizeof(g_sys.ap_ssid), "Claude-Meter-%02X%02X", mac[4], mac[5]);
    /* Fresh 8-digit password per boot, shown only on the device's screen:
     * joining requires physical access. */
    snprintf(s_ap_pass, sizeof(s_ap_pass), "%08lu",
             (unsigned long)(esp_random() % 100000000UL));

    wifi_config_t ap = {0};
    strncpy((char *)ap.ap.ssid, g_sys.ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(g_sys.ap_ssid);
    strncpy((char *)ap.ap.password, s_ap_pass, sizeof(ap.ap.password));
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 2;
    ap.ap.channel = 1;

    s_setup_mode = true;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    esp_wifi_start();
    xTaskCreate(dns_task, "dns", 3072, NULL, 3, NULL);
    ESP_LOGW(TAG, "setup AP \"%s\" up at 192.168.4.1", g_sys.ap_ssid);
}

/* ------------------------------------------------------------------ */
/* Bring-up                                                            */
/* ------------------------------------------------------------------ */
static void sntp_sync(void)
{
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.apple.com");
    esp_sntp_init();
    for (int i = 0; i < 20; i++) {
        if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
            ESP_LOGI(TAG, "SNTP synced");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGW(TAG, "SNTP timeout - countdowns wait for the clock");
}

static void mdns_setup(void)
{
    if (mdns_init() != ESP_OK) return;
    mdns_hostname_set("claude-meter");
    mdns_instance_name_set("Claude Meter");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
}

static void net_task(void *arg)
{
    (void)arg;
    g_sys.boot = BOOT_WIFI;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_config_t sta = {0};
    esp_wifi_get_config(WIFI_IF_STA, &sta);     /* NVS-stored config, if any */
    if (!sta.sta.ssid[0] && SECRETS_HAVE_WIFI) {
        ESP_LOGI(TAG, "no stored Wi-Fi config - seeding from secrets.h");
        net_set_wifi(WIFI_SSID, WIFI_PASSWORD);
        esp_wifi_get_config(WIFI_IF_STA, &sta);
    }
    bool have_creds = sta.sta.ssid[0] != '\0';
    if (have_creds) {
        strncpy(g_sys.ssid, (const char *)sta.sta.ssid, sizeof(g_sys.ssid) - 1);
        ESP_LOGI(TAG, "joining \"%s\"", g_sys.ssid);
    }

    EventBits_t bits = 0;
    if (have_creds) {
        ESP_ERROR_CHECK(esp_wifi_start());
        bits = xEventGroupWaitBits(s_eg, CONNECTED_BIT | FAILED_BIT, pdFALSE, pdFALSE,
                                   pdMS_TO_TICKS(20000));
    }

    if (!(bits & CONNECTED_BIT)) {
        /* No network: raise the setup AP, keep trying the old one slowly.
         * Deliberately never confirm an OTA image on this path -- an update
         * that breaks Wi-Fi rolls back on the next reset. */
        if (have_creds) esp_wifi_stop();
        start_setup_ap();
        g_sys.boot = BOOT_SETUP;        /* after the AP name/password exist */
        web_start();
        app_say("setup", MELODY_SETUP);
        for (int t = 0;; t++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (xEventGroupGetBits(s_eg) & CONNECTED_BIT) {
                ESP_LOGW(TAG, "Wi-Fi came up during setup - restarting");
                vTaskDelay(pdMS_TO_TICKS(300));
                esp_restart();
            }
            if (have_creds && t % SETUP_RETRY_S == SETUP_RETRY_S - 1) esp_wifi_connect();
        }
    }

    g_sys.boot = BOOT_TIME;
    sntp_sync();
    mdns_setup();
    web_start();
    g_sys.boot = BOOT_FETCH;
    app_say("@online/online", MELODY_BOOT);
    usage_start();
    vTaskDelete(NULL);
}

void net_start(void)
{
    s_eg = xEventGroupCreate();
    xTaskCreate(net_task, "net", 6144, NULL, 4, NULL);
}
