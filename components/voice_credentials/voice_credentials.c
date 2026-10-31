#include "voice_credentials.h"

#include <stdatomic.h>
#include <math.h>
#include <string.h>

#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#define VOICE_CREDENTIAL_NAMESPACE "xj_voice"
#define VOICE_CREDENTIAL_SCHEMA 1U
#define LOCK_TIMEOUT_MS 1000U

typedef struct {
    bool stored;
    voice_provider_t provider;
    char model[VOICE_CREDENTIAL_MODEL_CAPACITY];
    uint32_t revision;
    esp_err_t last_error;
} credential_runtime_t;

static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock;
static credential_runtime_t s_rt;
static _Atomic uint32_t s_window_deadline;
static _Atomic int s_init_state;

enum {
    INIT_UNINITIALIZED = 0,
    INIT_INITIALIZING,
    INIT_READY,
    INIT_FAILED,
};

static bool take_lock(void)
{
    if (!s_lock) return false;
    TickType_t ticks = pdMS_TO_TICKS(LOCK_TIMEOUT_MS);
    if (ticks == 0) ticks = 1;
    return xSemaphoreTake(s_lock, ticks) == pdTRUE;
}

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static bool deadline_pending(uint32_t now, uint32_t deadline)
{
    return deadline != 0U && (int32_t)(deadline - now) > 0;
}

static bool bounded_printable(const char *value, size_t capacity,
                              bool allow_empty)
{
    if (!value) return false;
    size_t length = strnlen(value, capacity);
    if (length >= capacity || (!allow_empty && length == 0)) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)value[i];
        if (c < 0x21U || c > 0x7eU) return false;
    }
    return true;
}

static voice_cloud_result_t validation_transport(
    const voice_cloud_transport_request_t *request,
    voice_cloud_transport_response_t *response, void *context)
{
    (void)request;
    (void)response;
    (void)context;
    return VOICE_CLOUD_TRANSPORT_ERROR;
}

static bool validation_network_ready(void *context)
{
    (void)context;
    return false;
}

bool voice_credentials_value_valid(const voice_credentials_value_t *value)
{
    if (!value || value->provider < VOICE_PROVIDER_MIMO_DIRECT ||
        value->provider >= VOICE_PROVIDER_COUNT ||
        !bounded_printable(value->api_key, sizeof(value->api_key), false) ||
        !bounded_printable(value->model, sizeof(value->model), true))
        return false;
    voice_provider_credentials_t provider = {
        .provider = value->provider,
        .api_key = value->api_key,
        .model_override = value->model,
        .endpoint_override = NULL,
    };
    voice_cloud_config_t cloud;
    return voice_provider_build_cloud_config(
        &provider, validation_transport, NULL,
        validation_network_ready, NULL, &cloud) == VOICE_PROVIDER_OK;
}

esp_err_t voice_credentials_global_init(void)
{
    int state = atomic_load_explicit(&s_init_state, memory_order_acquire);
    if (state == INIT_READY) return ESP_OK;
    if (state == INIT_FAILED) return ESP_ERR_NO_MEM;
    int expected = INIT_UNINITIALIZED;
    if (atomic_compare_exchange_strong_explicit(
            &s_init_state, &expected, INIT_INITIALIZING,
            memory_order_acq_rel, memory_order_acquire)) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
        bool ok = s_lock != NULL;
        if (ok) memset(&s_rt, 0, sizeof(s_rt));
        atomic_store_explicit(
            &s_init_state, ok ? INIT_READY : INIT_FAILED,
            memory_order_release);
        return ok ? ESP_OK : ESP_ERR_NO_MEM;
    }
    for (uint32_t i = 0; i < 1000U; i++) {
        state = atomic_load_explicit(&s_init_state, memory_order_acquire);
        if (state == INIT_READY) return ESP_OK;
        if (state == INIT_FAILED) return ESP_ERR_NO_MEM;
        taskYIELD();
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t voice_credentials_load(voice_credentials_value_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    voice_credentials_value_t value;
    memset(&value, 0, sizeof(value));
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(
        VOICE_CREDENTIAL_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    uint8_t schema = 0;
    uint8_t provider = 0;
    size_t key_size = sizeof(value.api_key);
    size_t model_size = sizeof(value.model);
    err = nvs_get_u8(handle, "schema", &schema);
    if (err == ESP_OK) err = nvs_get_u8(handle, "provider", &provider);
    if (err == ESP_OK)
        err = nvs_get_str(handle, "api_key", value.api_key, &key_size);
    if (err == ESP_OK) {
        esp_err_t model_err =
            nvs_get_str(handle, "model", value.model, &model_size);
        if (model_err == ESP_ERR_NVS_NOT_FOUND) value.model[0] = '\0';
        else err = model_err;
    }
    nvs_close(handle);
    if (err != ESP_OK) return err;
    value.provider = (voice_provider_t)provider;
    if (schema != VOICE_CREDENTIAL_SCHEMA ||
        !voice_credentials_value_valid(&value))
        return ESP_ERR_INVALID_STATE;
    if (take_lock()) {
        s_rt.stored = true;
        s_rt.provider = value.provider;
        memcpy(s_rt.model, value.model, sizeof(s_rt.model));
        s_rt.last_error = ESP_OK;
        xSemaphoreGive(s_lock);
    }
    *out = value;
    memset(&value, 0, sizeof(value));
    return ESP_OK;
}

esp_err_t voice_credentials_save(const voice_credentials_value_t *value)
{
    if (!voice_credentials_value_valid(value)) return ESP_ERR_INVALID_ARG;
    if (!voice_credentials_provisioning_window_is_open())
        return ESP_ERR_INVALID_STATE;
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(
        VOICE_CREDENTIAL_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK)
        err = nvs_set_u8(handle, "schema", VOICE_CREDENTIAL_SCHEMA);
    if (err == ESP_OK)
        err = nvs_set_u8(handle, "provider", (uint8_t)value->provider);
    if (err == ESP_OK) err = nvs_set_str(handle, "api_key", value->api_key);
    if (err == ESP_OK) err = nvs_set_str(handle, "model", value->model);
    if (err == ESP_OK) err = nvs_commit(handle);
    if (handle) nvs_close(handle);
    if (take_lock()) {
        s_rt.last_error = err;
        if (err == ESP_OK) {
            s_rt.stored = true;
            s_rt.provider = value->provider;
            memcpy(s_rt.model, value->model, sizeof(s_rt.model));
            s_rt.revision++;
            if (s_rt.revision == 0U) s_rt.revision = 1U;
        }
        xSemaphoreGive(s_lock);
    }
    if (err == ESP_OK) voice_credentials_close_provisioning_window();
    return err;
}

esp_err_t voice_credentials_erase(void)
{
    if (!voice_credentials_provisioning_window_is_open())
        return ESP_ERR_INVALID_STATE;
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(
        VOICE_CREDENTIAL_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) err = nvs_erase_all(handle);
    if (err == ESP_OK) err = nvs_commit(handle);
    if (handle) nvs_close(handle);
    if (take_lock()) {
        s_rt.last_error = err;
        if (err == ESP_OK) {
            s_rt.stored = false;
            s_rt.provider = VOICE_PROVIDER_MIMO_DIRECT;
            memset(s_rt.model, 0, sizeof(s_rt.model));
            s_rt.revision++;
            if (s_rt.revision == 0U) s_rt.revision = 1U;
        }
        xSemaphoreGive(s_lock);
    }
    if (err == ESP_OK) voice_credentials_close_provisioning_window();
    return err;
}

esp_err_t voice_credentials_open_provisioning_window(uint32_t duration_ms)
{
    if (duration_ms < 1000U || duration_ms > 300000U)
        return ESP_ERR_INVALID_ARG;
    uint32_t deadline = now_ms() + duration_ms;
    if (deadline == 0U) deadline = 1U;
    atomic_store_explicit(&s_window_deadline, deadline, memory_order_release);
    return ESP_OK;
}

bool voice_credentials_provisioning_window_is_open(void)
{
    uint32_t deadline = atomic_load_explicit(
        &s_window_deadline, memory_order_acquire);
    bool open = deadline_pending(now_ms(), deadline);
    if (!open && deadline != 0U)
        atomic_store_explicit(&s_window_deadline, 0U, memory_order_release);
    return open;
}

void voice_credentials_close_provisioning_window(void)
{
    atomic_store_explicit(&s_window_deadline, 0U, memory_order_release);
}

esp_err_t voice_credentials_get_snapshot(voice_credentials_snapshot_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!take_lock()) return ESP_ERR_INVALID_STATE;
    voice_credentials_snapshot_t value = {
        .stored = s_rt.stored,
        .has_api_key = s_rt.stored,
        .provisioning_window_open =
            voice_credentials_provisioning_window_is_open(),
        .provider = s_rt.provider,
        .revision = s_rt.revision,
        .last_error = s_rt.last_error,
    };
    memcpy(value.model, s_rt.model, sizeof(value.model));
    xSemaphoreGive(s_lock);
    *out = value;
    return ESP_OK;
}

static bool json_exact_u32(
    const cJSON *root, const char *name, uint32_t *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
        item->valuedouble < 0.0 ||
        item->valuedouble > (double)UINT32_MAX)
        return false;
    uint32_t value = (uint32_t)item->valuedouble;
    if ((double)value != item->valuedouble) return false;
    *out = value;
    return true;
}

static void erase_json_secret(cJSON *root)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "api_key");
    if (cJSON_IsString(item) && item->valuestring) {
        size_t length = strlen(item->valuestring);
        memset(item->valuestring, 0, length);
    }
}

esp_err_t voice_credentials_process_secure_frame(
    const char *frame, size_t length,
    voice_credentials_command_result_t *out)
{
    if (!frame || length == 0U || length > 1024U || !out)
        return ESP_ERR_INVALID_ARG;
    voice_credentials_command_result_t result = {
        .code = "BAD_CREDENTIAL_FRAME",
    };
    cJSON *root = cJSON_ParseWithLengthOpts(
        frame, length, NULL, false);
    if (!cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        *out = result;
        return ESP_OK;
    }
    uint32_t version = 0;
    const cJSON *command =
        cJSON_GetObjectItemCaseSensitive(root, "cmd");
    if (!json_exact_u32(root, "v", &version) || version != 1U ||
        !json_exact_u32(root, "seq", &result.sequence) ||
        result.sequence == 0U || !cJSON_IsString(command) ||
        !command->valuestring) {
        erase_json_secret(root);
        cJSON_Delete(root);
        *out = result;
        return ESP_OK;
    }
    if (!voice_credentials_provisioning_window_is_open()) {
        result.code = "PHYSICAL_AUTH_REQUIRED";
        erase_json_secret(root);
        cJSON_Delete(root);
        *out = result;
        return ESP_OK;
    }

    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (strcmp(command->valuestring, "configure_voice") == 0) {
        const cJSON *provider_item =
            cJSON_GetObjectItemCaseSensitive(root, "provider");
        const cJSON *key_item =
            cJSON_GetObjectItemCaseSensitive(root, "api_key");
        const cJSON *model_item =
            cJSON_GetObjectItemCaseSensitive(root, "model");
        voice_credentials_value_t value;
        memset(&value, 0, sizeof(value));
        value.provider = VOICE_PROVIDER_MIMO_DIRECT;
        if (!cJSON_IsString(provider_item) ||
            !provider_item->valuestring ||
            !cJSON_IsString(key_item) || !key_item->valuestring ||
            (model_item && (!cJSON_IsString(model_item) ||
                            !model_item->valuestring))) {
            result.code = "BAD_CREDENTIAL_PAYLOAD";
        } else if (strcmp(provider_item->valuestring, "mimo_direct") == 0) {
            value.provider = VOICE_PROVIDER_MIMO_DIRECT;
        } else if (strcmp(provider_item->valuestring, "ai_gateway") == 0) {
            value.provider = VOICE_PROVIDER_ESPRESSIF_AI_GATEWAY;
        } else {
            result.code = "BAD_PROVIDER";
        }
        if (strcmp(result.code, "BAD_CREDENTIAL_FRAME") == 0) {
            size_t key_length = strnlen(
                key_item->valuestring, sizeof(value.api_key));
            size_t model_length = model_item
                ? strnlen(model_item->valuestring, sizeof(value.model)) : 0U;
            if (key_length == 0U || key_length >= sizeof(value.api_key) ||
                model_length >= sizeof(value.model)) {
                result.code = "BAD_CREDENTIAL_LENGTH";
            } else {
                memcpy(value.api_key, key_item->valuestring, key_length + 1U);
                if (model_length > 0U)
                    memcpy(value.model, model_item->valuestring,
                           model_length + 1U);
                err = voice_credentials_save(&value);
                if (err == ESP_OK) {
                    result.ok = true;
                    result.restart_required = true;
                    result.code = "VOICE_CONFIG_SAVED";
                } else {
                    result.code = err == ESP_ERR_INVALID_ARG
                        ? "BAD_CREDENTIAL_PAYLOAD" : "CREDENTIAL_STORE_ERROR";
                }
            }
        }
        memset(&value, 0, sizeof(value));
    } else if (strcmp(command->valuestring, "erase_voice_credentials") == 0) {
        err = voice_credentials_erase();
        if (err == ESP_OK) {
            result.ok = true;
            result.restart_required = true;
            result.code = "VOICE_CONFIG_ERASED";
        } else {
            result.code = "CREDENTIAL_STORE_ERROR";
        }
    } else {
        result.code = "UNKNOWN_CREDENTIAL_COMMAND";
    }
    erase_json_secret(root);
    cJSON_Delete(root);
    *out = result;
    return ESP_OK;
}

#ifdef XIAOJING_TESTING
esp_err_t voice_credentials_test_reset(void)
{
    voice_credentials_open_provisioning_window(1000U);
    esp_err_t err = voice_credentials_erase();
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    return err;
}
#endif
