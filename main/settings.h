/*
 * settings.h  --  persistent user settings + the OAuth token (NVS "cfg").
 * All accessors are thread-safe (one mutex); callers get copies.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TOKEN_MAX   512

typedef struct {
    char     tz[48];        /* POSIX TZ string                               */
    uint8_t  volume;        /* 0..100 codec volume                           */
    bool     mute;
    bool     quiet;         /* quiet hours enabled                           */
    uint8_t  quiet_from;    /* hour 0..23, sounds suppressed from..to        */
    uint8_t  quiet_to;
    uint8_t  bl_usb;        /* backlight % on external power                 */
    uint8_t  bl_batt;       /* backlight % on battery                        */
    uint16_t blank_s;       /* screen blank after idle seconds, 0 = never    */
    bool     h24;           /* 24-hour clock                                 */
    bool     talk;          /* spoken announcements (else tones)             */
    bool     listen;        /* "Jarvis" wake word + voice commands           */
    bool     listen_batt;   /* ...also on battery (default on; costs battery) */
    bool     brain;         /* unmatched speech -> brain_server.py on the Mac */
    bool     wit;           /* unprompted remarks now and then (v2.4)        */
    char     notify_ip[16]; /* host that last pushed a token (online-notify) */
} settings_t;

void settings_init(void);                       /* load from NVS (call once) */
void settings_get(settings_t *out);
void settings_put(const settings_t *in);        /* persist + apply TZ/volume */

void settings_get_token(char *buf, size_t n);
void settings_set_token(const char *tok);
bool settings_has_token(void);
void settings_set_notify_ip(const char *ip);

bool settings_quiet_now(void);                  /* mute or inside quiet hours */
bool settings_muted(void);                      /* mute only                  */
