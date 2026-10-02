/*
 * secrets_compat.h  --  makes main/secrets.h optional.
 *
 * v2 keeps Wi-Fi credentials and the token in NVS, so a build without
 * secrets.h is normal: the device reuses the Wi-Fi config it already has,
 * or opens a setup access point. secrets.h, when present, only seeds a
 * factory-fresh device (and may set CFG_AUTH_SECRET).
 */
#pragma once

#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif
#ifndef CLAUDE_TOKEN
#define CLAUDE_TOKEN ""
#endif

/* secrets.h.example placeholders count as "not set". */
#define SECRETS_HAVE_WIFI  (WIFI_SSID[0] != '\0' && strcmp(WIFI_SSID, "your-wifi-ssid") != 0)
#define SECRETS_HAVE_TOKEN (CLAUDE_TOKEN[0] != '\0' && strcmp(CLAUDE_TOKEN, "your-claude-oauth-access-token") != 0)
