/*
 * voice.c  --  speech from a mu-law clip pack in the "voice" partition.
 *
 * The pack index (~8 KB) is cached in RAM; sample data is streamed from flash
 * in small chunks, decoded to 16-bit and handed to audio_stream_write(), so a
 * sentence costs a few hundred bytes of RAM no matter how long it is.
 *
 * Pack v1 = mu-law (8 bit), v2 = IMA-ADPCM (4 bit, v2.3). v2 packs also hold
 * fused clips -- "p42" ("forty two percent."), "h3" ("three hours"), "d2",
 * "m15" -- that replace a number + unit join; older packs fall back to the
 * separate words. Clips carry their own soft lead-in/decay, so they are
 * joined with no gap (tools/make_voice.py renders them to fit).
 *
 * "@cat" picks one of the pack's "q_cat_NN" variants at random (v2.4, the
 * meter's personality), never one of the last few it used; "@cat/name" falls
 * back to clip "name" when the pack has no such variants.
 */
#include "voice.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_random.h"

#include "audio.h"

static const char *TAG = "voice";

#define PACK_MAGIC      "CMVP"
#define PACK_RATE       16000
#define NAME_LEN        20
#define GAP_MS          0           /* between clips (they carry padding) */
#define PAUSE_MS        240         /* "." token                          */
#define MAX_CLIPS_SAID  24          /* clips per script                   */
#define MAX_TOKENS      32

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
static int                    s_version;
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
        memcmp(h.magic, PACK_MAGIC, 4) != 0 || (h.version != 1 && h.version != 2) ||
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
    s_version = h.version;
    memcpy(s_voice, h.voice, sizeof(s_voice) - 1);
    s_voice[sizeof(s_voice) - 1] = '\0';
    s_ready = true;
    ESP_LOGI(TAG, "voice pack \"%s\": %d clips (%s)", s_voice, s_count,
             s_version == 2 ? "ADPCM" : "mu-law");
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

static int add_entry(step_t *st, int n, const pack_entry_t *e)
{
    if (n >= MAX_CLIPS_SAID || !e) return n;
    st[n].clip = e;
    st[n].pause_ms = GAP_MS;
    return n + 1;
}

static int add_clip(step_t *st, int n, const char *name)
{
    const pack_entry_t *e = find(name);
    if (!e) ESP_LOGW(TAG, "missing clip \"%s\"", name);
    return add_entry(st, n, e);
}

/* "@cat" -> a random "q_cat_NN" clip, avoiding the last few picks. */
#define QCAT_MAX   40
#define QCAT_HIST  4
static struct { char cat[NAME_LEN]; int16_t hist[QCAT_HIST]; } s_qhist[QCAT_MAX];

static const pack_entry_t *find_variant(const char *cat)
{
    char pre[NAME_LEN];
    int plen = snprintf(pre, sizeof(pre), "q_%s_", cat);
    if (plen >= (int)sizeof(pre)) return NULL;
    int cand[64], nc = 0;
    for (int i = 0; i < s_count && nc < 64; i++) {
        const char *nm = s_index[i].name;
        if (strncmp(nm, pre, plen) == 0 && nm[plen] >= '0' && nm[plen] <= '9') cand[nc++] = i;
    }
    if (!nc) return NULL;

    int h = 0;                                  /* this category's history slot */
    while (h < QCAT_MAX && s_qhist[h].cat[0] && strncmp(s_qhist[h].cat, cat, NAME_LEN)) h++;
    if (h == QCAT_MAX) h = esp_random() % QCAT_MAX;
    if (strncmp(s_qhist[h].cat, cat, NAME_LEN)) {
        strlcpy(s_qhist[h].cat, cat, NAME_LEN);
        for (int k = 0; k < QCAT_HIST; k++) s_qhist[h].hist[k] = -1;
    }
    int avoid = nc / 2 < QCAT_HIST ? nc / 2 : QCAT_HIST;   /* keep some choice */
    int pick = cand[esp_random() % nc];
    for (int tries = 0; tries < 16; tries++) {
        bool recent = false;
        for (int k = 0; k < avoid; k++) recent |= s_qhist[h].hist[k] == pick;
        if (!recent) break;
        pick = cand[esp_random() % nc];
    }
    memmove(&s_qhist[h].hist[1], &s_qhist[h].hist[0], (QCAT_HIST - 1) * sizeof(int16_t));
    s_qhist[h].hist[0] = pick;
    return &s_index[pick];
}

static int add_variant(step_t *st, int n, const char *tok)
{
    char cat[NAME_LEN];
    strlcpy(cat, tok + 1, sizeof(cat));
    char *fb = strchr(cat, '/');
    if (fb) *fb++ = '\0';
    const pack_entry_t *e = find_variant(cat);
    if (e) return add_entry(st, n, e);
    return fb && *fb ? add_clip(st, n, fb) : n;
}

/* A fused clip like "p42" / "h3", if the pack has it. */
static const pack_entry_t *find_fused(char prefix, long v)
{
    char b[8];
    snprintf(b, sizeof(b), "%c%ld", prefix, v);
    return find(b);
}

static int add_num(step_t *st, int n, long v)
{
    char b[8];
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    snprintf(b, sizeof(b), "%ld", v);
    return add_clip(st, n, b);
}

static int add_unit(step_t *st, int n, long v, char prefix, const char *one, const char *many)
{
    const pack_entry_t *e = find_fused(prefix, v);
    if (e) return add_entry(st, n, e);
    n = add_num(st, n, v);
    return add_clip(st, n, v == 1 ? one : many);
}

/* "2 days 4 hours" / "3 hours 12 minutes" / "40 minutes" */
static int add_duration(step_t *st, int n, long s)
{
    if (s < 60) s = 60;
    long d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60;
    if (d > 0) {
        n = add_unit(st, n, d, 'd', "day", "days");
        if (h > 0) n = add_unit(st, n, h, 'h', "hour", "hours");
    } else if (h > 0) {
        n = add_unit(st, n, h, 'h', "hour", "hours");
        if (m > 0) n = add_unit(st, n, m, 'm', "minute", "minutes");
    } else {
        n = add_unit(st, n, m, 'm', "minute", "minutes");
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

/* IMA ADPCM (mirrors adpcm_encode() in tools/make_voice.py) */
static const int16_t IMA_STEPS[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};
static const int8_t IMA_IDX[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

typedef struct { int pred, idx; } ima_t;

static int16_t ima_step(ima_t *s, uint8_t code)
{
    int step = IMA_STEPS[s->idx];
    int diff = step >> 3;
    if (code & 4) diff += step;
    if (code & 2) diff += step >> 1;
    if (code & 1) diff += step >> 2;
    int p = (code & 8) ? s->pred - diff : s->pred + diff;
    s->pred = p < -32768 ? -32768 : p > 32767 ? 32767 : p;
    int i = s->idx + IMA_IDX[code & 7];
    s->idx = i < 0 ? 0 : i > 88 ? 88 : i;
    return (int16_t)s->pred;
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
    uint8_t  raw[CHUNK];
    int16_t  st[CHUNK * 2];                 /* stereo frames */
    uint32_t off = e->offset, left = e->length;
    uint32_t max = s_version == 2 ? CHUNK / 2 : CHUNK;  /* ADPCM: 2 frames per byte */
    ima_t ima = { 0, 0 };
    while (left) {
        uint32_t n = left > max ? max : left, frames = 0;
        if (esp_partition_read(s_part, off, raw, n) != ESP_OK) return;
        for (uint32_t i = 0; i < n; i++) {
            if (s_version == 2) {
                int16_t a = ima_step(&ima, raw[i] & 0x0F), b = ima_step(&ima, raw[i] >> 4);
                st[2 * frames] = st[2 * frames + 1] = a; frames++;
                st[2 * frames] = st[2 * frames + 1] = b; frames++;
            } else {
                st[2 * frames] = st[2 * frames + 1] = mulaw_decode(raw[i]); frames++;
            }
        }
        audio_stream_write(st, frames);
        off += n;
        left -= n;
    }
}

bool voice_play_script(const char *script)
{
    if (!s_ready || !script) return false;
    step_t steps[MAX_CLIPS_SAID];
    int n = 0, nt = 0;
    char buf[160], *tok[MAX_TOKENS];
    strncpy(buf, script, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    for (char *save = NULL, *t = strtok_r(buf, " ", &save); t && nt < MAX_TOKENS; t = strtok_r(NULL, " ", &save))
        tok[nt++] = t;
    for (int k = 0; k < nt; k++) {
        const char *t = tok[k];
        if (t[0] == '#') {
            long v = atol(t + 1);
            const pack_entry_t *p;
            /* "#42 percent" -> the fused "forty two percent." clip */
            if (k + 1 < nt && !strcmp(tok[k + 1], "percent") &&
                v >= 0 && v <= 100 && (p = find_fused('p', v))) {
                n = add_entry(steps, n, p);
                k++;
            } else {
                n = add_num(steps, n, v);
            }
        }
        else if (t[0] == '~')            n = add_duration(steps, n, atol(t + 1));
        else if (t[0] == '@')            n = add_variant(steps, n, t);
        else if (!strcmp(t, ".")) { if (n) steps[n - 1].pause_ms = PAUSE_MS; }
        else                             n = add_clip(steps, n, t);
    }
    if (!n) return false;

    audio_stream_begin();
    for (int i = 0; i < n; i++) {
        play_clip(steps[i].clip);
        if (i < n - 1 && steps[i].pause_ms) play_silence(steps[i].pause_ms);
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
