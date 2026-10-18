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

#include <stddef.h>
#include <stdint.h>

#if __has_include("sdkconfig.h")   /* not in host tests */
#include "sdkconfig.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Status captions in Chinese on boards whose caption font has Hanzi. */
#if CONFIG_MUSE_BOARD_M5STACK_CORES3
#define MUSE_TR(en, zh) zh
#else
#define MUSE_TR(en, zh) en
#endif

/*
 * CoreS3 captions have ASCII, symbol, and CJK glyphs. These helpers keep
 * characters without a useful stand-in in UTF-8; curly quotes become straight
 * ones, an em dash "--", and accented letters their plain ones where needed.
 */

/* The Unicode code point at s and its byte length; -1 for malformed UTF-8.
 * A malformed byte consumes one byte, so callers can stop at it safely. */
int32_t muse_text_codepoint(const char *s, size_t *len);

/* Copies as many complete, valid UTF-8 characters as fit, always NUL-
 * terminating when cap is nonzero. Stops before malformed/incomplete input;
 * returns the number of bytes copied. */
size_t muse_text_copy_utf8(char *out, size_t cap, const char *src);

/* Appends as many complete UTF-8 characters as fit; stops before malformed
 * input and returns appended bytes. */
size_t muse_text_append_utf8(char *dst, size_t cap, const char *src);

/* The stand-in for the UTF-8 character at s into out, and its length (0 drops
 * the character); -1 to keep it. *len is the character's length in bytes. */
int muse_text_ascii(const char *s, size_t *len, char out[4]);

/* Puts the stand-ins into s, which has room for cap bytes. If one doesn't fit,
 * the text ends there. */
void muse_text_to_ascii(char *s, size_t cap);

/* text, or if it needs stand-ins and fits in cap bytes, a copy with them in buf. */
const char *muse_text_showable(const char *text, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
