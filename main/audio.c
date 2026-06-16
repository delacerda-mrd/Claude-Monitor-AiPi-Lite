/*
 * audio.c  --  ES8311 I2S tone player for AIPI-Lite
 *
 * Pin mapping (from xiaozhi-esp32 board/aipi-lite/config.h):
 *   I2C  SDA=4   SCL=5        — ES8311 control
 *   I2S  MCLK=6  BCLK=14  WS=12  DOUT=11  DIN=13
 *   PA   GPIO 9               — external speaker amplifier enable
 *
 * Dependencies:
 *   espressif/es8311 ^1.0.0   — legacy codec driver (I2C register layer)
 *   driver/i2s_std            — IDF I2S standard-mode driver
 *   driver/i2c                — IDF legacy I2C master driver
 */

#include "audio.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "es8311.h"
#include "esp_log.h"

static const char *TAG = "audio";

/* ------------------------------------------------------------------ */
/* Board pin / audio-format constants                                  */
/* ------------------------------------------------------------------ */
#define I2C_NUM           I2C_NUM_1
#define I2C_SDA_IO        GPIO_NUM_5
#define I2C_SCL_IO        GPIO_NUM_4

#define I2S_NUM           I2S_NUM_0
#define I2S_MCK_IO        GPIO_NUM_6
#define I2S_BCK_IO        GPIO_NUM_14
#define I2S_WS_IO         GPIO_NUM_12
#define I2S_DO_IO         GPIO_NUM_11
#define I2S_DI_IO         GPIO_NUM_13    /* unused — simplex TX only   */

#define PA_PIN            GPIO_NUM_9

#define SAMPLE_RATE       24000
#define MCLK_MULTIPLE     256
#define MCLK_FREQ_HZ      (SAMPLE_RATE * MCLK_MULTIPLE)

#define SINE_TABLE_SIZE   256
#define AMPLITUDE          0.8f           /* fraction of full-scale    */
#define ATTACK_MS          5
#define RELEASE_MS         20

/* ------------------------------------------------------------------ */
/* Static state                                                        */
/* ------------------------------------------------------------------ */
static i2s_chan_handle_t  tx_handle;
static i2s_chan_handle_t  rx_handle;
static bool               tx_enabled;   /* TX starts disabled at init */
static es8311_handle_t    es_handle;
static int16_t            sine_table[SINE_TABLE_SIZE];

/* Volume presets and current index */
static const int vol_presets[] = {65, 70, 80};
static const int vol_presets_count = 3;
static int vol_index = 0;           /* starts at 65 % */

/* ------------------------------------------------------------------ */
/* Musical note frequencies (equal temperament, A4 = 440 Hz)           */
/* ------------------------------------------------------------------ */
#define NOTE_C4   262
#define NOTE_D4   294
#define NOTE_E4   330
#define NOTE_F4   349
#define NOTE_G4   392
#define NOTE_A4   440
#define NOTE_B4   494
#define NOTE_C5   523
#define NOTE_D5   587
#define NOTE_E5   659
#define NOTE_F5   698
#define NOTE_G5   784
#define NOTE_A5   880
#define NOTE_B5   988
#define NOTE_C6  1047
#define NOTE_D6  1175
#define NOTE_E6  1319
#define NOTE_C7  2093
#define REST        0     /* silence */

/* A note in a melody */
typedef struct {
    int freq;
    int dur_ms;
} note_t;

/* Predefined melodies — all notes ≥ 150 ms for reliable DMA playback */
static const note_t melody_boot[] = {
    {NOTE_C5, 180}, {NOTE_E5, 180}, {NOTE_G5, 250},
};
static const note_t melody_poll_ok[] = {
    {NOTE_C6, 150},
};
static const note_t melody_threshold_60[] = {
    {NOTE_G5, 180}, {NOTE_C6, 180},
};
static const note_t melody_threshold_85[] = {
    {NOTE_G5, 150}, {REST, 80}, {NOTE_G5, 150}, {REST, 80}, {NOTE_G5, 150},
};
static const note_t melody_error[] = {
    {NOTE_E5, 250}, {NOTE_C5, 300},
};
static const note_t melody_token_saved[] = {
    {NOTE_C5, 180}, {NOTE_F5, 220},
};
static const note_t melody_button[] = {
    {NOTE_C7, 150},
};

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void pa_set(bool on)
{
    gpio_set_level(PA_PIN, on ? 1 : 0);
}

/* Fill sine_table with one cycle of sin(x), scaled to AMPLITUDE */
static void sine_table_init(void)
{
    for (int i = 0; i < SINE_TABLE_SIZE; i++) {
        sine_table[i] = (int16_t)(
            sinf(2.0f * (float)M_PI * i / SINE_TABLE_SIZE)
            * 32767.0f * AMPLITUDE);
    }
}

/* ------------------------------------------------------------------ */
/* I2C init (legacy driver — required by es8311 component)             */
/* ------------------------------------------------------------------ */
static esp_err_t i2c_init(void)
{
    i2c_config_t cfg = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = I2C_SDA_IO,
        .scl_io_num       = I2C_SCL_IO,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_NUM, &cfg));
    return i2c_driver_install(I2C_NUM, I2C_MODE_MASTER, 0, 0, 0);
}

/* ------------------------------------------------------------------ */
/* ES8311 codec init                                                   */
/* ------------------------------------------------------------------ */
static esp_err_t es8311_init_codec(void)
{
    /* Try both possible I2C addresses (CE pin state varies by board) */
    const uint16_t addrs[] = {ES8311_ADDRRES_0, ES8311_ADDRESS_1};
    esp_err_t ret = ESP_FAIL;

    for (int i = 0; i < 2; i++) {
        es_handle = es8311_create(I2C_NUM, addrs[i]);
        if (!es_handle) continue;

        es8311_clock_config_t clk = {
            .mclk_inverted      = false,
            .sclk_inverted      = false,
            .mclk_from_mclk_pin = true,
            .mclk_frequency     = MCLK_FREQ_HZ,
            .sample_frequency   = SAMPLE_RATE,
        };

        ret = es8311_init(es_handle, &clk,
                          ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "ES8311 init failed at addr 0x%02x: %s",
                     addrs[i], esp_err_to_name(ret));
            es8311_delete(es_handle);
            es_handle = NULL;
            continue;
        }

        ret = es8311_sample_frequency_config(es_handle,
                        MCLK_FREQ_HZ, SAMPLE_RATE);
        if (ret != ESP_OK) goto fail;

        ret = es8311_voice_volume_set(es_handle, 70, NULL);
        if (ret != ESP_OK) goto fail;

        ret = es8311_microphone_config(es_handle, false);
        if (ret != ESP_OK) goto fail;

        ret = es8311_voice_mute(es_handle, false);
        if (ret != ESP_OK) goto fail;

        ESP_LOGI(TAG, "ES8311 initialized at addr 0x%02x (MCLK=%d Hz, Fs=%d Hz)",
                 addrs[i], MCLK_FREQ_HZ, SAMPLE_RATE);

        /* The es8311 driver enables HP output only (reg 0x13 = 0x10).
         * Enable the speaker output too (bit 3 = 0x08) in case the
         * onboard speaker is wired to SPKOUT rather than HPOUT. */
        {
            i2c_cmd_handle_t cmd = i2c_cmd_link_create();
            i2c_master_start(cmd);
            i2c_master_write_byte(cmd, (addrs[i] << 1) | I2C_MASTER_WRITE, true);
            i2c_master_write_byte(cmd, 0x13, true);       /* register */
            i2c_master_write_byte(cmd, 0x18, true);       /* HP + SPK enable */
            i2c_master_stop(cmd);
            esp_err_t r = i2c_master_cmd_begin(I2C_NUM, cmd, pdMS_TO_TICKS(50));
            i2c_cmd_link_delete(cmd);
            ESP_LOGI(TAG, "ES8311 SPK+HP enable (reg 0x13=0x18): %s",
                     esp_err_to_name(r));
        }

        return ESP_OK;
    }

    ESP_LOGE(TAG, "ES8311 not found at any address — check I2C wiring");
    return ESP_FAIL;

fail:
    ESP_LOGE(TAG, "ES8311 config failed: %s", esp_err_to_name(ret));
    es8311_delete(es_handle);
    es_handle = NULL;
    return ret;
}

/* ------------------------------------------------------------------ */
/* I2S init  (standard Philips / I²S, master, simplex TX)              */
/* ------------------------------------------------------------------ */
static esp_err_t i2s_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM,
                                                            I2S_ROLE_MASTER);
    chan_cfg.auto_clear = false;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCK_IO,
            .bclk = I2S_BCK_IO,
            .ws   = I2S_WS_IO,
            .dout = I2S_DO_IO,
            .din  = I2S_DI_IO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = MCLK_MULTIPLE;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));

    /* Enable TX now but we'll disable/re-enable per tone for reliable
     * short-burst playback (same pattern as IDF i2s_es8311 example).
     * RX stays enabled for the ES8311 MCLK to keep running. */
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));

    ESP_LOGI(TAG, "I2S simplex TX ready");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

esp_err_t audio_init(void)
{
    /* PA pin — hold low until first playback */
    gpio_reset_pin(PA_PIN);
    gpio_set_direction(PA_PIN, GPIO_MODE_OUTPUT);
    pa_set(false);

    sine_table_init();

    esp_err_t ret = i2c_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* I2S must be started BEFORE ES8311 init — the codec needs
     * the MCLK signal to respond to I2C register access. */
    ret = i2s_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = es8311_init_codec();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "audio subsystem ready");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Tone generator                                                      */
/* ------------------------------------------------------------------ */

void audio_play_tone(int freq_hz, int duration_ms)
{
    if (!tx_handle || duration_ms <= 0 || freq_hz <= 0) return;

    int total_samples = SAMPLE_RATE * duration_ms / 1000;
    if (total_samples < 1) total_samples = 1;

    /* Stereo interleaved: both channels carry the same mono signal */
    int16_t *stereo = malloc(total_samples * 2 * sizeof(int16_t));
    if (!stereo) return;

    float phase_inc = (float)freq_hz / SAMPLE_RATE;
    float phase     = 0.0f;

    int attack_samples  = SAMPLE_RATE * ATTACK_MS  / 1000;
    int release_samples = SAMPLE_RATE * RELEASE_MS / 1000;
    if (attack_samples  > total_samples / 3) attack_samples  = total_samples / 3;
    if (release_samples > total_samples / 3) release_samples = total_samples / 3;

    for (int i = 0; i < total_samples; i++) {
        /* Sine-table lookup with linear interpolation */
        float pos = phase * SINE_TABLE_SIZE;
        int   i0  = (int)pos;
        int   i1  = (i0 + 1) % SINE_TABLE_SIZE;
        float frac = pos - i0;

        int16_t sample = (int16_t)(
            sine_table[i0] * (1.0f - frac) + sine_table[i1] * frac);

        /* Envelope — avoid clicks */
        float env = 1.0f;
        if (i < attack_samples) {
            env = (float)i / attack_samples;
        } else if (i >= total_samples - release_samples) {
            env = (float)(total_samples - 1 - i) / release_samples;
        }
        sample = (int16_t)(sample * env);

        stereo[i * 2]     = sample;
        stereo[i * 2 + 1] = sample;

        phase += phase_inc;
        if (phase >= 1.0f) phase -= 1.0f;
    }

    /* Disable TX, preload the tone data, then re-enable.
     * This ensures the DMA starts from a known state and the first
     * sample hits the codec immediately — critical for short tones.
     * Skip the disable on the very first tone (TX never enabled yet,
     * which would otherwise log a spurious i2s_common error). */
    if (tx_enabled) i2s_channel_disable(tx_handle);

    size_t preloaded = 0;
    i2s_channel_preload_data(tx_handle, stereo,
                             total_samples * 2 * sizeof(int16_t),
                             &preloaded);
    free(stereo);

    pa_set(true);
    vTaskDelay(pdMS_TO_TICKS(5));           /* PA rail settle              */

    i2s_channel_enable(tx_handle);
    tx_enabled = true;

    /* Wait for DMA to actually play the audio before killing PA. */
    vTaskDelay(pdMS_TO_TICKS(duration_ms + 30));
    pa_set(false);
}

/* ------------------------------------------------------------------ */
/* Melody player                                                       */
/* ------------------------------------------------------------------ */

void audio_play_melody(melody_type_t type)
{
    const note_t *notes;
    int count;

    switch (type) {
    case MELODY_BOOT:           notes = melody_boot;           count = 3; break;
    case MELODY_POLL_OK:        notes = melody_poll_ok;        count = 1; break;
    case MELODY_THRESHOLD_60:   notes = melody_threshold_60;   count = 2; break;
    case MELODY_THRESHOLD_85:   notes = melody_threshold_85;   count = 5; break;
    case MELODY_ERROR:          notes = melody_error;          count = 2; break;
    case MELODY_TOKEN_SAVED:    notes = melody_token_saved;    count = 2; break;
    case MELODY_BUTTON:         notes = melody_button;         count = 1; break;
    default: return;
    }

    for (int i = 0; i < count; i++) {
        if (notes[i].freq == REST) {
            vTaskDelay(pdMS_TO_TICKS(notes[i].dur_ms));
        } else {
            audio_play_tone(notes[i].freq, notes[i].dur_ms);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Volume cycling                                                       */
/* ------------------------------------------------------------------ */

void audio_volume_cycle(void)
{
    if (!es_handle) return;

    vol_index = (vol_index + 1) % vol_presets_count;
    int vol = vol_presets[vol_index];
    es8311_voice_volume_set(es_handle, vol, NULL);
    ESP_LOGI(TAG, "Volume set to %d%%", vol);

    /* Feedback tone — pitch rises with volume */
    int freq = (vol == 65) ? 440 : (vol == 70) ? 660 : 880;
    audio_play_tone(freq, 150);
}
