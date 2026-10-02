/*
 * usage.h  --  Anthropic usage polling (poll task) + shared usage snapshot.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

typedef enum {
    POLL_OK = 0,
    POLL_AUTH,      /* token missing / rejected -> push a fresh one   */
    POLL_NET,       /* network or API failure                         */
} poll_result_t;

typedef enum {
    SRC_NONE = 0,
    SRC_USAGE_API,  /* GET /api/oauth/usage  (free)                    */
    SRC_HEADERS,    /* POST /v1/messages rate-limit headers (1 token)  */
} usage_src_t;

typedef struct {
    bool          have_data;        /* at least one good poll since boot */
    int           session_pct;      /* 5-hour window, may exceed 100     */
    time_t        session_reset;    /* epoch, 0 = unknown                */
    int           weekly_pct;       /* 7-day window                      */
    time_t        weekly_reset;
    bool          ok;               /* last poll succeeded               */
    poll_result_t err;              /* valid when !ok                    */
    usage_src_t   src;
    int64_t       last_ok_us;       /* esp_timer time of last good poll  */
    bool          polling;          /* a request is in flight            */
    uint32_t      seq;              /* bumps on every change (UI dirty)  */
    uint32_t      resets;           /* bumps when a window resets        */
} usage_t;

#define HIST_N       60             /* samples kept                      */
#define HIST_STEP_S  300            /* one sample per 5 min -> 5 hours   */

typedef struct {
    uint8_t s[HIST_N];              /* oldest first                      */
    uint8_t w[HIST_N];
    int     count;
} usage_hist_t;

void usage_init(void);              /* before any usage_get()            */
void usage_start(void);             /* spawn the poll task               */
void usage_get(usage_t *out);
void usage_hist_get(usage_hist_t *out);
void usage_poll_now(void);          /* any task: poll ASAP               */
const char *usage_src_str(usage_src_t s);
const char *usage_err_str(poll_result_t r);
