/*
 * net.h  --  Wi-Fi (NVS creds -> secrets.h -> setup AP), SNTP, mDNS, and the
 * device -> host "please push me a token" notification.
 */
#pragma once

#include <stdbool.h>

#define NOTIFY_PORT          5555
/* Used until a host has pushed a token (then the pusher's IP is remembered). */
#define NOTIFY_HOST_DEFAULT  "http://ArcTrooper.local:5555/notify"

void net_start(void);                   /* spawns the bring-up task          */
void net_notify_host(void);             /* fire-and-forget GET to the host   */
bool net_set_wifi(const char *ssid, const char *pass);  /* persist; caller reboots */
const char *net_ap_password(void);      /* setup-AP password ("" if none)    */
