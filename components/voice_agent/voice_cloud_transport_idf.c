#include "voice_cloud_transport_idf.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_coexist.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "voice_route.h"

/* ------------------------------------------------------------------ */
/* Compile-time switch: set to 1 to use set_post_field()+perform()    */
/* instead of the manual open/write/fetch flow.                       */
/* ------------------------------------------------------------------ */
#ifndef VOICE_CLOUD_USE_PERFORM
#define VOICE_CLOUD_USE_PERFORM 1
#endif

/* Keep each HTTPS write below the 1440-byte TCP MSS used by this target.
 * Larger writes were observed to stall awaiting an ACK on some Wi-Fi paths.
 * 768 raw bytes map exactly to 1024 base64 bytes and fit safely on the task
 * stack, avoiding both IP fragmentation and PSRAM-backed TLS buffers. */
#define VOICE_CLOUD_TX_RAW_CHUNK 768U
#define VOICE_CLOUD_TX_B64_CHUNK 1025U
#define VOICE_CLOUD_HTTP_BUFFER_SIZE 1024
#define VOICE_CLOUD_PREFIX_CAPACITY 1024U
#define VOICE_CLOUD_AUTH_HEADER_CAPACITY 264U

static const char *TAG = "voice_http";

static void restore_coex_balance(void)
{
    esp_err_t err = esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED)
        ESP_LOGW(TAG, "restore balanced coexistence failed: 0x%x", err);
}

static const char TOOL_REQUEST_SUFFIX[] =
    "\"}},{\"type\":\"text\",\"text\":\"Interpret the audio and call exactly "
    "one allowed tool.\"}]}],\"tools\":[{\"type\":\"function\",\"function\":{"
    "\"name\":\"submit_machine_intent\",\"description\":\"Submit one bounded "
    "machine intent; never control hardware directly.\",\"parameters\":{"
    "\"type\":\"object\",\"additionalProperties\":false,\"properties\":{"
    "\"tool\":{\"type\":\"string\",\"enum\":[\"start_program\",\"cancel\","
    "\"status\",\"ack_load\",\"ack_unload\",\"skip_uv\",\"ack_fault\"]},"
    "\"program\":{\"type\":\"string\",\"enum\":[\"formal\",\"demo\"]},"
    "\"allow_uv\":{\"type\":\"boolean\"},\"allow_dry\":{\"type\":\"boolean\"}},"
    "\"required\":[\"tool\"]}}}],\"tool_choice\":{\"type\":\"function\","
    "\"function\":{\"name\":\"submit_machine_intent\"}}}";

static const char AUTO_ROUTE_REQUEST_SUFFIX[] =
    "\"}},{\"type\":\"text\",\"text\":\"Transcribe this utterance verbatim. "
    "The transcript field is mandatory and must be non-empty. Return exactly "
    "one route_voice_request function call.\"}]}],\"tools\":[{"
    "\"type\":\"function\",\"function\":{\"name\":\"route_voice_request\","
    "\"description\":\"Classify the utterance and optionally submit one "
    "bounded machine intent. Never control hardware directly.\",\"parameters\":{"
    "\"type\":\"object\",\"additionalProperties\":false,\"properties\":{"
    "\"domain\":{\"type\":\"string\",\"enum\":[\"control\",\"status\",\"chat\","
    "\"clarify\",\"reject\"]},\"transcript\":{\"type\":\"string\","
    "\"minLength\":1,\"maxLength\":256},\"reply_text\":{\"type\":\"string\",\"maxLength\":512},"
    "\"confidence_milli\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":1000},"
    "\"requires_confirmation\":{\"type\":\"boolean\"},"
    "\"tool\":{\"type\":\"string\",\"enum\":[\"none\",\"start_program\","
    "\"cancel\",\"status\",\"ack_load\",\"ack_unload\",\"skip_uv\","
    "\"ack_fault\"]},\"program\":{\"type\":\"string\",\"enum\":[\"formal\","
    "\"demo\"]},\"allow_uv\":{\"type\":\"boolean\"},\"allow_dry\":{"
    "\"type\":\"boolean\"}},\"required\":[\"domain\",\"transcript\",\"reply_text\","
    "\"confidence_milli\",\"requires_confirmation\",\"tool\"]}}}],"
    "\"tool_choice\":{\"type\":\"function\",\"function\":{"
    "\"name\":\"route_voice_request\"}}}";

typedef struct {
    esp_http_client_handle_t client;
    voice_cloud_idf_context_t *cancel;
    int64_t deadline_us;
} transport_state_t;

void voice_cloud_idf_context_init(voice_cloud_idf_context_t *context)
{
    if (!context) return;
    atomic_init(&context->cancel_requested, false);
}

void voice_cloud_idf_cancel(voice_cloud_idf_context_t *context)
{
    if (!context) return;
    atomic_store_explicit(&context->cancel_requested, true,
                          memory_order_release);
}

static bool canceled(const transport_state_t *state)
{
    return state->cancel &&
           atomic_load_explicit(&state->cancel->cancel_requested,
                                memory_order_acquire);
}

static voice_cloud_result_t check_abort(const transport_state_t *state)
{
    if (canceled(state)) return VOICE_CLOUD_CANCELED;
    if (esp_timer_get_time() >= state->deadline_us)
        return VOICE_CLOUD_TIMEOUT;
    return VOICE_CLOUD_OK;
}

#if !VOICE_CLOUD_USE_PERFORM
static voice_cloud_result_t write_all(transport_state_t *state,
                                      const char *data, size_t length)
{
    size_t written = 0;
    while (written < length) {
        voice_cloud_result_t abort_result = check_abort(state);
        if (abort_result != VOICE_CLOUD_OK) return abort_result;
        size_t remaining = length - written;
        int chunk = remaining > (size_t)INT_MAX ?
                    INT_MAX : (int)remaining;
        int result = esp_http_client_write(
            state->client, data + written, chunk);
        if (result <= 0) return VOICE_CLOUD_TRANSPORT_ERROR;
        written += (size_t)result;
    }
    return VOICE_CLOUD_OK;
}

static voice_cloud_result_t write_base64(
    transport_state_t *state, const uint8_t *data, size_t length)
{
    unsigned char encoded[VOICE_CLOUD_TX_B64_CHUNK];
    voice_cloud_result_t final_result = VOICE_CLOUD_OK;
    size_t offset = 0;
    unsigned block_index = 0;
    while (offset < length) {
        size_t raw = length - offset;
        if (raw > VOICE_CLOUD_TX_RAW_CHUNK)
            raw = VOICE_CLOUD_TX_RAW_CHUNK;
        size_t encoded_length = 0;
        int result = mbedtls_base64_encode(
            encoded, sizeof(encoded), &encoded_length,
            data + offset, raw);
        if (result != 0) {
            final_result = VOICE_CLOUD_TRANSPORT_ERROR;
            break;
        }
        voice_cloud_result_t write_result = write_all(
            state, (const char *)encoded, encoded_length);
        if (write_result != VOICE_CLOUD_OK) {
            ESP_LOGW(TAG, "base64 upload stopped at block=%u raw=%u/%u result=%d",
                     block_index, (unsigned)offset, (unsigned)length,
                     (int)write_result);
            final_result = write_result;
            break;
        }
        offset += raw;
        block_index++;
    }
    if (final_result == VOICE_CLOUD_OK) {
        ESP_LOGI(TAG, "base64 upload complete blocks=%u raw=%u",
                 block_index, (unsigned)length);
    }
    return final_result;
}

static voice_cloud_result_t map_open_error(esp_err_t error)
{
    if (error == ESP_ERR_TIMEOUT) return VOICE_CLOUD_TIMEOUT;
    /* The secure transport is mandatory. Connection/handshake errors remain
     * distinguishable from HTTP response status errors. */
    return VOICE_CLOUD_TLS_ERROR;
}
#endif

/* ------------------------------------------------------------------ */
/* Event-handler sink for perform()-based transport                    */
/* ------------------------------------------------------------------ */

typedef struct {
    char *body;
    size_t capacity;
    size_t used;
    esp_err_t error;
} response_sink_t;

static esp_err_t response_event_handler(esp_http_client_event_t *event)
{
    response_sink_t *sink = (response_sink_t *)event->user_data;
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

/* ------------------------------------------------------------------ */
/* set_post_field() + perform() transport                              */
/* ------------------------------------------------------------------ */

static voice_cloud_result_t build_auth_header(
    esp_http_client_handle_t client,
    const voice_cloud_transport_request_t *request,
    char *authorization, size_t auth_capacity)
{
    if (request->auth == VOICE_CLOUD_AUTH_API_KEY) {
        return esp_http_client_set_header(client, "api-key",
                                          request->api_key) == ESP_OK
                   ? VOICE_CLOUD_OK
                   : VOICE_CLOUD_TRANSPORT_ERROR;
    }
    if (request->auth == VOICE_CLOUD_AUTH_BEARER) {
        int n = snprintf(authorization, auth_capacity,
                         "Bearer %s", request->api_key);
        if (n <= 7 || (size_t)n >= auth_capacity)
            return VOICE_CLOUD_BAD_CONFIG;
        return esp_http_client_set_header(client, "Authorization",
                                          authorization) == ESP_OK
                   ? VOICE_CLOUD_OK
                   : VOICE_CLOUD_TRANSPORT_ERROR;
    }
    return VOICE_CLOUD_BAD_ARG;
}

voice_cloud_result_t voice_cloud_idf_transport_perform(
    const voice_cloud_transport_request_t *request,
    voice_cloud_transport_response_t *response,
    void *context)
{
    if (!request || !response || !response->body ||
        response->body_capacity < 2) {
        return VOICE_CLOUD_BAD_ARG;
    }

    voice_cloud_idf_context_t *cancel =
        (voice_cloud_idf_context_t *)context;
    if (cancel) {
        atomic_store_explicit(&cancel->cancel_requested, false,
                              memory_order_release);
    }
    uint32_t overall_timeout_ms = request->overall_timeout_ms;
    if (overall_timeout_ms == 0U)
        overall_timeout_ms = request->connect_timeout_ms;
    if (overall_timeout_ms == 0U) return VOICE_CLOUD_BAD_CONFIG;
    transport_state_t state = {
        .client = NULL,
        .cancel = cancel,
        .deadline_us = esp_timer_get_time() +
            (int64_t)overall_timeout_ms * 1000,
    };

    bool auto_route = request->kind == VOICE_CLOUD_REQUEST_AUTO_ROUTE;
    if (!auto_route && request->kind != VOICE_CLOUD_REQUEST_TOOL_ONLY)
        return VOICE_CLOUD_BAD_ARG;

    const char *suffix = auto_route ? AUTO_ROUTE_REQUEST_SUFFIX
                                    : TOOL_REQUEST_SUFFIX;
    const char *source = request->invocation_source ==
                         VOICE_INVOCATION_BUTTON2_PTT
                         ? "button2_ptt" : "wake_word";
    const char *policy = request->route_policy ==
                         VOICE_ROUTE_POLICY_CHAT_PRIORITY
                         ? "CHAT_PRIORITY" : "AUTO";
    const char *system_instruction = auto_route
        ? "Return exactly one route_voice_request function call. Source=%s; "
          "route_policy=%s. AUTO distinguishes washing control, status, "
          "clarification, rejection, and casual chat. CHAT_PRIORITY is "
          "read-only: only status may be a tool; any mutating washing request "
          "must require confirmation and must use tool=none. Never request "
          "GPIO, relay, valve, heater, pump, PWM, motor, or raw posture "
          "control. Reply concisely in the user's language."
        : "Return exactly one submit_machine_intent function call. "
          "Never request GPIO, relay, valve, heater, pump, PWM, motor, "
          "or raw posture control.";

    char system_prompt[512];
    int system_length = auto_route
        ? snprintf(system_prompt, sizeof(system_prompt),
                   system_instruction, source, policy)
        : snprintf(system_prompt, sizeof(system_prompt),
                   "%s", system_instruction);
    if (system_length <= 0 || (size_t)system_length >= sizeof(system_prompt))
        return VOICE_CLOUD_BAD_CONFIG;

    char prefix[VOICE_CLOUD_PREFIX_CAPACITY];
    int prefix_length = snprintf(
        prefix, sizeof(prefix),
        "{\"model\":\"%s\",\"messages\":[{\"role\":\"system\",\"content\":"
        "\"%s\"},{\"role\":\"user\",\"content\":[{\"type\":\"input_audio\","
        "\"input_audio\":{\"data\":\"data:audio/wav;base64,",
        request->model, system_prompt);
    if (prefix_length <= 0 || (size_t)prefix_length >= sizeof(prefix))
        return VOICE_CLOUD_BAD_CONFIG;

    size_t base64_length = 4U * ((request->wav_length + 2U) / 3U);
    size_t suffix_length = strlen(suffix);
    if (base64_length > (size_t)INT_MAX ||
        (size_t)prefix_length > (size_t)INT_MAX - base64_length ||
        suffix_length > (size_t)INT_MAX -
            ((size_t)prefix_length + base64_length)) {
        return VOICE_CLOUD_WAV_TOO_LARGE;
    }
    size_t total_body = (size_t)prefix_length + base64_length + suffix_length;

    /* Allocate the complete JSON body in PSRAM.  At ~146KB for a 110KB WAV
     * this is comfortable within the 8MB PSRAM budget and avoids any
     * streaming/partial-write edge cases. */
    char *body = heap_caps_malloc(total_body + 1, MALLOC_CAP_SPIRAM);
    if (!body) return VOICE_CLOUD_TRANSPORT_ERROR;

    memcpy(body, prefix, (size_t)prefix_length);
    size_t body_used = (size_t)prefix_length;

    /* Base64-encode WAV data into the body buffer. */
    size_t enc_remaining = request->wav_length;
    size_t enc_offset = 0;
    while (enc_remaining > 0U) {
        size_t raw = enc_remaining > VOICE_CLOUD_TX_RAW_CHUNK
                         ? VOICE_CLOUD_TX_RAW_CHUNK : enc_remaining;
        size_t encoded_length = 0;
        int rc = mbedtls_base64_encode(
            (unsigned char *)body + body_used,
            total_body + 1 - body_used, &encoded_length,
            request->wav + enc_offset, raw);
        if (rc != 0) {
            heap_caps_free(body);
            return VOICE_CLOUD_TRANSPORT_ERROR;
        }
        body_used += encoded_length;
        enc_offset += raw;
        enc_remaining -= raw;
    }
    memcpy(body + body_used, suffix, suffix_length);
    body_used += suffix_length;
    body[body_used] = '\0';

    ESP_LOGI(TAG, "perform transport body=%u wav=%u internal_free=%u "
             "largest=%u psram_free=%u",
             (unsigned)body_used, (unsigned)request->wav_length,
             (unsigned)heap_caps_get_free_size(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(
                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

    /* Allocate response buffer in PSRAM. */
    char *resp_buf = heap_caps_malloc(
        response->body_capacity, MALLOC_CAP_SPIRAM);
    if (!resp_buf) {
        heap_caps_free(body);
        return VOICE_CLOUD_TRANSPORT_ERROR;
    }
    resp_buf[0] = '\0';
    response_sink_t sink = {
        .body = resp_buf,
        .capacity = response->body_capacity,
        .used = 0U,
        .error = ESP_OK,
    };

    /* Only attach certificate bundle for HTTPS endpoints. */
    bool use_tls = (strncmp(request->endpoint, "https://", 8) == 0);

    esp_http_client_config_t http_config = {
        .url = request->endpoint,
        .timeout_ms = (int)request->connect_timeout_ms,
        .crt_bundle_attach = use_tls ? esp_crt_bundle_attach : NULL,
        .event_handler = response_event_handler,
        .user_data = &sink,
        .keep_alive_enable = false,
        .buffer_size = VOICE_CLOUD_HTTP_BUFFER_SIZE,
        .buffer_size_tx = VOICE_CLOUD_HTTP_BUFFER_SIZE,
        /* The complete-body perform() path owns the request state and has a
         * bounded esp_http_client timeout.  Async mode can repeatedly return
         * EAGAIN while the TLS socket is waiting, which leaves the cloud
         * worker stuck despite the outer deadline. */
        .is_async = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (!client) {
        heap_caps_free(resp_buf);
        heap_caps_free(body);
        return VOICE_CLOUD_TRANSPORT_ERROR;
    }
    state.client = client;

    esp_err_t coex_err = esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
    if (coex_err != ESP_OK && coex_err != ESP_ERR_NOT_SUPPORTED)
        ESP_LOGW(TAG, "Wi-Fi coex preference failed: 0x%x", coex_err);

    esp_err_t err = esp_http_client_set_method(client, HTTP_METHOD_POST);
    if (err == ESP_OK)
        err = esp_http_client_set_header(
            client, "Content-Type", "application/json");
    if (err == ESP_OK)
        err = esp_http_client_set_header(
            client, "Accept-Encoding", "identity");

    char authorization[VOICE_CLOUD_AUTH_HEADER_CAPACITY];
    if (err == ESP_OK) {
        voice_cloud_result_t auth_result = build_auth_header(
            client, request, authorization, sizeof(authorization));
        if (auth_result != VOICE_CLOUD_OK) err = ESP_FAIL;
    }
    if (err == ESP_OK)
        err = esp_http_client_set_post_field(client, body, (int)body_used);

    int64_t t0 = esp_timer_get_time();
    int64_t t_connect = 0, t_perform = 0;
    voice_cloud_result_t abort_result = VOICE_CLOUD_OK;
    if (err == ESP_OK) {
        t_connect = esp_timer_get_time();
        ESP_LOGI(TAG, "perform: set_post_field done, calling perform()...");
        do {
            abort_result = check_abort(&state);
            if (abort_result != VOICE_CLOUD_OK) {
                (void)esp_http_client_close(client);
                break;
            }
            err = esp_http_client_perform(client);
            if (err == ESP_ERR_HTTP_EAGAIN)
                vTaskDelay(pdMS_TO_TICKS(1));
        } while (err == ESP_ERR_HTTP_EAGAIN);
        t_perform = esp_timer_get_time();
        ESP_LOGI(TAG, "perform: returned err=0x%x abort=%d elapsed=%lldms",
                 (unsigned)err, (int)abort_result,
                 (t_perform - t_connect) / 1000);
    }
    int64_t elapsed_ms = (esp_timer_get_time() - t0) / 1000;

    response->http_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    voice_cloud_result_t result;
    if (abort_result != VOICE_CLOUD_OK) {
        response->body[0] = '\0';
        response->body_length = 0;
        result = abort_result;
    } else if (err == ESP_OK && sink.error == ESP_OK) {
        size_t copy_len = sink.used < response->body_capacity - 1
                              ? sink.used : response->body_capacity - 1;
        memcpy(response->body, resp_buf, copy_len);
        response->body[copy_len] = '\0';
        response->body_length = copy_len;
        result = VOICE_CLOUD_OK;
        ESP_LOGI(TAG, "perform OK status=%d body=%u elapsed=%lldms",
                 response->http_status, (unsigned)copy_len,
                 (long long)elapsed_ms);
    } else {
        response->body[0] = '\0';
        response->body_length = 0;
        if (sink.error == ESP_ERR_NO_MEM)
            result = VOICE_CLOUD_RESPONSE_TOO_LARGE;
        else if (err == ESP_ERR_TIMEOUT)
            result = VOICE_CLOUD_TIMEOUT;
        else if (response->http_status >= 200 &&
                 response->http_status < 300)
            result = VOICE_CLOUD_TRANSPORT_ERROR;
        else
            result = VOICE_CLOUD_HTTP_ERROR;
        ESP_LOGW(TAG, "perform failed status=%d err=0x%x sink_err=0x%x "
                 "elapsed=%lldms",
                 response->http_status, (unsigned)err,
                 (unsigned)sink.error, (long long)elapsed_ms);
        /* Log bounded error body preview (no PII — error bodies are safe). */
        if (sink.used > 0) {
            int preview = sink.used > 384U ? 384 : (int)sink.used;
            ESP_LOGW(TAG, "error body preview=%.*s", preview, resp_buf);
        }
    }

    heap_caps_free(resp_buf);
    memset(body, 0, body_used);
    heap_caps_free(body);
    restore_coex_balance();

    if (response->http_status >= 200 && response->http_status < 300) {
        ESP_LOGI(TAG, "HTTP body length=%u", (unsigned)response->body_length);
    }
    return result;
}

voice_cloud_result_t voice_cloud_idf_transport(
    const voice_cloud_transport_request_t *request,
    voice_cloud_transport_response_t *response,
    void *context)
{
#if VOICE_CLOUD_USE_PERFORM
    return voice_cloud_idf_transport_perform(request, response, context);
#else
    if (!request || !response || !response->body ||
        response->body_capacity < 2) {
        return VOICE_CLOUD_BAD_ARG;
    }

    voice_cloud_idf_context_t *cancel =
        (voice_cloud_idf_context_t *)context;
    if (cancel) {
        atomic_store_explicit(&cancel->cancel_requested, false,
                              memory_order_release);
    }

    bool auto_route = request->kind == VOICE_CLOUD_REQUEST_AUTO_ROUTE;
    if (!auto_route && request->kind != VOICE_CLOUD_REQUEST_TOOL_ONLY)
        return VOICE_CLOUD_BAD_ARG;

    const char *suffix = auto_route ? AUTO_ROUTE_REQUEST_SUFFIX
                                    : TOOL_REQUEST_SUFFIX;
    const char *source = request->invocation_source ==
                         VOICE_INVOCATION_BUTTON2_PTT
                         ? "button2_ptt" : "wake_word";
    const char *policy = request->route_policy ==
                         VOICE_ROUTE_POLICY_CHAT_PRIORITY
                         ? "CHAT_PRIORITY" : "AUTO";
    const char *system_instruction = auto_route
        ? "Return exactly one route_voice_request function call. Source=%s; "
          "route_policy=%s. AUTO distinguishes washing control, status, "
          "clarification, rejection, and casual chat. CHAT_PRIORITY is "
          "read-only: only status may be a tool; any mutating washing request "
          "must require confirmation and must use tool=none. Never request "
          "GPIO, relay, valve, heater, pump, PWM, motor, or raw posture "
          "control. Reply concisely in the user's language."
        : "Return exactly one submit_machine_intent function call. "
          "Never request GPIO, relay, valve, heater, pump, PWM, motor, "
          "or raw posture control.";

    char system_prompt[512];
    int system_length = auto_route
        ? snprintf(system_prompt, sizeof(system_prompt),
                   system_instruction, source, policy)
        : snprintf(system_prompt, sizeof(system_prompt),
                   "%s", system_instruction);
    if (system_length <= 0 ||
        (size_t)system_length >= sizeof(system_prompt))
        return VOICE_CLOUD_BAD_CONFIG;

    char prefix[VOICE_CLOUD_PREFIX_CAPACITY];
    int prefix_length = snprintf(
        prefix, sizeof(prefix),
        "{\"model\":\"%s\",\"messages\":[{\"role\":\"system\",\"content\":"
        "\"%s\"},{\"role\":\"user\",\"content\":[{\"type\":\"input_audio\","
        "\"input_audio\":{\"data\":\"data:audio/wav;base64,",
        request->model, system_prompt);
    if (prefix_length <= 0 ||
        (size_t)prefix_length >= sizeof(prefix)) {
        return VOICE_CLOUD_BAD_CONFIG;
    }

    size_t base64_length = 4U * ((request->wav_length + 2U) / 3U);
    size_t suffix_length = strlen(suffix);
    if (base64_length > (size_t)INT_MAX ||
        (size_t)prefix_length > (size_t)INT_MAX - base64_length ||
        suffix_length > (size_t)INT_MAX -
            ((size_t)prefix_length + base64_length)) {
        return VOICE_CLOUD_WAV_TOO_LARGE;
    }
    int content_length = (int)((size_t)prefix_length +
                               base64_length + suffix_length);

    esp_http_client_config_t http_config = {
        .url = request->endpoint,
        .timeout_ms = (int)request->connect_timeout_ms,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
        .buffer_size = VOICE_CLOUD_HTTP_BUFFER_SIZE,
        .buffer_size_tx = VOICE_CLOUD_HTTP_BUFFER_SIZE,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (!client) return VOICE_CLOUD_TRANSPORT_ERROR;

    ESP_LOGI(TAG,
             "HTTP open endpoint=%s body=%d "
             "internal_free=%u largest=%u",
             request->endpoint,
             content_length,
             (unsigned)heap_caps_get_free_size(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    transport_state_t state = {
        .client = client,
        .cancel = cancel,
        .deadline_us = esp_timer_get_time() +
            (int64_t)request->overall_timeout_ms * 1000,
    };
    response->body_length = 0;
    response->http_status = 0;

    esp_err_t error = esp_http_client_set_method(
        client, HTTP_METHOD_POST);
    if (error == ESP_OK) {
        error = esp_http_client_set_header(
            client, "Content-Type", "application/json");
    }
    if (error == ESP_OK) {
        error = esp_http_client_set_header(
            client, "Accept-Encoding", "identity");
    }
    char authorization[VOICE_CLOUD_AUTH_HEADER_CAPACITY];
    if (error == ESP_OK &&
        request->auth == VOICE_CLOUD_AUTH_API_KEY) {
        error = esp_http_client_set_header(
            client, "api-key", request->api_key);
    } else if (error == ESP_OK &&
               request->auth == VOICE_CLOUD_AUTH_BEARER) {
        int auth_length = snprintf(
            authorization, sizeof(authorization),
            "Bearer %s", request->api_key);
        if (auth_length <= 7 ||
            (size_t)auth_length >= sizeof(authorization)) {
            error = ESP_ERR_INVALID_ARG;
        } else {
            error = esp_http_client_set_header(
                client, "Authorization", authorization);
        }
    } else if (error == ESP_OK) {
        error = ESP_ERR_INVALID_ARG;
    }
    if (error != ESP_OK) {
        esp_http_client_cleanup(client);
        return VOICE_CLOUD_TRANSPORT_ERROR;
    }

    esp_err_t coex_err = esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
    if (coex_err != ESP_OK && coex_err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "Wi-Fi coexistence preference failed: 0x%x", coex_err);
    } else {
        ESP_LOGI(TAG, "coexistence preference: Wi-Fi during cloud upload");
    }

    error = esp_http_client_open(client, content_length);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s (0x%x), errno=%d, "
                 "internal_free=%u largest=%u",
                 esp_err_to_name(error), (unsigned)error,
                 esp_http_client_get_errno(client),
                 (unsigned)heap_caps_get_free_size(
                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(
                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        voice_cloud_result_t result = map_open_error(error);
        esp_http_client_cleanup(client);
        restore_coex_balance();
        return result;
    }
    ESP_LOGI(TAG, "HTTP TLS connected; streaming request body");

    voice_cloud_result_t result = write_all(
        &state, prefix, (size_t)prefix_length);
    if (result == VOICE_CLOUD_OK) {
        result = write_base64(&state, request->wav, request->wav_length);
    }
    if (result == VOICE_CLOUD_OK) {
        result = write_all(&state, suffix, suffix_length);
    }
    if (result != VOICE_CLOUD_OK) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        restore_coex_balance();
        return result;
    }

    ESP_LOGI(TAG, "HTTP request body sent; waiting for response headers");

    int64_t header_length = esp_http_client_fetch_headers(client);
    if (header_length < 0) {
        result = check_abort(&state);
        if (result == VOICE_CLOUD_OK)
            result = VOICE_CLOUD_TRANSPORT_ERROR;
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        restore_coex_balance();
        return result;
    }
    response->http_status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP response status=%d header_length=%lld",
             response->http_status, (long long)header_length);

    size_t used = 0;
    while (used + 1 < response->body_capacity) {
        result = check_abort(&state);
        if (result != VOICE_CLOUD_OK) break;
        size_t remaining = response->body_capacity - used - 1;
        int request_length = remaining > 1024 ? 1024 : (int)remaining;
        int read_count = esp_http_client_read(
            client, response->body + used, request_length);
        if (read_count < 0) {
            result = VOICE_CLOUD_TRANSPORT_ERROR;
            break;
        }
        if (read_count == 0) break;
        used += (size_t)read_count;
    }
    if (result == VOICE_CLOUD_OK &&
        used + 1 == response->body_capacity) {
        char extra;
        int extra_count = esp_http_client_read(client, &extra, 1);
        if (extra_count > 0) result = VOICE_CLOUD_RESPONSE_TOO_LARGE;
        else if (extra_count < 0) result = VOICE_CLOUD_TRANSPORT_ERROR;
    }
    response->body[used] = '\0';
    response->body_length = used;

    /* Successful responses contain the user's transcript and must not be
     * emitted to production serial logs. Error bodies remain bounded for
     * provider diagnostics and never contain request headers/API keys. */
    if (response->http_status >= 200 && response->http_status < 300) {
        ESP_LOGI(TAG, "HTTP body length=%u", (unsigned)used);
    } else {
        int preview = used > 384U ? 384 : (int)used;
        ESP_LOGW(TAG, "HTTP error body length=%u preview=%.*s",
                 (unsigned)used, preview, response->body);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    restore_coex_balance();
    return result;
#endif /* !VOICE_CLOUD_USE_PERFORM */
}

/* ================================================================== */
/* Diagnostic mode: auto-test transport on boot (no button needed)    */
/* ================================================================== */

#if VOICE_CLOUD_DIAGNOSTIC_MODE

#ifndef VOICE_CLOUD_DIAG_LAN_URL
#define VOICE_CLOUD_DIAG_LAN_URL "http://10.196.41.158:8089/post"
#endif

#ifndef VOICE_CLOUD_DIAG_MIMO_ENDPOINT
#define VOICE_CLOUD_DIAG_MIMO_ENDPOINT "https://api.xiaomimimo.com/v1/chat/completions"
#endif

#ifndef VOICE_CLOUD_DIAG_MIMO_MODEL
#define VOICE_CLOUD_DIAG_MIMO_MODEL "mimo-v2.5"
#endif

/* Generate a synthetic 16kHz mono PCM16 WAV into PSRAM.
 * Returns 0 on success; *out_wav and *out_len set. */
static int generate_synth_wav(uint32_t duration_ms,
                              uint8_t **out_wav, size_t *out_len)
{
    const uint32_t sample_rate = 16000U;
    const uint32_t num_samples = sample_rate * duration_ms / 1000U;
    const uint32_t data_bytes = num_samples * 2U;
    const uint32_t wav_bytes = 44U + data_bytes;

    uint8_t *wav = heap_caps_malloc(wav_bytes, MALLOC_CAP_SPIRAM);
    if (!wav) return -1;

    /* Minimal RIFF/WAVE header */
    memset(wav, 0, 44);
    memcpy(wav, "RIFF", 4);
    uint32_t chunk_size = wav_bytes - 8;
    memcpy(wav + 4, &chunk_size, 4);
    memcpy(wav + 8, "WAVE", 4);
    memcpy(wav + 12, "fmt ", 4);
    uint32_t fmt_size = 16;
    memcpy(wav + 16, &fmt_size, 4);
    uint16_t fmt_tag = 1; /* PCM */
    memcpy(wav + 20, &fmt_tag, 2);
    uint16_t channels = 1;
    memcpy(wav + 22, &channels, 2);
    memcpy(wav + 24, &sample_rate, 4);
    uint32_t byte_rate = sample_rate * 2;
    memcpy(wav + 28, &byte_rate, 4);
    uint16_t block_align = 2;
    memcpy(wav + 32, &block_align, 2);
    uint16_t bits = 16;
    memcpy(wav + 34, &bits, 2);
    memcpy(wav + 36, "data", 4);
    memcpy(wav + 40, &data_bytes, 4);

    /* Fill with a 440Hz sine at ~50% amplitude to produce measurable RMS. */
    int16_t *pcm = (int16_t *)(wav + 44);
    for (uint32_t i = 0; i < num_samples; i++) {
        /* Approximate sin using integer math: 440Hz at 16kHz sample rate */
        float phase = (float)i * 2.0f * 3.14159265f * 440.0f / (float)sample_rate;
        pcm[i] = (int16_t)(16000.0f * sinf(phase));
    }

    *out_wav = wav;
    *out_len = (size_t)wav_bytes;
    return 0;
}

static void diag_log_heap(const char *tag)
{
    ESP_LOGI("diag", "%s internal_free=%u largest=%u psram_free=%u",
             tag,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

/* Run one diagnostic request. Returns VOICE_CLOUD_OK on success. */
static voice_cloud_result_t diag_run_one(
    const char *label,
    const char *endpoint,
    const char *api_key,
    voice_cloud_auth_t auth,
    const uint8_t *wav, size_t wav_len,
    bool require_choices)
{
    char *resp = heap_caps_malloc(16384, MALLOC_CAP_SPIRAM);
    if (!resp) {
        ESP_LOGE("diag", "alloc resp failed");
        return VOICE_CLOUD_TRANSPORT_ERROR;
    }
    voice_cloud_transport_response_t response = {
        .body = resp,
        .body_capacity = 16384,
    };
    voice_cloud_transport_request_t request = {
        .endpoint = endpoint,
        .api_key = api_key,
        .model = VOICE_CLOUD_DIAG_MIMO_MODEL,
        .auth = auth,
        .kind = VOICE_CLOUD_REQUEST_AUTO_ROUTE,
        .invocation_source = VOICE_INVOCATION_WAKE_WORD,
        .route_policy = VOICE_ROUTE_POLICY_AUTO,
        .wav = wav,
        .wav_length = wav_len,
        .connect_timeout_ms = 20000,
        .overall_timeout_ms = 60000,
        .max_response_bytes = 4096,
    };

    diag_log_heap("before");
    int64_t t0 = esp_timer_get_time();
    voice_cloud_result_t result =
        voice_cloud_idf_transport_perform(&request, &response, NULL);
    int64_t elapsed = (esp_timer_get_time() - t0) / 1000;
    diag_log_heap("after");

    ESP_LOGI("diag", "[%s] result=%d status=%d body=%u elapsed=%lldms",
             label, (int)result, response.http_status,
             (unsigned)response.body_length, (long long)elapsed);
    bool http_ok = response.http_status >= 200 && response.http_status < 300;
    bool shape_ok = !require_choices ||
                    (response.body_length > 0U &&
                     strstr(resp, "\"choices\"") != NULL);
    if (result == VOICE_CLOUD_OK && (!http_ok || !shape_ok)) {
        result = http_ok ? VOICE_CLOUD_BAD_RESPONSE
                         : VOICE_CLOUD_HTTP_ERROR;
        ESP_LOGW("diag", "[%s] rejected status=%d shape_ok=%d",
                 label, response.http_status, (int)shape_ok);
    }
    if (result != VOICE_CLOUD_OK && response.body_length > 0) {
        int preview = response.body_length > 256 ? 256 : (int)response.body_length;
        ESP_LOGI("diag", "[%s] body_preview=%.*s", label, preview, resp);
    }
    heap_caps_free(resp);
    return result;
}

typedef struct {
    char mimo_endpoint[128];
    char mimo_key[128];
} diag_context_t;

static void diagnostic_task(void *arg)
{
    diag_context_t *ctx = (diag_context_t *)arg;
    ESP_LOGI("diag", "===== TRANSPORT DIAGNOSTIC START =====");
    diag_log_heap("boot");

    /* Wait for Wi-Fi to be ready (up to 30s). */
    for (int i = 0; i < 30; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    /* --- Test A: LAN HTTP (no TLS, no auth) --- */
    ESP_LOGI("diag", "--- Test A: LAN HTTP POST ---");
    ESP_LOGI("diag", "NOTE: ESP32 IP must be on same subnet as LAN server");
    {
        uint8_t *wav = NULL;
        size_t wav_len = 0;
        if (generate_synth_wav(1000, &wav, &wav_len) == 0) {
            for (int i = 0; i < 3; i++) {
                ESP_LOGI("diag", "LAN round %d/3", i + 1);
                voice_cloud_result_t r = diag_run_one(
                    "LAN", VOICE_CLOUD_DIAG_LAN_URL, "test",
                    VOICE_CLOUD_AUTH_API_KEY, wav, wav_len, false);
                if (r != VOICE_CLOUD_OK)
                    ESP_LOGW("diag", "LAN round %d FAILED result=%d",
                             i + 1, (int)r);
            }
            heap_caps_free(wav);
        } else {
            ESP_LOGE("diag", "synth WAV alloc failed");
        }
    }

    /* --- Test B: MiMo HTTPS with synthetic audio (5 rounds) --- */
    if (ctx && ctx->mimo_endpoint[0] && ctx->mimo_key[0]) {
        ESP_LOGI("diag", "--- Test B: MiMo HTTPS (synthetic WAV, 5 rounds) ---");
        uint8_t *wav = NULL;
        size_t wav_len = 0;
        if (generate_synth_wav(1000, &wav, &wav_len) == 0) {
            int pass = 0, fail = 0;
            for (int i = 0; i < 5; i++) {
                ESP_LOGI("diag", "MiMo round %d/5", i + 1);
                voice_cloud_result_t r = diag_run_one(
                    "MiMo", ctx->mimo_endpoint, ctx->mimo_key,
                    VOICE_CLOUD_AUTH_API_KEY, wav, wav_len, true);
                if (r == VOICE_CLOUD_OK) {
                    pass++;
                } else {
                    fail++;
                    ESP_LOGW("diag", "MiMo round %d FAILED result=%d",
                             i + 1, (int)r);
                }
                vTaskDelay(pdMS_TO_TICKS(3000));
            }
            ESP_LOGI("diag", "MiMo results: %d PASS / %d FAIL / 5 total",
                     pass, fail);
            heap_caps_free(wav);
        } else {
            ESP_LOGE("diag", "synth WAV alloc failed for MiMo test");
        }
    } else {
        ESP_LOGW("diag", "MiMo test skipped (no credentials)");
    }

    ESP_LOGI("diag", "===== TRANSPORT DIAGNOSTIC END =====");
    if (ctx) heap_caps_free(ctx);
    vTaskDelete(NULL);
}

void voice_cloud_diag_start_ex(const char *mimo_endpoint,
                               const char *mimo_key)
{
    diag_context_t *ctx = heap_caps_calloc(1, sizeof(diag_context_t),
                                           MALLOC_CAP_SPIRAM);
    if (ctx) {
        if (mimo_endpoint)
            strlcpy(ctx->mimo_endpoint, mimo_endpoint,
                    sizeof(ctx->mimo_endpoint));
        if (mimo_key)
            strlcpy(ctx->mimo_key, mimo_key, sizeof(ctx->mimo_key));
    }
    xTaskCreatePinnedToCore(diagnostic_task, "transport_diag",
                            8192, ctx, 3, NULL, 1);
}

void voice_cloud_diag_start(void)
{
    voice_cloud_diag_start_ex(NULL, NULL);
}

#endif /* VOICE_CLOUD_DIAGNOSTIC_MODE */
