/*
 * group_d_test_support.h — Round 1.6 global test support
 *
 * Non-static module for MCP owner tracking, global cleanup, isolation poison.
 * Visible to both test_runner.c (tearDown) and group_d_tests.c (per-test).
 * No TEST_ASSERT in cleanup paths.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "mcp23017_driver.h"
#include "button_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create a mock MCP and register as global owner.
 * Returns NULL on failure. Asserts no previous owner exists. */
mcp23017_handle_t *group_d_test_create_mock_mcp(void);

/* Destroy a mock MCP, verifying it is the registered owner.
 * Returns ESP_OK, ESP_ERR_INVALID_ARG (NULL), or ESP_ERR_INVALID_STATE (not owner). */
esp_err_t group_d_test_destroy_mock_mcp(mcp23017_handle_t *handle);

/* Get current MCP alloc balance (alloc_count - free_count). */
int32_t group_d_test_mcp_alloc_balance(void);

/* Global cleanup for Group D: hooks, button_service, MCP owner.
 * Returns first error. Sets poison on failure. */
esp_err_t group_d_test_global_cleanup(void);

/* Isolation poison state. */
bool group_d_test_is_poisoned(void);
void group_d_test_set_poisoned(bool poisoned);

/* Test sink event history for D33 etc.
 * Fixed capacity ring buffer, thread-safe via mutex. */
#define GROUP_D_EVENT_HISTORY_CAP 8

typedef struct {
    button_sink_event_t items[GROUP_D_EVENT_HISTORY_CAP];
    int count;
} group_d_event_history_t;

#ifdef __cplusplus
}
#endif
