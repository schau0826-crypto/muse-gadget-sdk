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
 * Xiaomi MiMo speech recognition: POST /v1/chat/completions with the
 * recording as a base64 WAV data URL in a single input_audio part, model
 * mimo-v2.5-asr. The reply is plain JSON; choices[0].message.content is the
 * text. Same API key and endpoint as the TTS.
 *
 * The request runs on the caller's task, after push-to-talk is released, so
 * the chat session keeps its single-threaded turn handling.
 */

#include "muse_asr_mimo.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "muse_chat_priv.h"
#include "sdkconfig.h"

#ifndef CONFIG_MUSE_MIMO_ASR
#define CONFIG_MUSE_MIMO_ASR 0
#endif
#ifndef CONFIG_MUSE_MIMO_TTS_API_KEY
#define CONFIG_MUSE_MIMO_TTS_API_KEY ""
#endif
#ifndef CONFIG_MUSE_MIMO_ASR_LANGUAGE
#define CONFIG_MUSE_MIMO_ASR_LANGUAGE "zh"
#endif

static const char *TAG = "muse_asr";

#define ASR_URL "https://api.xiaomimimo.com/v1/chat/completions"
#define ASR_MODEL "mimo-v2.5-asr"
#define ASR_RATE 16000
#define ASR_MAX_BYTES (ASR_RATE * 2 * 10)   /* 10 s of 16 kHz mono PCM16 */
#define RX_MAX 4096
#define TEXT_MAX 2048
#define TIMEOUT_MS 30000

static uint8_t *s_wav;    /* 44-byte header + PCM */
static char *s_json;      /* the whole request body */
static char *s_rx;
static char *s_text;
static size_t s_json_cap;
static bool s_ready;

static void *psram(size_t n)
{
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void wav_header(uint8_t *h, uint32_t bytes)
{
    memcpy(h, "RIFF", 4);
    put32(h + 4, 36 + bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);          /* fmt chunk size */
    put16(h + 20, 1);           /* PCM */
    put16(h + 22, 1);           /* mono */
    put32(h + 24, ASR_RATE);
    put32(h + 28, ASR_RATE * 2);
    put16(h + 32, 2);           /* block align */
    put16(h + 34, 16);          /* bits per sample */
    memcpy(h + 36, "data", 4);
    put32(h + 40, bytes);
}

bool muse_asr_init(void)
{
    if (!CONFIG_MUSE_MIMO_ASR) {
        return false;
    }
    if (!CONFIG_MUSE_MIMO_TTS_API_KEY[0]) {
        ESP_LOGW(TAG, "no MiMo API key: ASR stays off");
        return false;
    }
    if (s_ready) {
        return true;
    }
    /* The request body holds the base64 of the whole WAV. */
    s_wav = (uint8_t *)psram(44 + ASR_MAX_BYTES);
    s_json_cap = (44 + ASR_MAX_BYTES + 2) / 3 * 4 + 2048;
    s_json = (char *)psram(s_json_cap);
    s_rx = (char *)psram(RX_MAX);
    s_text = (char *)psram(TEXT_MAX);
    if (!s_wav || !s_json || !s_rx || !s_text) {
        ESP_LOGE(TAG, "out of PSRAM for ASR buffers");
        return false;
    }
    s_ready = true;
    ESP_LOGI(TAG, "MiMo ASR ready: %s, language %s", ASR_MODEL, CONFIG_MUSE_MIMO_ASR_LANGUAGE);
    return true;
}

const char *muse_asr_transcribe(const int16_t *pcm, size_t samples)
{
    if (!s_ready || !pcm || !samples) {
        return NULL;
    }
    size_t bytes = samples * 2;
    if (bytes > ASR_MAX_BYTES) {
        bytes = ASR_MAX_BYTES;
    }
    wav_header(s_wav, (uint32_t)bytes);
    memcpy(s_wav + 44, pcm, bytes);
    size_t wav_len = 44 + bytes;

    static const char PREFIX[] =
        "{\"model\":\"" ASR_MODEL "\",\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":"
        "\"input_audio\",\"input_audio\":{\"data\":\"data:audio/wav;base64,";
    static const char SUFFIX[] = "\"}}]}],\"asr_options\":{\"language\":\"" CONFIG_MUSE_MIMO_ASR_LANGUAGE "\"}}";

    size_t prefix = sizeof(PREFIX) - 1;
    size_t b64 = (wav_len + 2) / 3 * 4;
    if (prefix + b64 + sizeof(SUFFIX) >= s_json_cap) {
        ESP_LOGE(TAG, "request body too small");
        return NULL;
    }
    memcpy(s_json, PREFIX, prefix);
    size_t made = muse_hatch_base64(s_wav, wav_len, s_json + prefix);
    memcpy(s_json + prefix + made, SUFFIX, sizeof(SUFFIX) - 1);
    size_t body_len = prefix + made + sizeof(SUFFIX) - 1;
    s_json[body_len] = '\0';

    esp_http_client_config_t cfg = {
        .url = ASR_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = TIMEOUT_MS,
        .buffer_size = RX_MAX,
        .buffer_size_tx = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGW(TAG, "client init failed");
        return NULL;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "api-key", CONFIG_MUSE_MIMO_TTS_API_KEY);

    int status = 0;
    size_t got = 0;
    esp_err_t err = esp_http_client_open(client, (int)body_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "connect failed: %s", esp_err_to_name(err));
    } else if (esp_http_client_write(client, s_json, (int)body_len) != (int)body_len) {
        ESP_LOGW(TAG, "request send failed");
    } else {
        esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        for (;;) {
            if (got >= RX_MAX - 1) {
                break;
            }
            int n = esp_http_client_read(client, s_rx + got, (int)(RX_MAX - 1 - got));
            if (n <= 0) {
                break;
            }
            got += (size_t)n;
        }
        s_rx[got] = '\0';
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status != 200) {
        ESP_LOGW(TAG, "HTTP %d: %.200s", status, s_rx[0] ? s_rx : "(no body)");
        return NULL;
    }
    cJSON *root = cJSON_Parse(s_rx);
    if (!root) {
        ESP_LOGW(TAG, "bad JSON: %.200s", s_rx);
        return NULL;
    }
    const char *text = NULL;
    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    cJSON *first = cJSON_GetArrayItem(choices, 0);
    cJSON *message = cJSON_GetObjectItem(first, "message");
    cJSON *content = cJSON_GetObjectItem(message, "content");
    if (cJSON_IsString(content) && content->valuestring[0]) {
        strlcpy(s_text, content->valuestring, TEXT_MAX);
        text = s_text;
    }
    cJSON_Delete(root);
    if (text) {
        ESP_LOGI(TAG, "recognized: \"%s\"", text);
    } else {
        ESP_LOGW(TAG, "no text in response");
    }
    return text;
}
