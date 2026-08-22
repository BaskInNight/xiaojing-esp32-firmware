#include "voice_tts_idf.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "mbedtls/base64.h"

#define TTS_RESPONSE_CAPACITY (512U * 1024U)
#define TTS_REQUEST_CAPACITY 2048U
#define TTS_ESCAPED_TEXT_CAPACITY 1200U
#define TTS_MAX_TEXT_BYTES 512U

static const char *TAG = "voice_tts";

typedef struct {
    char *body;
    size_t capacity;
    size_t used;
    esp_err_t error;
} tts_http_sink_t;

static esp_err_t http_event(esp_http_client_event_t *event)
{
    tts_http_sink_t *sink = (tts_http_sink_t *)event->user_data;
    if (!sink) return ESP_OK;
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
        size_t length = (size_t)event->data_len;
        if (sink->used > sink->capacity - 1U ||
            length > sink->capacity - 1U - sink->used) {
            sink->error = ESP_ERR_NO_MEM;
            return ESP_FAIL;
        }
        memcpy(sink->body + sink->used, event->data, length);
        sink->used += length;
        sink->body[sink->used] = '\0';
    }
    return ESP_OK;
}

static bool json_escape(const char *input, char *output, size_t capacity)
{
    if (!input || !output || capacity == 0U) return false;
    size_t used = 0U;
    for (const unsigned char *p = (const unsigned char *)input; *p; p++) {
        const char *escape = NULL;
        if (*p == '"') escape = "\\\"";
        else if (*p == '\\') escape = "\\\\";
        else if (*p == '\n') escape = "\\n";
        else if (*p == '\r') escape = "\\r";
        else if (*p == '\t') escape = "\\t";
        if (*p < 0x20U && !escape) return false;
        if (escape) {
            if (used + 2U >= capacity) return false;
            output[used++] = escape[0];
            output[used++] = escape[1];
        } else {
            if (used + 1U >= capacity) return false;
            output[used++] = (char)*p;
        }
    }
    output[used] = '\0';
    return true;
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static esp_err_t parse_wav(uint8_t *wav, size_t length,
                           voice_tts_audio_t *out)
{
    if (!wav || !out || length < 44U || memcmp(wav, "RIFF", 4) != 0 ||
        memcmp(wav + 8, "WAVE", 4) != 0)
        return ESP_ERR_INVALID_RESPONSE;

    bool have_fmt = false;
    uint16_t format = 0U, channels = 0U, bits = 0U;
    uint32_t rate = 0U;
    size_t offset = 12U;
    while (offset + 8U <= length) {
        const uint8_t *chunk = wav + offset;
        uint32_t chunk_size = le32(chunk + 4);
        size_t data_offset = offset + 8U;
        if ((size_t)chunk_size > length - data_offset)
            return ESP_ERR_INVALID_RESPONSE;
        if (memcmp(chunk, "fmt ", 4) == 0 && chunk_size >= 16U) {
            format = le16(wav + data_offset);
            channels = le16(wav + data_offset + 2U);
            rate = le32(wav + data_offset + 4U);
            bits = le16(wav + data_offset + 14U);
            have_fmt = true;
        } else if (memcmp(chunk, "data", 4) == 0 && have_fmt) {
            if (format != 1U || channels != 1U || bits != 16U ||
                (rate != 16000U && rate != 24000U) || chunk_size == 0U ||
                (chunk_size & 1U) != 0U)
                return ESP_ERR_NOT_SUPPORTED;
            out->storage = wav;
            out->storage_bytes = length;
            out->pcm = (const int16_t *)(wav + data_offset);
            out->pcm_bytes = chunk_size;
            out->sample_rate = rate;
            out->duration_ms = (uint32_t)(((uint64_t)chunk_size * 1000U) /
                                          ((uint64_t)rate * 2U));
            return ESP_OK;
        }
        offset = data_offset + (size_t)chunk_size + (chunk_size & 1U);
    }
    return ESP_ERR_INVALID_RESPONSE;
}

esp_err_t voice_tts_idf_synthesize(const char *endpoint,
                                   const char *api_key,
                                   const char *utf8_text,
                                   voice_tts_audio_t *out)
{
    if (!endpoint || !endpoint[0] || !api_key || !api_key[0] ||
        !utf8_text || !utf8_text[0] || !out ||
        strlen(utf8_text) > TTS_MAX_TEXT_BYTES)
        return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    char escaped[TTS_ESCAPED_TEXT_CAPACITY];
    if (!json_escape(utf8_text, escaped, sizeof(escaped)))
        return ESP_ERR_INVALID_ARG;
    char request[TTS_REQUEST_CAPACITY];
    int request_length = snprintf(
        request, sizeof(request),
        "{\"model\":\"mimo-v2.5-tts\",\"messages\":["
        "{\"role\":\"user\",\"content\":"
        "\"请用知性、温柔、自然、清晰的年轻中文女声朗读，"
        "语速稍慢，情绪稳定，带轻微蜜糖般的亲和感；"
        "不要播音腔、客服腔、夸张撒娇，不要读标点。\"},"
        "{\"role\":\"assistant\",\"content\":\"%s\"}],"
        "\"audio\":{\"format\":\"wav\",\"voice\":\"mimo_default\"}}",
        escaped);
    memset(escaped, 0, sizeof(escaped));
    if (request_length <= 0 || (size_t)request_length >= sizeof(request))
        return ESP_ERR_INVALID_SIZE;

    char *response = heap_caps_malloc(
        TTS_RESPONSE_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!response) return ESP_ERR_NO_MEM;
    response[0] = '\0';
    tts_http_sink_t sink = {
        .body = response,
        .capacity = TTS_RESPONSE_CAPACITY,
        .used = 0U,
        .error = ESP_OK,
    };
    esp_http_client_config_t config = {
        .url = endpoint,
        .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event,
        .user_data = &sink,
        .keep_alive_enable = false,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(response);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_http_client_set_method(client, HTTP_METHOD_POST);
    if (err == ESP_OK) err = esp_http_client_set_header(
        client, "Content-Type", "application/json");
    if (err == ESP_OK) err = esp_http_client_set_header(
        client, "Accept-Encoding", "identity");
    if (err == ESP_OK) err = esp_http_client_set_header(
        client, "api-key", api_key);
    if (err == ESP_OK) err = esp_http_client_set_post_field(
        client, request, request_length);
    if (err == ESP_OK) err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    memset(request, 0, sizeof(request));
    if (err == ESP_OK && sink.error != ESP_OK) err = sink.error;
    if (err != ESP_OK || status < 200 || status >= 300) {
        ESP_LOGW(TAG, "TTS HTTP failed status=%d err=0x%x bytes=%u",
                 status, (unsigned)err, (unsigned)sink.used);
        free(response);
        return err != ESP_OK ? err : ESP_ERR_HTTP_BASE;
    }

    const char *audio_object = strstr(response, "\"audio\"");
    const char *data = audio_object ? strstr(audio_object, "\"data\":\"") : NULL;
    if (!data) {
        free(response);
        return ESP_ERR_INVALID_RESPONSE;
    }
    data += strlen("\"data\":\"");
    const char *end = strchr(data, '"');
    if (!end || end <= data) {
        free(response);
        return ESP_ERR_INVALID_RESPONSE;
    }
    size_t encoded_length = (size_t)(end - data);
    size_t wav_capacity = (encoded_length / 4U) * 3U + 3U;
    uint8_t *wav = heap_caps_malloc(
        wav_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!wav) {
        free(response);
        return ESP_ERR_NO_MEM;
    }
    size_t wav_length = 0U;
    int decode_result = mbedtls_base64_decode(
        wav, wav_capacity, &wav_length,
        (const unsigned char *)data, encoded_length);
    free(response);
    if (decode_result != 0) {
        free(wav);
        return ESP_ERR_INVALID_RESPONSE;
    }
    err = parse_wav(wav, wav_length, out);
    if (err != ESP_OK) {
        free(wav);
        memset(out, 0, sizeof(*out));
        return err;
    }
    ESP_LOGI(TAG, "TTS ready wav=%u pcm=%u rate=%u duration=%ums",
             (unsigned)wav_length, (unsigned)out->pcm_bytes,
             (unsigned)out->sample_rate, (unsigned)out->duration_ms);
    return ESP_OK;
}

void voice_tts_idf_release(voice_tts_audio_t *audio)
{
    if (!audio) return;
    free(audio->storage);
    memset(audio, 0, sizeof(*audio));
}
