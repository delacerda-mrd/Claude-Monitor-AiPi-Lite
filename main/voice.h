/*
 * voice.h  --  spoken announcements from the clip pack in the "voice"
 * partition (built by tools/make_voice.py, uploaded via POST /voice).
 *
 * Script format: space-separated tokens
 *   name    a clip from the pack ("session_at", "percent", "online", ...)
 *   #N      a number 0..100 spoken as one word ("#42" -> "forty two")
 *   ~S      a duration in seconds -> "3 hours 12 minutes" / "2 days 4 hours"
 *   .       a short pause
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool        voice_init(void);           /* (re)load the pack index; false if none   */
bool        voice_ready(void);
const char *voice_name(void);           /* TTS voice the pack was built with        */
int         voice_clip_count(void);

/* Audio task only: speak a script through audio_stream_*(). false if no pack. */
bool voice_play_script(const char *script);

/* Pack upload (httpd task): erase-as-you-go writes into the partition. */
bool voice_write_begin(size_t total);
bool voice_write(const void *data, size_t len);
bool voice_write_end(void);             /* validates + reloads the index            */
