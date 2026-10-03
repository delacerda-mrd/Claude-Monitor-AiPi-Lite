/*
 * brain.h  --  "brain on the Mac": what the offline command set can't
 * handle goes to brain_server.py on the Mac that pushes our token
 * (settings notify_ip, port 5556). It transcribes the audio (whisper),
 * asks Claude, and answers with speech in the meter's own voice, plus an
 * optional action (page change, mute, ...).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BRAIN_PORT  5556

void brain_init(void);
bool brain_available(void);             /* enabled, Mac known, Wi-Fi up, not busy */
bool brain_busy(void);
/* Ask about an utterance (16 kHz mono). Takes ownership of `pcm` (a
 * heap_caps_malloc buffer) on success; false = not sent, caller frees. */
bool brain_ask(int16_t *pcm, int samples);
const char *brain_heard(void);          /* last transcript ("" if none)           */
const char *brain_status(void);         /* "idle" / "asking" / last error         */
