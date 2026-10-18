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

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "muse_chat_priv.h"
#include "muse_state.h"
#include "muse_text.h"

#define INPUT_MAX (64 * 1024)

static int s_cols = 16;
static int s_lines = 2;

void muse_state_page(int *cols, int *lines)
{
    *cols = s_cols;
    *lines = s_lines;
}

static char s_input[INPUT_MAX];

static size_t read_input(void)
{
    size_t n = fread(s_input, 1, sizeof(s_input) - 1, stdin);
    s_input[n] = '\0';
    return n;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        return 2;
    }
    size_t cap = (size_t)strtoul(argv[2], NULL, 10);
    size_t input_len = read_input();
    char *out = (char *)calloc(cap ? cap : 1, 1);
    if (!out) {
        return 2;
    }

    if (!strcmp(argv[1], "copy")) {
        muse_text_copy_utf8(out, cap, s_input);
        fwrite(out, 1, strlen(out), stdout);
    } else if (!strcmp(argv[1], "append")) {
        char *split = memchr(s_input, '\1', input_len);
        if (!split) {
            free(out);
            return 2;
        }
        *split++ = '\0';
        muse_text_copy_utf8(out, cap, s_input);
        muse_text_append_utf8(out, cap, split);
        fwrite(out, 1, strlen(out), stdout);
    } else if (!strcmp(argv[1], "plain") || !strcmp(argv[1], "speech")) {
        muse_hatch_plain_text(s_input, out, cap, argv[1][0] == 's');
        fwrite(out, 1, strlen(out), stdout);
    } else if (!strcmp(argv[1], "units")) {
        printf("%u", (unsigned)muse_hatch_speech_units(s_input));
    } else if (!strcmp(argv[1], "offset")) {
        printf("%u", (unsigned)muse_hatch_units_offset(s_input, cap));
    } else if (!strcmp(argv[1], "tail")) {
        muse_hatch_tail_words(s_input, out, cap);
        fwrite(out, 1, strlen(out), stdout);
    } else if (!strcmp(argv[1], "caption")) {
        if (argc != 6) {
            free(out);
            return 2;
        }
        s_cols = atoi(argv[3]);
        s_lines = atoi(argv[4]);
        size_t at = (size_t)strtoul(argv[5], NULL, 10);
        bool shown = muse_hatch_caption_at(s_input, at, out, cap);
        fputc(shown ? '1' : '0', stdout);
        fputc('\n', stdout);
        fwrite(out, 1, strlen(out), stdout);
    } else {
        free(out);
        return 2;
    }
    free(out);
    return 0;
}
