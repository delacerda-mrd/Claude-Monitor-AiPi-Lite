/*
 * voice.c  --  speech from a mu-law clip pack in the "voice" partition.
 *
 * The pack index (~4 KB) is cached in RAM; sample data is streamed from flash
 * in small chunks, decoded to 16-bit and handed to audio_stream_write(), so a
 * sentence costs a few hundred bytes of RAM no matter how long it is.
 */
#include "voice.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"

#include "audio.h"

static const char *TAG = "voice";

#define PACK_MAGIC      "CMVP"
#define PACK_RATE       16000
#define NAME_LEN        20
#define GAP_MS          55          /* between clips                     */
#define PAUSE_MS        260         /* "." token                          */
#define MAX_CLIPS_SAID  24          /* clips per script                   */

typedef struct __attribute__((packed)) {
    char     magic[4];
    uint16_t version;
    uint16_t count;
    uint32_t rate;
    char     voice[24];
} pack_hdr_t;

typedef struct __attribute__((packed)) {
    char     name[NAME_LEN];
    uint32_t offset;
    uint32_t length;
} pack_entry_t;

static const esp_partition_t *s_part;
static pack_entry_t          *s_index;
static int                    s_count;
static char                   s_voice[24];
static volatile bool          s_ready;

/* ------------------------------------------------------------------ */
/* Index                                                               */
/* ------------------------------------------------------------------ */
bool voice_init(void)
{
    s_ready = false;
    if (!s_part)
        s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "voice");
    if (!s_part) {
        ESP_LOGW(TAG, "no voice partition (flash the v2.1 partition table over USB)");
        return false;
    }
    pack_hdr_t h;
    if (esp_partition_read(s_part, 0, &h, sizeof(h)) != ESP_OK ||
        memcmp(h.magic, PACK_MAGIC, 4) != 0 || h.version != 1 ||
        h.rate != PACK_RATE || h.count == 0 || h.count > 1024) {
        ESP_LOGW(TAG, "no voice pack installed - using tones");
        return false;
    }
    pack_entry_t *idx = malloc(sizeof(pack_entry_t) * h.count);
    if (!idx) return false;
    if (esp_partition_read(s_part, sizeof(h), idx, sizeof(pack_entry_t) * h.count) != ESP_OK) {
        free(idx);
        return false;
    }
    free(s_index);
    s_index = idx;
    s_count = h.count;
    memcpy(s_voice, h.voice, sizeof(s_voice) - 1);
    s_voice[sizeof(s_voice) - 1] = '\0';
    s_ready = true;
    ESP_LOGI(TAG, "voice pack \"%s\": %d clips", s_voice, s_count);
    return true;
}

bool        voice_ready(void)      { return s_ready; }
const char *voice_name(void)       { return s_ready ? s_voice : ""; }
int         voice_clip_count(void) { return s_ready ? s_count : 0; }

static const pack_entry_t *find(const char *name)
{
    for (int i = 0; i < s_count; i++)
        if (strncmp(s_index[i].name, name, NAME_LEN) == 0) return &s_index[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Script -> clip list                                                 */
/* ------------------------------------------------------------------ */
typedef struct { const pack_entry_t *clip; int pause_ms; } step_t;

static int add_clip(step_t *st, int n, const char *name)
{
    if (n >= MAX_CLIPS_SAID) return n;
    const pack_entry_t *e = find(name);
    if (!e) { ESP_LOGW(TAG, "missing clip \"%s\"", name); return n; }
    st[n].clip = e;
    st[n].pause_ms = GAP_MS;
    return n + 1;
}

static int add_num(step_t *st, int n, long v)
{
    char b[8];
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    snprintf(b, sizeof(b), "%ld", v);
    return add_clip(st, n, b);
}

static int add_unit(step_t *st, int n, long v, const char *one, const char *many)
{
    n = add_num(st, n, v);
    return add_clip(st, n, v == 1 ? one : many);
}

/* "2 days 4 hours" / "3 hours 12 minutes" / "40 minutes" */
static int add_duration(step_t *st, int n, long s)
{
    if (s < 60) s = 60;
    long d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60;
    if (d > 0) {
        n = add_unit(st, n, d, "day", "days");
        if (h > 0) n = add_unit(st, n, h, "hour", "hours");
    } else if (h > 0) {
        n = add_unit(st, n, h, "hour", "hours");
        if (m > 0) n = add_unit(st, n, m, "minute", "minutes");
    } else {
        n = add_unit(st, n, m, "minute", "minutes");
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Playback                                                            */
/* ------------------------------------------------------------------ */
static int16_t mulaw_decode(uint8_t u)
{
    u = ~u;
    int t = ((u & 0x0F) << 3) + 0x84;
    t <<= (u & 0x70) >> 4;
    return (u & 0x80) ? (int16_t)(0x84 - t) : (int16_t)(t - 0x84);
}

#define CHUNK 256

static void play_silence(int ms)
{
    static int16_t zeros[CHUNK * 2];
    int frames = PACK_RATE * ms / 1000;
    while (frames > 0) {
        int f = frames > CHUNK ? CHUNK : frames;
        audio_stream_write(zeros, f);
        frames -= f;
    }
}

static void play_clip(const pack_entry_t *e)
{
    uint8_t  mu[CHUNK];
    int16_t  st[CHUNK * 2];
    uint32_t off = e->offset, left = e->length;
    while (left) {
        uint32_t n = left > CHUNK ? CHUNK : left;
        if (esp_partition_read(s_part, off, mu, n) != ESP_OK) return;
        for (uint32_t i = 0; i < n; i++) {
            int16_t s = mulaw_decode(mu[i]);
            st[2 * i] = st[2 * i + 1] = s;
        }
        audio_stream_write(st, n);
        off += n;
        left -= n;
    }
}

bool voice_play_script(const char *script)
{
    if (!s_ready || !script) return false;
    step_t steps[MAX_CLIPS_SAID];
    int n = 0;
    char buf[160];
    strncpy(buf, script, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    for (char *save = NULL, *t = strtok_r(buf, " ", &save); t; t = strtok_r(NULL, " ", &save)) {
        if (t[0] == '#')                 n = add_num(steps, n, atol(t + 1));
        else if (t[0] == '~')            n = add_duration(steps, n, atol(t + 1));
        else if (!strcmp(t, ".")) { if (n) steps[n - 1].pause_ms = PAUSE_MS; }
        else                             n = add_clip(steps, n, t);
    }
    if (!n) return false;

    audio_stream_begin();
    for (int i = 0; i < n; i++) {
        play_clip(steps[i].clip);
        play_silence(i == n - 1 ? 30 : steps[i].pause_ms);
    }
    audio_stream_end();
    return true;
}

/* ------------------------------------------------------------------ */
/* Upload                                                              */
/* ------------------------------------------------------------------ */
#define ERASE_BLOCK 0x10000
static size_t s_wr_off, s_wr_total, s_erased;

bool voice_write_begin(size_t total)
{
    if (!s_part) s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "voice");
    if (!s_part || total < sizeof(pack_hdr_t) || total > s_part->size) return false;
    s_ready = false;                    /* nobody reads while we write */
    s_wr_off = 0;
    s_wr_total = total;
    s_erased = 0;
    return true;
}

bool voice_write(const void *data, size_t len)
{
    if (s_wr_off + len > s_wr_total) return false;
    while (s_erased < s_wr_off + len) {         /* erase just ahead of the writes */
        size_t sz = s_part->size - s_erased < ERASE_BLOCK ? s_part->size - s_erased : ERASE_BLOCK;
        if (esp_partition_erase_range(s_part, s_erased, sz) != ESP_OK) return false;
        s_erased += sz;
    }
    if (esp_partition_write(s_part, s_wr_off, data, len) != ESP_OK) return false;
    s_wr_off += len;
    return true;
}

bool voice_write_end(void)
{
    if (s_wr_off != s_wr_total) return false;
    return voice_init();
}
