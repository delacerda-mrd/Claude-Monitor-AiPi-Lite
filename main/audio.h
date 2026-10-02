/*
 * audio.h  --  ES8311 I2S tone player for AIPI-Lite
 *
 * Initializes the onboard ES8311 codec + I2S peripheral and provides
 * blocking tone / melody playback for UI feedback sounds.
 */

#pragma once

#include "esp_err.h"

/* Melody presets */
typedef enum {
    MELODY_BOOT,           /* rising arpeggio: "I'm alive" */
    MELODY_POLL_OK,        /* single short chirp: data fetched */
    MELODY_THRESHOLD_60,   /* two ascending notes: usage warning */
    MELODY_THRESHOLD_85,   /* three staccato beeps: urgent */
    MELODY_ERROR,          /* descending minor third: API / network error */
    MELODY_TOKEN_SAVED,    /* ascending fourth: web config confirm */
    MELODY_BUTTON,         /* very short tick: tactile feedback */
    MELODY_RESET,          /* bright rising run: a usage window reset */
    MELODY_SETUP,          /* two soft notes: setup AP is up */
} melody_type_t;

/**
 * Initialize audio subsystem:
 *   - I2C master on GPIO 4 (SDA) / 5 (SCL)
 *   - ES8311 codec (slave, MCLK from pin 6)
 *   - I2S simplex TX on GPIO 11 (DOUT), 14 (BCLK), 12 (WS), 6 (MCLK)
 *   - PA enable on GPIO 9
 *
 * Call once after display init, before any audio_play_* calls.
 */
esp_err_t audio_init(void);

/**
 * Play a single sine-wave tone at `freq_hz` for `duration_ms` milliseconds.
 * Blocks until the tone finishes.  Callable from any task.
 */
void audio_play_tone(int freq_hz, int duration_ms);

/**
 * Play a predefined melody.  Blocks until the melody finishes.
 */
void audio_play_melody(melody_type_t type);

/**
 * Queue a melody on the audio task and return immediately. Safe from any
 * task (the main loop must never block on audio). Drops the request if the
 * queue is full.
 */
void audio_play_async(melody_type_t type);

/** Set codec output volume (0..100). Takes effect on the next sound. */
void audio_set_volume(int vol);

/**
 * Cycle to the next volume preset (65 → 70 → 80 → 65 …).
 * Applies the change immediately and plays a short feedback tone
 * whose pitch rises with volume level.
 */
void audio_volume_cycle(void);
