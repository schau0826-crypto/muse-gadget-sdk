/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Reply speech from Xiaomi MiMo's streaming TTS (CONFIG_MUSE_MIMO_TTS_*).
 * One job at a time: a worker task POSTs the text, parses the server-sent
 * events and queues their PCM for the chat session task to play.
 */

#define MUSE_TTS_RATE 24000   /* MiMo streams 24 kHz mono PCM16 */

typedef enum {
    MUSE_TTS_PENDING,   /* more audio may come */
    MUSE_TTS_ENDED,     /* all of it has been read */
    MUSE_TTS_FAILED,    /* the request failed; what was read is all there is */
} muse_tts_state_t;

/* Starts the worker; false when no API key is configured or it can't start. */
bool muse_tts_init(void);
/* Starts speaking `text`, replacing any job in progress. Returns the job, or 0. */
uint32_t muse_tts_speak(const char *text);
/* Abandons the job in progress, if any. */
void muse_tts_stop(void);
/* Samples the job has produced so far; *all once that's the whole of it. */
size_t muse_tts_generated(uint32_t job, bool *all);
/* Takes up to `max` samples of the job's audio without waiting. */
size_t muse_tts_read(uint32_t job, int16_t *out, size_t max, muse_tts_state_t *state);

#ifdef __cplusplus
}
#endif
