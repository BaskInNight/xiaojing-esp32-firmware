/*
 * group_d_test_support.c — Round 1.7 global test support implementation
 *
 * R1.7: atomic_bool isolation poison (not volatile).
 *       global_cleanup preserves MCP owner on service failure.
 * Non-static: MCP owner lifecycle, allocation tracking, global cleanup.
 * Called from test_runner.c tearDown and group_d_tests.c per-test.
 */

#include "group_d_test_support.h"
#include "button_service.h"
#include "esp_log.h"
#include <stdatomic.h>

static const char *TAG = "g_d_support";

/* MCP owner tracking — non-static, visible across TUs */
static mcp23017_handle_t *s_group_d_mcp_owner = NULL;
static _Atomic int32_t s_mcp_alloc_count = 0;
static _Atomic int32_t s_mcp_free_count = 0;
/* R1.7: atomic_bool — volatile does not provide cross-task synchronization */
static atomic_bool s_isolation_poisoned = false;

mcp23017_handle_t *group_d_test_create_mock_mcp(void)
{
    if (s_group_d_mcp_owner != NULL) {
        ESP_LOGE(TAG, "MCP owner leak: previous handle not destroyed");
        return NULL;
    }
    mcp23017_handle_t *h = mcp23017_test_create_mock(true);
    if (!h) return NULL;
    s_group_d_mcp_owner = h;
    atomic_fetch_add(&s_mcp_alloc_count, 1);
    return h;
}

esp_err_t group_d_test_destroy_mock_mcp(mcp23017_handle_t *handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (handle != s_group_d_mcp_owner) {
        ESP_LOGE(TAG, "MCP owner mismatch: destroying non-owner handle");
        return ESP_ERR_INVALID_STATE;
    }
    mcp23017_destroy(handle);
    s_group_d_mcp_owner = NULL;
    atomic_fetch_add(&s_mcp_free_count, 1);
    return ESP_OK;
}

int32_t group_d_test_mcp_alloc_balance(void)
{
    return atomic_load(&s_mcp_alloc_count) - atomic_load(&s_mcp_free_count);
}

bool group_d_test_is_poisoned(void)
{
    return atomic_load(&s_isolation_poisoned);
}

void group_d_test_set_poisoned(bool poisoned)
{
    atomic_store(&s_isolation_poisoned, poisoned);
}

esp_err_t group_d_test_global_cleanup(void)
{
    esp_err_t first_err = ESP_OK;

    /* 1. Stop/abort button_service (hooks, lifecycle) */
    esp_err_t svc_err = button_service_test_global_cleanup();
    if (svc_err != ESP_OK) {
        ESP_LOGW(TAG, "button_service cleanup failed: 0x%x", svc_err);
        if (first_err == ESP_OK) first_err = svc_err;
        /* R1.7: Service cleanup failed — task may still own MCP.
         * Do NOT destroy MCP owner. Set poison. Return error. */
        atomic_store(&s_isolation_poisoned, true);
        return first_err;
    }

    /* 2. Destroy MCP owner if it exists — only after service confirmed stopped */
    if (s_group_d_mcp_owner) {
        esp_err_t mcp_err = group_d_test_destroy_mock_mcp(s_group_d_mcp_owner);
        if (mcp_err != ESP_OK && first_err == ESP_OK) first_err = mcp_err;
    }

    /* 3. Set poison on failure, clear on success */
    if (first_err != ESP_OK) {
        atomic_store(&s_isolation_poisoned, true);
        ESP_LOGE(TAG, "global cleanup failed: 0x%x — isolation poisoned", first_err);
    } else {
        atomic_store(&s_isolation_poisoned, false);
    }

    return first_err;
}
