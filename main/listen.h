/*
 * listen.h  --  offline voice control: "Jarvis" wake word (esp-sr WakeNet9)
 * then an English command (MultiNet7), all on-device.
 *
 * What the command set doesn't match goes to the "brain on the Mac" (brain.h).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    LISTEN_OFF = 0,     /* disabled, no models, or on battery           */
    LISTEN_IDLE,        /* waiting for the wake word                    */
    LISTEN_PROMPT,      /* woke; playing "Yes?"                         */
    LISTEN_COMMAND,     /* recognizing a command                        */
    LISTEN_THINKING,    /* the Mac is working on an unmatched utterance */
} listen_state_t;

bool listen_init(void);                 /* load models + start tasks; false if no models */
listen_state_t listen_state(void);
const char *listen_heard(void);         /* last recognized phrase ("" if none)           */
int64_t listen_heard_us(void);          /* esp_timer time it was heard                   */
float listen_level_db(void);            /* mic level (dBFS), for diagnostics             */
void listen_wake_now(void);             /* skip the wake word (push-to-talk / web test)  */

/* Diagnostics: the raw mic audio (16 kHz mono) around the last command
 * attempt -- from the wake word to the end of the command window. */
const int16_t *listen_last_take(int *samples);
