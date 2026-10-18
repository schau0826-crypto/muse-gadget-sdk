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

/* Shared by both chat backends: the voice note's encoding, and reply and
 * transcript text cut to fit the caption. Also the serial console's "@chat"
 * lines for typed turns. */
#include "muse_chat_priv.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "muse_state.h"
#include "muse_text.h"

#define CAPTION_CHARS 32   /* a short tail, counted in UTF-8 characters */
#define CONSOLE_LINE 400   /* one "@chat" line */
#define CONSOLE_TEXT 240   /* escaped text per line */

static void put_le(uint8_t *p, uint32_t v, int n)
{
    for (int i = 0; i < n; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

void muse_hatch_wav_header(uint8_t h[MUSE_HATCH_WAV_HEADER], uint32_t rate)
{
    memcpy(h, "RIFF", 4);
    put_le(h + 4, UINT32_MAX, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    put_le(h + 16, 16, 4);
    put_le(h + 20, 1, 2);             /* PCM */
    put_le(h + 22, 1, 2);             /* mono */
    put_le(h + 24, rate, 4);
    put_le(h + 28, rate * 2, 4);
    put_le(h + 32, 2, 2);
    put_le(h + 34, 16, 2);
    memcpy(h + 36, "data", 4);
    put_le(h + 40, UINT32_MAX, 4);
}

size_t muse_hatch_base64(const uint8_t *in, size_t n, char *out)
{
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *o = out;
    size_t i = 0;
    for (; i + 2 < n; i += 3) {
        uint32_t v = in[i] << 16 | in[i + 1] << 8 | in[i + 2];
        *o++ = A[v >> 18];
        *o++ = A[v >> 12 & 63];
        *o++ = A[v >> 6 & 63];
        *o++ = A[v & 63];
    }
    if (i < n) {
        uint32_t v = in[i] << 16 | (i + 1 < n ? in[i + 1] << 8 : 0);
        *o++ = A[v >> 18];
        *o++ = A[v >> 12 & 63];
        *o++ = i + 1 < n ? A[v >> 6 & 63] : '=';
        *o++ = '=';
    }
    return o - out;
}

static bool break_space(int32_t cp)
{
    return cp == ' ' || cp == 0x3000;   /* ASCII and ideographic spaces */
}

static bool opening_punctuation(int32_t cp)
{
    return cp == '(' || cp == '[' || cp == '{' || cp == '<'
        || cp == 0x2018 || cp == 0x201C || cp == 0x3008 || cp == 0x300A
        || cp == 0x300C || cp == 0x300E || cp == 0x3010 || cp == 0x3014
        || cp == 0x3016 || cp == 0x3018 || cp == 0x301A || cp == 0x301D
        || cp == 0xFF08 || cp == 0xFF3B || cp == 0xFF5B || cp == 0xFF5F
        || cp == 0xFF62;
}

static bool closing_punctuation(int32_t cp)
{
    return cp == ')' || cp == ']' || cp == '}' || cp == '>'
        || cp == ',' || cp == '.' || cp == '!' || cp == '?' || cp == ':' || cp == ';'
        || cp == 0x2019 || cp == 0x201D || cp == 0x2026
        || cp == 0x3001 || cp == 0x3002 || cp == 0x3009 || cp == 0x300B
        || cp == 0x300D || cp == 0x300F || cp == 0x3011 || cp == 0x3015
        || cp == 0x3017 || cp == 0x3019 || cp == 0x301B || cp == 0x301E
        || cp == 0x301F || cp == 0xFF09 || cp == 0xFF0C || cp == 0xFF0E
        || cp == 0xFF01 || cp == 0xFF1A || cp == 0xFF1B || cp == 0xFF1F
        || cp == 0xFF3D || cp == 0xFF5D || cp == 0xFF60 || cp == 0xFF63;
}

static const char *skip_break_spaces(const char *p)
{
    while (*p) {
        size_t bytes;
        int32_t cp = muse_text_codepoint(p, &bytes);
        if (!break_space(cp)) {
            break;
        }
        p += bytes;
    }
    return p;
}

static const char *previous_character(const char *start, const char *end)
{
    const char *p = end - 1;
    while (p > start && ((unsigned char)*p & 0xC0) == 0x80) {
        p--;
    }
    return p;
}

/* Last CAPTION_CHARS Unicode characters of `src`, starting on a word boundary when possible. */
void muse_hatch_tail_words(const char *src, char *out, size_t cap)
{
    const char *p = src;
    size_t chars = 0;
    for (const char *q = src; *q;) {
        size_t bytes;
        muse_text_codepoint(q, &bytes);
        q += bytes;
        chars++;
    }
    if (chars > CAPTION_CHARS) {
        size_t skip = chars - CAPTION_CHARS;
        while (*p && skip--) {
            size_t bytes;
            muse_text_codepoint(p, &bytes);
            p += bytes;
        }
        const char *sp = NULL;
        size_t distance = 0;
        for (const char *q = p; *q && distance < 12;) {
            size_t bytes;
            int32_t cp = muse_text_codepoint(q, &bytes);
            if (break_space(cp)) {
                sp = q;
                break;
            }
            q += bytes;
            distance++;
        }
        if (sp) {
            p = sp;
            size_t bytes;
            muse_text_codepoint(p, &bytes);
            p += bytes;
            p = skip_break_spaces(p);
        }
    }
    p = skip_break_spaces(p);
    muse_text_copy_utf8(out, cap, p);
}

/*
 * The next line of `text` wrapped to `cols` characters as the caption shows
 * them in the screen's cells: English words break at spaces, CJK has no
 * spaces so it breaks between characters but never before closing
 * punctuation. ASCII stand-ins keep their expanded widths and omitted
 * symbols take no cells.
 */
static bool next_line(const char **text, int cols, const char **start, size_t *len)
{
    const char *p = skip_break_spaces(*text);
    while (*p == '\n') {
        p = skip_break_spaces(p + 1);
    }
    const char *end = p, *brk = NULL;
    int n = 0, brk_n = 0;
    while (*end && *end != '\n') {
        size_t bytes;
        char shown[4];
        int32_t cp = muse_text_codepoint(end, &bytes);
        int width = muse_text_ascii(end, &bytes, shown);
        width = width < 0 ? 1 : width;
        if (n + width > cols && n) {
            break;
        }
        if (break_space(cp)) {
            brk = end;
            brk_n = n;
        }
        n += width;
        end += bytes;
    }
    bool forced_break = *end && *end != '\n';
    /* English wraps at spaces, unless that leaves the line mostly empty: a
     * "- " bullet before a long run of Hanzi would sit alone on its line. */
    if (forced_break && brk && brk > p && brk_n * 3 >= cols) {
        end = brk;
    }
    if (forced_break) {
        for (;;) {
            const char *after = skip_break_spaces(end);
            if (!*after || *after == '\n') {
                break;
            }
            size_t next_bytes, prev_bytes;
            int32_t next_cp = muse_text_codepoint(after, &next_bytes);
            const char *prev = previous_character(p, end);
            int32_t prev_cp = muse_text_codepoint(prev, &prev_bytes);
            if (!opening_punctuation(prev_cp) && !closing_punctuation(next_cp)) {
                break;
            }
            if (prev == p) {
                /* A one-column page cannot split an opening/closing pair. */
                end = after + next_bytes;
                break;
            }
            end = prev;
        }
    }
    *start = p;
    *len = end - p;
    *text = end;
    return end != p;
}

/*
 * Wraps `text` to the screen's page (muse_state_page) and puts the lines
 * around byte `at` in `out`. It scrolls a line at a time: the line holding
 * `at` is second from the top, the one before it kept for context, until the
 * text's last lines fill the page.
 */
bool muse_hatch_caption_at(const char *text, size_t at, char *out, size_t cap)
{
    if (!text || !out || !cap) {
        return false;
    }
    int cols, lines;
    muse_state_page(muse_text_has_cjk(text), &cols, &lines);
    const char *p = text, *start;
    size_t len;
    int line = -1, n = 0;
    while (next_line(&p, cols, &start, &len)) {
        if (line < 0 && (size_t)(start + len - text) > at) {
            line = n;
        }
        n++;
    }
    if (!n) {
        return false;
    }
    if (line < 0) {
        line = n - 1;
    }
    int first = line - (lines > 1);   /* a one-line page just shows that line */
    first = first < n - lines ? first : n - lines;
    first = first < 0 ? 0 : first;

    size_t o = 0;
    out[0] = '\0';
    p = text;
    for (n = 0; n < first + lines && next_line(&p, cols, &start, &len); n++) {
        if (n < first) {
            continue;
        }
        size_t sep = o ? 1 : 0;
        if (o + sep + len >= cap) {
            break;   /* whole lines only */
        }
        o += snprintf(out + o, cap - o, "%s%.*s", o ? "\n" : "", (int)len, start);
    }
    return o != 0;
}

/* ---- Markdown replies as plain text ---- */

typedef struct {
    char *out;
    size_t cap, len;
} sink_t;

static void put(sink_t *s, const char *p, size_t n)
{
    if (s->len + n >= s->cap) {
        n = s->cap - 1 - s->len;
    }
    memcpy(s->out + s->len, p, n);
    s->len += n;
    s->out[s->len] = '\0';
}

static bool url_start(const char *p)
{
    return !strncmp(p, "https://", 8) || !strncmp(p, "http://", 7);
}

/* A bare URL ends at a space, a non-ASCII character or a closing mark; the
 * sentence's own punctuation after it isn't part of it. */
static const char *url_end(const char *p)
{
    const char *e = p;
    while ((unsigned char)*e > ' ' && (unsigned char)*e < 0x80 && !strchr(")]>\"'<`*", *e)) {
        e++;
    }
    while (e > p && strchr(".,;:!?", e[-1])) {
        e--;
    }
    return e;
}

static void put_host(sink_t *s, const char *p, const char *end)
{
    const char *h = p + (p[4] == 's' ? 8 : 7);
    if (!strncmp(h, "www.", 4)) {
        h += 4;
    }
    const char *e = h;
    while (e < end && *e != '/' && *e != '?' && *e != '#') {
        e++;
    }
    put(s, h, e - h);
}

/* Whether the '[' at p opens a link: "](" follows on its line. */
static bool link_ahead(const char *p)
{
    for (p++; *p && *p != '\n' && *p != '['; p++) {
        if (*p == ']') {
            return p[1] == '(';
        }
    }
    return false;
}

/* Past a link's "(target)" at p: its closing ')', or the line's end while it streams in. */
static const char *skip_target(const char *p)
{
    int depth = 0;
    for (; *p && *p != '\n'; p++) {
        if (*p == '(') {
            depth++;
        } else if (*p == ')' && --depth == 0) {
            return p + 1;
        }
    }
    return p;
}

static void put_inline(sink_t *s, const char *p, const char *end, bool speech)
{
    int links = 0;
    while (p < end) {
        char c = *p;
        if (url_start(p)) {
            const char *e = url_end(p);
            if (!speech) {
                put_host(s, p, e);
            }
            p = e;
            if (*p == '>') {
                p++;   /* <autolink> */
            }
        } else if (c == '<' && url_start(p + 1)) {
            p++;
        } else if (c == '!' && p[1] == '[' && link_ahead(p + 1)) {
            p++;   /* an image: its alt text, like a link's */
        } else if (c == '[' && link_ahead(p)) {
            links++;
            p++;
        } else if (c == ']' && links && p[1] == '(') {
            links--;
            p = skip_target(p + 1);
        } else if (c == '*' || c == '`') {
            p++;
        } else if ((c == '~' || c == '_') && p[1] == c) {
            p += 2;
        } else if (c == '|') {
            put(s, " ", 1);   /* table cells */
            p++;
        } else {
            put(s, p++, 1);
        }
    }
}

/* Whether p..end has only characters from `set`, and at least `min` of `mark`. */
static bool only_marks(const char *p, const char *end, const char *set, char mark, int min)
{
    int seen = 0;
    for (; p < end; p++) {
        if (!strchr(set, *p)) {
            return false;
        }
        seen += *p == mark;
    }
    return seen >= min;
}

static bool ends_in_pause(const sink_t *s)
{
    if (!s->len) {
        return true;
    }
    const char *last = previous_character(s->out, s->out + s->len);
    size_t bytes;
    int32_t cp = muse_text_codepoint(last, &bytes);
    return break_space(cp) || closing_punctuation(cp) || cp == '\n';
}

size_t muse_hatch_plain_text(const char *src, char *out, size_t cap, bool speech)
{
    if (!out || !cap) {
        return 0;
    }
    sink_t s = { out, cap, 0 };
    out[0] = '\0';
    bool fence = false;
    for (const char *p = src ? src : ""; *p;) {
        const char *eol = strchr(p, '\n');
        eol = eol ? eol : p + strlen(p);
        const char *q = p;
        while (q < eol && (*q == ' ' || *q == '\t')) {
            q++;
        }
        size_t before = s.len;
        if (!strncmp(q, "```", 3) || !strncmp(q, "~~~", 3)) {
            fence = !fence;
        } else if (fence) {
            if (!speech) {
                put(&s, q, eol - q);   /* code shows as it is, and isn't read out */
            }
        } else if (only_marks(q, eol, "-*_ ", *q, 3) || only_marks(q, eol, "|-: ", '-', 1)) {
            /* a rule, or a table's header separator */
        } else {
            if (*q == '#') {
                const char *r = q;
                while (*r == '#') {
                    r++;
                }
                if (r == eol || *r == ' ') {
                    q = r < eol ? r + 1 : r;
                }
            }
            while (*q == '>') {
                q += q[1] == ' ' ? 2 : 1;
            }
            if ((*q == '-' || *q == '*' || *q == '+') && q[1] == ' ') {
                q += 2;
                if (!speech) {
                    put(&s, "- ", 2);
                }
            }
            put_inline(&s, q, eol, speech);
        }
        if (s.len > before && *eol) {
            /* A list item or heading without punctuation still gets its pause. */
            if (speech && !ends_in_pause(&s)) {
                put(&s, "\xE3\x80\x82", 3);   /* 。 */
            }
            put(&s, "\n", 1);
        }
        p = *eol ? eol + 1 : eol;
    }
    return muse_text_copy_utf8(out, cap, out);   /* put may have cut a character */
}

static int speech_unit(int32_t cp)
{
    if (cp >= 0x1F000 || cp < 0) {
        return 0;   /* emoji and malformed bytes */
    }
    if (closing_punctuation(cp) || cp == '\n') {
        return 3;
    }
    if (cp >= 0x2E80 && !(cp >= 0x3000 && cp <= 0x303F)) {
        return 6;
    }
    bool alnum = ((cp | 0x20) >= 'a' && (cp | 0x20) <= 'z') || (cp >= '0' && cp <= '9');
    return cp < 0x80 && alnum ? 2 : 0;
}

size_t muse_hatch_speech_units(const char *text)
{
    size_t units = 0;
    for (const char *p = text; p && *p;) {
        size_t bytes;
        units += speech_unit(muse_text_codepoint(p, &bytes));
        p += bytes;
    }
    return units;
}

size_t muse_hatch_units_offset(const char *text, size_t units)
{
    size_t said = 0;
    const char *p = text;
    while (p && *p) {
        size_t bytes;
        said += speech_unit(muse_text_codepoint(p, &bytes));
        if (said > units) {
            break;
        }
        p += bytes;
    }
    return p ? (size_t)(p - text) : 0;
}

/* ---- Typed turns on the serial console ---- */

/* JSON-escapes whole characters of *src into out, up to cap - 1 bytes; moves *src past them. */
static size_t escape_some(const char **src, char *out, size_t cap)
{
    const unsigned char *p = (const unsigned char *)*src;
    size_t o = 0;
    while (*p) {
        char esc[8];
        const char *put = esc;
        size_t n = 1, put_len = 2;
        esc[0] = '\\';
        if (*p == '"' || *p == '\\') {
            esc[1] = (char)*p;
        } else if (*p == '\n') {
            esc[1] = 'n';
        } else if (*p == '\r') {
            esc[1] = 'r';
        } else if (*p == '\t') {
            esc[1] = 't';
        } else if (*p < 0x20) {
            put_len = snprintf(esc, sizeof(esc), "\\u%04x", *p);
        } else {
            while (n < 4 && (p[n] & 0xC0) == 0x80) {
                n++;   /* the rest of a UTF-8 character */
            }
            put = (const char *)p;
            put_len = n;
        }
        if (o + put_len >= cap) {
            break;
        }
        memcpy(out + o, put, put_len);
        o += put_len;
        p += n;
    }
    out[o] = '\0';
    *src = (const char *)p;
    return o;
}

/* Each line goes out in one write, so other tasks' log lines land between lines, not inside them. */
void muse_hatch_console(const char *type, const char *text, const char *fields, ...)
{
    static unsigned seq;
    char line[CONSOLE_LINE];
    do {
        size_t n = snprintf(line, sizeof(line), "@chat {\"seq\":%u,\"type\":\"%s\"",
                            __atomic_add_fetch(&seq, 1, __ATOMIC_RELAXED), type);
        if (fields) {
            va_list ap;
            va_start(ap, fields);
            line[n++] = ',';
            n += vsnprintf(line + n, sizeof(line) - n, fields, ap);
            va_end(ap);
        }
        if (n > sizeof(line) - CONSOLE_TEXT - 16) {
            n = sizeof(line) - CONSOLE_TEXT - 16;   /* never: fields are short */
        }
        if (text) {
            const char *was = text;
            n += snprintf(line + n, sizeof(line) - n, ",\"text\":\"");
            n += escape_some(&text, line + n, CONSOLE_TEXT);
            line[n++] = '"';
            if (text == was) {
                text = NULL;
            }
        }
        line[n++] = '}';
        line[n++] = '\n';
        fwrite(line, 1, n, stdout);
        fflush(stdout);
    } while (text && *text);
}

size_t muse_hatch_unescape(char *s)
{
    char *o = s;
    for (const char *p = s; *p; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            *o++ = *p == 'n' ? '\n' : *p == 'r' ? '\r' : *p == 't' ? '\t' : *p;
        } else {
            *o++ = *p;
        }
    }
    *o = '\0';
    return o - s;
}
