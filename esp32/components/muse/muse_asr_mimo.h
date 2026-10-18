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

/* Xiaomi MiMo speech recognition for push-to-talk turns. The API key is the
 * MiMo TTS key (CONFIG_MUSE_MIMO_TTS_API_KEY); without it ASR stays off and
 * callers keep sending recordings to Muse's own ASR. */

/* Allocates the request buffers. Returns false when ASR can't run. */
bool muse_asr_init(void);

/* Transcribes `samples` of 16 kHz mono PCM16. Blocks while the request runs
 * (a few seconds) and returns the recognized text in a module-owned buffer,
 * valid until the next call, or NULL when it failed. */
const char *muse_asr_transcribe(const int16_t *pcm, size_t samples);

#ifdef __cplusplus
}
#endif
