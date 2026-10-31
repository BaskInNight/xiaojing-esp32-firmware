#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "voice_provider.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VOICE_CREDENTIAL_API_KEY_CAPACITY 256U
#define VOICE_CREDENTIAL_MODEL_CAPACITY    64U
#define VOICE_CREDENTIAL_WINDOW_DEFAULT_MS 60000U

typedef struct {
    voice_provider_t provider;
    char api_key[VOICE_CREDENTIAL_API_KEY_CAPACITY];
    char model[VOICE_CREDENTIAL_MODEL_CAPACITY];
} voice_credentials_value_t;

typedef struct {
    bool stored;
    bool has_api_key;
    bool provisioning_window_open;
    voice_provider_t provider;
    char model[VOICE_CREDENTIAL_MODEL_CAPACITY];
    uint32_t revision;
    esp_err_t last_error;
} voice_credentials_snapshot_t;

typedef struct {
    uint32_t sequence;
    bool ok;
    bool restart_required;
    const char *code;
} voice_credentials_command_result_t;

esp_err_t voice_credentials_global_init(void);
bool voice_credentials_value_valid(const voice_credentials_value_t *value);
esp_err_t voice_credentials_load(voice_credentials_value_t *out);
esp_err_t voice_credentials_save(const voice_credentials_value_t *value);
esp_err_t voice_credentials_erase(void);

/* Physical-presence gate for secret writes. The key itself is never exposed
 * through snapshots or logs. */
esp_err_t voice_credentials_open_provisioning_window(uint32_t duration_ms);
bool voice_credentials_provisioning_window_is_open(void);
void voice_credentials_close_provisioning_window(void);
esp_err_t voice_credentials_get_snapshot(voice_credentials_snapshot_t *out);
esp_err_t voice_credentials_process_secure_frame(
    const char *frame, size_t length,
    voice_credentials_command_result_t *out);

#ifdef XIAOJING_TESTING
esp_err_t voice_credentials_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif
