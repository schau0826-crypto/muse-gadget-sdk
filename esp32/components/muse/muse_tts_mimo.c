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

/*
 * Xiaomi MiMo streaming TTS: POST /v1/chat/completions with the reply as the
 * assistant message, audio.format pcm16 and stream true. The answer is
 * server-sent events, each "data: {json}" line carrying a base64 chunk of
 * 24 kHz mono PCM16 in choices[0].delta.audio.data, then "data: [DONE]".
 *
 * The worker decodes each chunk into a PSRAM stream buffer; the chat session
 * task reads it without waiting. A job number tags the buffer's contents, so
 * audio left from an abandoned job is never played as the next one's.
 */

#include "muse_tts_mimo.h"

#include <stdatomic.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "sdkconfig.h"

/* Builds without the Muse UI's Kconfig (the Voice PE) speak nothing. */
#ifndef CONFIG_MUSE_MIMO_TTS_API_KEY
#define CONFIG_MUSE_MIMO_TTS_API_KEY ""
#define CONFIG_MUSE_MIMO_TTS_MODEL "mimo-v2.5-tts"
#define CONFIG_MUSE_MIMO_TTS_VOICE "mimo_default"
#define CONFIG_MUSE_MIMO_TTS_STYLE ""
#endif

static const char *TAG = "muse_tts";

#define TTS_URL "https://api.xiaomimimo.com/v1/chat/completions"
#define TEXT_MAX 4096                  /* a reply's text, as the session keeps it */
#define PCM_BYTES (512 * 1024)         /* ~11 s of 24 kHz speech; the worker waits while full */
#define EVENT_MAX (160 * 1024)          /* one event line: a base64 chunk and its JSON */
#define AUDIO_MAX (EVENT_MAX / 4 * 3 + 4)
#define RX_BYTES 4096
#define TIMEOUT_MS 20000

static StreamBufferHandle_t s_pcm;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static char *s_text, *s_work, *s_line, *s_rx;
static uint8_t *s_audio;

static uint32_t s_seq;                 /* last job handed out; under s_lock */
static atomic_uint s_want;             /* the job wanted now, or 0 */
static atomic_uint s_ready;            /* the job whose audio the buffer holds */
static atomic_uint s_done;             /* the last job the worker finished */
static atomic_bool s_failed;           /* how s_done went */
static atomic_size_t s_bytes;          /* PCM s_ready's job has queued so far */

typedef struct {
    uint32_t job;
    size_t line_len;
    bool overflow, finished, failed;
    bool has_carry;
    uint8_t carry;                     /* odd byte waiting for its sample's other half */
    size_t pcm_bytes;
    int64_t t0, first_us;
} fetch_t;

static void *psram(size_t n)
{
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static bool wanted(uint32_t job)
{
    return atomic_load(&s_want) == job;
}

/* Queues PCM, waiting while the buffer is full. False if the job was dropped. */
static bool push(uint32_t job, const uint8_t *p, size_t len)
{
    while (len) {
        if (!wanted(job)) {
            return false;
        }
        size_t n = xStreamBufferSend(s_pcm, p, len, pdMS_TO_TICKS(100));
        atomic_fetch_add(&s_bytes, n);
        p += n;
        len -= n;
    }
    return true;
}

static void on_event(fetch_t *f, char *data)
{
    if (!strcmp(data, "[DONE]")) {
        f->finished = true;
        return;
    }
    cJSON *root = cJSON_Parse(data);
    if (!root) {
        ESP_LOGW(TAG, "unreadable event (%u bytes)", (unsigned)strlen(data));
        return;
    }
    cJSON *err = cJSON_GetObjectItem(root, "error");
    if (err) {
        const char *msg = cJSON_GetStringValue(cJSON_GetObjectItem(err, "message"));
        ESP_LOGW(TAG, "server error: %s", msg ? msg : "(no message)");
        f->failed = true;
        cJSON_Delete(root);
        return;
    }
    cJSON *choice = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "choices"), 0);
    cJSON *audio = cJSON_GetObjectItem(cJSON_GetObjectItem(choice, "delta"), "audio");
    const char *b64 = cJSON_GetStringValue(cJSON_GetObjectItem(audio, "data"));
    if (b64 && b64[0]) {
        size_t off = f->has_carry ? 1 : 0, n = 0;
        s_audio[0] = f->carry;
        if (mbedtls_base64_decode(s_audio + off, AUDIO_MAX - off, &n, (const unsigned char *)b64, strlen(b64)) != 0) {
            ESP_LOGW(TAG, "bad base64 audio chunk");
        } else {
            n += off;
            f->has_carry = n & 1;
            if (f->has_carry) {
                f->carry = s_audio[--n];
            }
            if (n && !f->first_us) {
                f->first_us = esp_timer_get_time();
                ESP_LOGI(TAG, "first audio after %lld ms", (f->first_us - f->t0) / 1000);
            }
            f->pcm_bytes += n;
            if (!push(f->job, s_audio, n)) {
                f->finished = true;   /* dropped: stop reading */
            }
        }
    }
    cJSON_Delete(root);
}

/* Splits the body into lines and hands each "data:" payload to on_event. */
static void on_body(fetch_t *f, const char *p, size_t n)
{
    for (size_t i = 0; i < n && !f->finished && !f->failed; i++) {
        char c = p[i];
        if (c != '\n') {
            if (f->line_len < EVENT_MAX - 1) {
                s_line[f->line_len++] = c;
            } else {
                f->overflow = true;
            }
            continue;
        }
        if (f->line_len && s_line[f->line_len - 1] == '\r') {
            f->line_len--;
        }
        s_line[f->line_len] = '\0';
        if (f->overflow) {
            ESP_LOGW(TAG, "event line over %d bytes dropped", EVENT_MAX);
        } else if (!strncmp(s_line, "data:", 5)) {
            char *d = s_line + 5;
            while (*d == ' ') {
                d++;
            }
            on_event(f, d);
        }
        f->line_len = 0;
        f->overflow = false;
    }
}

static char *request_body(const char *text)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", CONFIG_MUSE_MIMO_TTS_MODEL);
    cJSON *msgs = cJSON_AddArrayToObject(root, "messages");
    if (CONFIG_MUSE_MIMO_TTS_STYLE[0]) {
        cJSON *u = cJSON_CreateObject();
        cJSON_AddStringToObject(u, "role", "user");
        cJSON_AddStringToObject(u, "content", CONFIG_MUSE_MIMO_TTS_STYLE);
        cJSON_AddItemToArray(msgs, u);
    }
    cJSON *a = cJSON_CreateObject();
    cJSON_AddStringToObject(a, "role", "assistant");
    cJSON_AddStringToObject(a, "content", text);
    cJSON_AddItemToArray(msgs, a);
    cJSON *audio = cJSON_AddObjectToObject(root, "audio");
    cJSON_AddStringToObject(audio, "format", "pcm16");
    if (CONFIG_MUSE_MIMO_TTS_VOICE[0]) {
        cJSON_AddStringToObject(audio, "voice", CONFIG_MUSE_MIMO_TTS_VOICE);
    }
    cJSON_AddBoolToObject(root, "stream", true);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

static bool fetch(uint32_t job, const char *text)
{
    fetch_t f = { .job = job, .t0 = esp_timer_get_time() };
    char *body = request_body(text);
    if (!body) {
        return false;
    }
    esp_http_client_config_t cfg = {
        .url = TTS_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = TIMEOUT_MS,
        .buffer_size = RX_BYTES,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        cJSON_free(body);
        return false;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "text/event-stream");
    esp_http_client_set_header(client, "api-key", CONFIG_MUSE_MIMO_TTS_API_KEY);

    int len = strlen(body);
    esp_err_t err = esp_http_client_open(client, len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "connect failed: %s", esp_err_to_name(err));
        f.failed = true;
    } else if (esp_http_client_write(client, body, len) != len) {
        ESP_LOGW(TAG, "request send failed");
        f.failed = true;
    } else {
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status != 200) {
            int n = esp_http_client_read_response(client, s_rx, RX_BYTES - 1);
            s_rx[n > 0 ? n : 0] = '\0';
            ESP_LOGW(TAG, "HTTP %d: %.200s", status, s_rx);
            f.failed = true;
        }
        while (!f.failed && !f.finished) {
            if (!wanted(job)) {
                ESP_LOGI(TAG, "job %u dropped", (unsigned)job);
                break;
            }
            int n = esp_http_client_read(client, s_rx, RX_BYTES);
            if (n < 0) {
                ESP_LOGW(TAG, "read failed (%d)", n);
                f.failed = true;
            } else if (n == 0) {
                break;
            } else {
                on_body(&f, s_rx, n);
            }
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    cJSON_free(body);

    ESP_LOGI(TAG, "job %u: %.1f s of speech in %lld ms%s", (unsigned)job,
             f.pcm_bytes / 2.0 / MUSE_TTS_RATE, (esp_timer_get_time() - f.t0) / 1000,
             f.failed ? " (failed)" : "");
    return !f.failed;
}

static void tts_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            uint32_t job = atomic_load(&s_want);
            if (job && job != atomic_load(&s_done)) {
                strlcpy(s_work, s_text, TEXT_MAX);
            } else {
                job = 0;
            }
            xSemaphoreGive(s_lock);
            if (!job) {
                break;
            }
            xStreamBufferReset(s_pcm);   /* the reader never blocks on it, so this can't fail */
            atomic_store(&s_bytes, 0);
            atomic_store(&s_ready, job);
            bool ok = fetch(job, s_work);
            atomic_store(&s_failed, !ok);
            atomic_store(&s_done, job);
        }
    }
}

bool muse_tts_init(void)
{
    if (!CONFIG_MUSE_MIMO_TTS_API_KEY[0]) {
        ESP_LOGI(TAG, "no MiMo API key: replies are captions only");
        return false;
    }
    if (s_task) {
        return true;
    }
    s_lock = xSemaphoreCreateMutex();
    s_pcm = xStreamBufferCreateWithCaps(PCM_BYTES, 1, MALLOC_CAP_SPIRAM);
    s_text = psram(TEXT_MAX);
    s_work = psram(TEXT_MAX);
    s_line = psram(EVENT_MAX);
    s_rx = psram(RX_BYTES);
    s_audio = psram(AUDIO_MAX);
    /* Stack in PSRAM, like the chat task's: TLS and cJSON run here. */
    if (!s_lock || !s_pcm || !s_text || !s_work || !s_line || !s_rx || !s_audio ||
        xTaskCreatePinnedToCoreWithCaps(tts_task, "muse_tts", 16 * 1024, NULL, 4, &s_task, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "start failed");
        s_task = NULL;
        return false;
    }
    ESP_LOGI(TAG, "MiMo TTS ready: %s, voice %s", CONFIG_MUSE_MIMO_TTS_MODEL, CONFIG_MUSE_MIMO_TTS_VOICE);
    return true;
}

uint32_t muse_tts_speak(const char *text)
{
    if (!s_task || !text) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_text, text, TEXT_MAX);
    bool spoken = false;
    for (const char *p = s_text; *p && !spoken; p++) {
        spoken = *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t';
    }
    uint32_t job = 0;
    if (spoken) {
        job = ++s_seq ? s_seq : ++s_seq;
        atomic_store(&s_want, job);
    }
    xSemaphoreGive(s_lock);
    if (job) {
        xTaskNotifyGive(s_task);
    }
    return job;
}

void muse_tts_stop(void)
{
    if (s_task) {
        atomic_store(&s_want, 0);
    }
}

size_t muse_tts_generated(uint32_t job, bool *all)
{
    *all = false;
    if (!s_task || atomic_load(&s_ready) != job) {
        return 0;
    }
    *all = atomic_load(&s_done) == job && !atomic_load(&s_failed);
    return atomic_load(&s_bytes) / sizeof(int16_t);
}

size_t muse_tts_read(uint32_t job, int16_t *out, size_t max, muse_tts_state_t *state)
{
    *state = MUSE_TTS_PENDING;
    if (!s_task || atomic_load(&s_ready) != job) {
        return 0;
    }
    /* Done first: everything it queued is in the buffer by then. */
    bool done = atomic_load(&s_done) == job;
    size_t n = xStreamBufferReceive(s_pcm, out, max * sizeof(int16_t), 0) / sizeof(int16_t);
    if (!n && done) {
        *state = atomic_load(&s_failed) ? MUSE_TTS_FAILED : MUSE_TTS_ENDED;
    }
    return n;
}
