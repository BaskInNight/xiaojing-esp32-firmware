/*
 * app_bootstrap.c — 安全初始化顺序 (Round 2.1)
 *
 * Four-phase shutdown: button→quiesce→services→executor
 * Button service wired to executor via HAL (not MCP)
 * FAKE mode: button_service enabled via fake HAL
 */

#include <stdio.h>
#include <inttypes.h>
#include <stdatomic.h>
#include "app_bootstrap.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "machine_config.h"
#include "machine_status_store.h"
#include "fake_hal.h"
#include "real_hal.h"
#include "board_config.h"
#include "position_service.h"
#include "safety_manager.h"
#include "bl50_service.h"
#include "water_service.h"
#include "dry_service.h"
#include "drain_service.h"
#include "detergent_service.h"
#include "uv_service.h"
#include "button_service.h"
#include "wash_executor.h"
#include "wash_executor_bootstrap.h"
#include "app_protocol.h"
#include "ble_transport.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

static const char *TAG = "bootstrap";

static fake_hal_ctx_t *s_fake_hal = NULL;
static real_hal_ctx_t *s_real_hal = NULL;
static const xiaojing_hal_t *s_hal = NULL;
static bool s_protocol_active = false;
static bool s_ble_active = false;

/* ---- Event queue (shared: services publish, adapter consumer reads) ---- */

static QueueHandle_t s_event_queue = NULL;

static esp_err_t bootstrap_event_publish(const machine_event_t *event,
                                          uint32_t timeout_ms, void *context) {
    (void)context;
    if (!s_event_queue) return ESP_ERR_INVALID_STATE;
    TickType_t ticks = (timeout_ms == 0) ? 0 : pdMS_TO_TICKS(timeout_ms);
    return (xQueueSend(s_event_queue, event, ticks) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

static machine_event_sink_t make_event_sink(void) {
    return (machine_event_sink_t){ .publish = bootstrap_event_publish, .context = NULL };
}

static esp_err_t protocol_tx_critical(const char *json, size_t length,
                                      uint32_t timeout_ms, void *context) {
    (void)context;
    return xiaojing_ble_transport_send_critical(json, length, timeout_ms);
}

static esp_err_t protocol_tx_status(const char *json, size_t length,
                                    uint32_t timeout_ms, void *context) {
    (void)timeout_ms; (void)context;
    return xiaojing_ble_transport_publish_status(json, length);
}

static esp_err_t ble_frame_sink(const char *frame, size_t length, void *context) {
    (void)context;
    return app_protocol_process_frame(frame, length);
}

static void ble_connection_sink(bool connected, void *context) {
    (void)context;
    app_protocol_set_connected(connected);
}

void *app_bootstrap_get_event_queue(void) {
    return (void *)s_event_queue;
}

/* ---- Button → Executor sink callbacks ---- */

static esp_err_t button_normal_sink(const button_sink_event_t *event,
                                     uint32_t timeout_ms, void *context) {
    (void)timeout_ms; (void)context;
    wash_executor_t *exec = wash_exec_bootstrap_get_executor();
    if (!exec) return ESP_ERR_INVALID_STATE;
    if (event->event_type == BUTTON_EVENT_CLICK) {
        return wash_executor_submit_urgent(exec, false);
    }
    return ESP_OK;
}

static esp_err_t button_urgent_sink(const button_sink_event_t *event,
                                      uint32_t timeout_ms, void *context) {
    (void)timeout_ms; (void)context;
    wash_executor_t *exec = wash_exec_bootstrap_get_executor();
    if (!exec) return ESP_ERR_INVALID_STATE;
    if (event->event_type == BUTTON_EVENT_CLICK) {
        return wash_executor_submit_urgent(exec, true);
    }
    return ESP_OK;
}

/* ---- NVS ---- */

static esp_err_t init_nvs(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
    }
    return err;
}

static void destroy_hal(void) {
    if (s_fake_hal) { fake_hal_destroy(s_fake_hal); s_fake_hal = NULL; s_hal = NULL; }
    if (s_real_hal) { real_hal_destroy(s_real_hal); s_real_hal = NULL; s_hal = NULL; }
}

/* ---- Rollback state (init vs started) ---- */

typedef struct {
    bool nvs, config, status_store, event_queue;
    bool hal;
    bool global_position, global_water, global_dry, global_drain;
    bool global_detergent, global_uv, global_button;
    bool safety_init, safety_started;
    bool position_init, position_started;
    bool bl50_init, bl50_started;
    bool water_init, water_started;
    bool dry_init, dry_started;
    bool drain_init, drain_started;
    bool detergent_init, detergent_started;
    bool uv_init, uv_started;
    bool button_init, button_started;
    bool adapter_consumer_started;
    bool executor_started;
    bool protocol_initialized;
    bool protocol_started;
    bool ble_initialized;
    bool ble_started;
} init_state_t;

/* ================================================================
 * Init — with goto rollback
 * Order: NVS→Config→Store→Global→EventQ→HAL→Safety→Position→BL50→
 *        Water→Dry→Drain→Detergent→UV→Button→Executor
 * ================================================================ */

esp_err_t app_bootstrap_init(app_bootstrap_result_t *result) {
    if (!result) return ESP_ERR_INVALID_ARG;
    memset(result, 0, sizeof(*result));

    init_state_t st = {0};
    esp_err_t err;

    err = init_nvs();
    if (err != ESP_OK) goto fail;
    st.nvs = true; result->nvs_ok = true;

    err = machine_config_init();
    if (err != ESP_OK || !machine_config_validate(machine_config_get())) { err = ESP_ERR_INVALID_STATE; goto fail; }
    st.config = true; result->config_ok = true;

    err = machine_status_store_init();
    if (err != ESP_OK) goto fail;
    st.status_store = true; result->status_store_ok = true;

    err = position_service_global_init();
    if (err != ESP_OK) goto fail;
    st.global_position = true;
    err = water_service_global_init();
    if (err != ESP_OK) goto fail;
    st.global_water = true;
    err = dry_service_global_init();
    if (err != ESP_OK) goto fail;
    st.global_dry = true;
    err = drain_service_global_init();
    if (err != ESP_OK) goto fail;
    st.global_drain = true;
    err = detergent_service_global_init();
    if (err != ESP_OK) goto fail;
    st.global_detergent = true;
    err = uv_service_global_init();
    if (err != ESP_OK) goto fail;
    st.global_uv = true;
    err = button_service_global_init();
    if (err != ESP_OK) goto fail;
    st.global_button = true;

    s_event_queue = xQueueCreate(APP_BOOTSTRAP_EVENT_QUEUE_DEPTH, sizeof(machine_event_t));
    if (!s_event_queue) { err = ESP_ERR_NO_MEM; goto fail; }
    st.event_queue = true;

    /* HAL */
    if (board_config_get_output_mode() == XIAOJING_MODE_FAKE) {
        s_fake_hal = fake_hal_create();
        if (!s_fake_hal) { err = ESP_ERR_NO_MEM; goto fail; }
        s_hal = fake_hal_get_interface(s_fake_hal);
    } else {
        s_real_hal = real_hal_create();
        if (!s_real_hal) { err = ESP_ERR_NOT_SUPPORTED; goto fail; }
        err = real_hal_set_ibt2_calibration(s_real_hal,
            machine_config_get()->position.direction_calibrated,
            machine_config_get()->position.rpwm_is_cw);
        if (err != ESP_OK) { destroy_hal(); goto fail; }
        s_hal = real_hal_get_interface(s_real_hal);
    }
    result->hal = s_hal;
    st.hal = true;

    /* Safety manager */
    {
        safety_manager_config_t sm_cfg = {
            .hal = s_hal, .config = machine_config_get(),
            .output_mode = board_config_get_output_mode(),
            .ptc_enabled = board_config_is_ptc_enabled(),
            .board_identity_confirmed = board_config_is_board_identity_confirmed(),
        };
        err = safety_manager_init(&sm_cfg);
        if (err != ESP_OK) goto fail;
        st.safety_init = true;
        err = safety_manager_start();
        if (err != ESP_OK) goto fail;
        st.safety_started = true; result->safety_mgr_ok = true;
    }

    /* Position service */
    {
        const position_params_t *pp = &machine_config_get()->position;
        position_service_config_t ps_cfg = {
            .default_policy = pp->default_policy, .move_pwm_percent = pp->move_pwm_percent,
            .approach_pwm_percent = pp->approach_pwm_percent, .debounce_ms = pp->debounce_ms,
            .move_timeout_ms = pp->move_timeout_ms, .brake_ms = pp->brake_ms,
            .rpwm_is_cw = pp->rpwm_is_cw, .direction_calibrated = pp->direction_calibrated,
        };
        err = position_service_init(&ps_cfg, s_hal, make_event_sink());
        if (err != ESP_OK) goto fail;
        st.position_init = true;
        err = position_service_start();
        if (err != ESP_OK) goto fail;
        st.position_started = true; result->position_svc_ok = true;
    }

    /* BL50 service */
    {
        bl50_service_config_t b50_cfg = {
            .hal = s_hal, .config = machine_config_get(),
            .event_sink = make_event_sink(),
        };
        err = bl50_service_init(&b50_cfg);
        if (err != ESP_OK) goto fail;
        st.bl50_init = true;
        err = bl50_service_start();
        if (err != ESP_OK) goto fail;
        st.bl50_started = true; result->bl50_svc_ok = true;
    }

    /* Water service */
    {
        const water_params_t *wp = &machine_config_get()->water;
        water_service_config_t ws_cfg = {
            .pulses_per_liter = wp->pulses_per_liter, .no_flow_timeout_ms = wp->no_flow_timeout_ms,
            .low_flow_window_ms = wp->low_flow_window_ms, .low_flow_min_pulses = 0,
            .source_batch_target_pulses = wp->source_batch_target_pulses,
            .source_batch_max_ms = wp->source_batch_max_ms, .source_settle_ms = wp->source_settle_ms,
            .transfer_timeout_ms = wp->transfer_timeout_ms, .default_transfer_ms = wp->default_transfer_ms,
            .max_fill_cycles = wp->max_fill_cycles, .total_inlet_timeout_ms = wp->total_inlet_timeout_ms,
        };
        err = water_service_init(&ws_cfg, s_hal, make_event_sink());
        if (err != ESP_OK) goto fail;
        st.water_init = true;
        err = water_service_start();
        if (err != ESP_OK) goto fail;
        st.water_started = true; result->water_svc_ok = true;
    }

    /* Dry service */
    {
        const dry_params_t *dp = &machine_config_get()->dry;
        err = dry_service_init(dp, s_hal, make_event_sink());
        if (err != ESP_OK) goto fail;
        st.dry_init = true;
        err = dry_service_start();
        if (err != ESP_OK) goto fail;
        st.dry_started = true; result->dry_svc_ok = true;
    }

    /* Drain service */
    {
        const drain_params_t *drp = &machine_config_get()->drain;
        err = drain_service_init(drp, s_hal, make_event_sink());
        if (err != ESP_OK) goto fail;
        st.drain_init = true;
        err = drain_service_start();
        if (err != ESP_OK) goto fail;
        st.drain_started = true; result->drain_svc_ok = true;
    }

    /* Detergent service */
    {
        const detergent_params_t *dtp = &machine_config_get()->detergent;
        err = detergent_service_init(dtp, s_hal, make_event_sink());
        if (err != ESP_OK) goto fail;
        st.detergent_init = true;
        err = detergent_service_start();
        if (err != ESP_OK) goto fail;
        st.detergent_started = true; result->detergent_svc_ok = true;
    }

    /* UV service */
    {
        const uv_params_t *uvp = &machine_config_get()->uv;
        err = uv_service_init(uvp, s_hal, make_event_sink());
        if (err != ESP_OK) goto fail;
        st.uv_init = true;
        err = uv_service_start();
        if (err != ESP_OK) goto fail;
        st.uv_started = true; result->uv_svc_ok = true;
    }

    /* Button service — uses HAL (not MCP), works in both FAKE and REAL mode.
     * Button is the local cancel/urgent entry point — init/start failure is fatal. */
    /* Executor must exist before any local or remote producer starts. */
    {
        err = wash_exec_bootstrap_create((void *)s_event_queue);
        if (err != ESP_OK) goto fail;
        result->executor = wash_exec_bootstrap_get_executor();
        result->adapter_ctx = wash_exec_bootstrap_get_adapter_ctx();
        st.adapter_consumer_started = true;
        st.executor_started = true; result->executor_ok = true;
    }

    {
        button_service_config_t btn_cfg = {
            .debounce_ms = 30,
            .poll_interval_ms = 200,
            .hal = s_hal,
            .enable_internal_pullup = true,
            .normal_sink = { .publish = button_normal_sink, .context = NULL },
            .urgent_sink = { .publish = button_urgent_sink, .context = NULL },
        };
        err = button_service_init(&btn_cfg);
        if (err != ESP_OK) goto fail;
        st.button_init = true;
        err = button_service_start();
        if (err != ESP_OK) goto fail;
        st.button_started = true; result->button_svc_ok = true;
    }

    /* Protocol owns planning/routing; BLE owns framing and GATT only. */
    {
        app_protocol_config_t protocol_cfg = {
            .executor = result->executor,
            .machine_config = machine_config_get(),
            .legacy_enabled = board_config_is_legacy_ble_enabled(),
            .critical_tx = protocol_tx_critical,
            .status_tx = protocol_tx_status,
            .tx_context = NULL,
        };
        err = app_protocol_init(&protocol_cfg);
        if (err != ESP_OK) goto fail;
        st.protocol_initialized = true;
        err = app_protocol_start();
        if (err != ESP_OK) goto fail;
        st.protocol_started = true;
        s_protocol_active = true;
        result->app_protocol_ok = true;
    }

    if (xiaojing_ble_transport_is_supported()) {
        ble_transport_config_t ble_cfg = {
            .device_name = BLE_TRANSPORT_DEVICE_NAME_DEFAULT,
            .frame_sink = ble_frame_sink,
            .connection_sink = ble_connection_sink,
            .context = NULL,
        };
        err = xiaojing_ble_transport_init(&ble_cfg);
        if (err != ESP_OK) goto fail;
        st.ble_initialized = true;
        err = xiaojing_ble_transport_start();
        if (err != ESP_OK) goto fail;
        st.ble_started = true;
        s_ble_active = true;
        result->ble_transport_ok = true;
    }

    return ESP_OK;

fail:
    ESP_LOGE(TAG, "Bootstrap failed (0x%x), rolling back...", err);
    /*
     * Four-phase rollback (reverse of init):
     *   Phase 1: button stop (producer)
     *   Phase 2: executor quiesce (reject new, keep consuming)
     *   Phase 3: services stop UV→Detergent→Drain→Dry→Water→BL50→Position
     *   Phase 4: executor stop/join → adapter consumer → safety → queue → HAL
     */
    bool rollback_halted = false;

    /* Phase 0: stop remote producer before protocol/executor dependencies. */
    if (st.ble_initialized) {
        esp_err_t s = xiaojing_ble_transport_stop();
        if (s != ESP_OK) {
            ESP_LOGE(TAG, "Rollback: BLE stop failed (0x%x)", s);
            rollback_halted = true;
        } else {
            s_ble_active = false;
        }
    }
    if (!rollback_halted && st.protocol_initialized) {
        esp_err_t s = app_protocol_stop();
        if (s != ESP_OK) {
            ESP_LOGE(TAG, "Rollback: protocol stop failed (0x%x)", s);
            rollback_halted = true;
        } else {
            s_protocol_active = false;
        }
    }

    /* Phase 1: Stop producer */
    if (!rollback_halted && st.button_started) {
        esp_err_t s = button_service_stop();
        if (s != ESP_OK) {
            ESP_LOGE(TAG, "Rollback: button stop failed (0x%x)", s);
            rollback_halted = true;
        }
    }

    /* Phase 2: Quiesce executor */
    if (st.executor_started) {
        wash_exec_bootstrap_quiesce();
    }

    /* Phase 3: Stop services (executor still consuming terminals) */
    if (!rollback_halted && st.uv_init) {
        esp_err_t s = uv_service_stop();
        if (s != ESP_OK) { ESP_LOGE(TAG, "Rollback: uv stop failed (0x%x)", s); rollback_halted = true; }
    }
    if (!rollback_halted && st.detergent_init) {
        esp_err_t s = detergent_service_stop();
        if (s != ESP_OK) { ESP_LOGE(TAG, "Rollback: detergent stop failed (0x%x)", s); rollback_halted = true; }
    }
    if (!rollback_halted && st.drain_init) {
        esp_err_t s = drain_service_stop();
        if (s != ESP_OK) { ESP_LOGE(TAG, "Rollback: drain stop failed (0x%x)", s); rollback_halted = true; }
    }
    if (!rollback_halted && st.dry_init) {
        esp_err_t s = dry_service_stop();
        if (s != ESP_OK) { ESP_LOGE(TAG, "Rollback: dry stop failed (0x%x)", s); rollback_halted = true; }
    }
    if (!rollback_halted && st.water_init) {
        esp_err_t s = water_service_stop();
        if (s != ESP_OK) { ESP_LOGE(TAG, "Rollback: water stop failed (0x%x)", s); rollback_halted = true; }
    }
    if (!rollback_halted && st.bl50_init) {
        esp_err_t s = bl50_service_stop();
        if (s != ESP_OK) { ESP_LOGE(TAG, "Rollback: bl50 stop failed (0x%x)", s); rollback_halted = true; }
    }
    if (!rollback_halted && st.position_init) {
        esp_err_t s = position_service_stop();
        if (s != ESP_OK) { ESP_LOGE(TAG, "Rollback: position stop failed (0x%x)", s); rollback_halted = true; }
    }

    /* Phase 4: Executor stop/join → adapter → safety → queue → HAL */
    if (!rollback_halted && st.executor_started) {
        esp_err_t es = wash_exec_bootstrap_stop(5000);
        if (es != ESP_OK) {
            ESP_LOGE(TAG, "Rollback: executor stop failed (0x%x)", es);
            rollback_halted = true;
        }
    }
    if (!rollback_halted) {
        esp_err_t bs = wash_exec_bootstrap_destroy();
        if (bs != ESP_OK) {
            ESP_LOGE(TAG, "Rollback: executor/adapter destroy failed (0x%x)", bs);
            rollback_halted = true;
        }
    }
    if (!rollback_halted && st.safety_init) {
        esp_err_t s = safety_manager_stop();
        if (s != ESP_OK) {
            ESP_LOGE(TAG, "Rollback: safety stop failed (0x%x)", s);
            rollback_halted = true;
        }
    }
    if (!rollback_halted) {
        if (st.event_queue) {
            machine_event_t drain;
            while (xQueueReceive(s_event_queue, &drain, 0) == pdTRUE) {}
            vQueueDelete(s_event_queue); s_event_queue = NULL;
        }
        if (st.hal) destroy_hal();
    }
    if (rollback_halted) {
        ESP_LOGE(TAG, "Rollback halted: resources preserved for later deinit");
    }
    result->hal = NULL;
    result->executor = NULL;
    result->adapter_ctx = NULL;
    return err;
}

/* ================================================================
 * Deinit — four-phase shutdown
 *
 * Phase 1: button stop (producer stops)
 * Phase 2: executor quiesce (reject new programs)
 * Phase 3: services stop (executor consumes terminals)
 * Phase 4: executor stop/join → adapter → safety → queue → HAL
 * ================================================================ */

esp_err_t app_bootstrap_deinit(void) {
    /* Phase 0: stop BLE producer, then protocol routing. */
    if (s_ble_active) {
        esp_err_t ble_err = xiaojing_ble_transport_stop();
        if (ble_err != ESP_OK) {
            ESP_LOGE(TAG, "deinit: BLE stop failed (0x%x), preserving resources",
                     ble_err);
            return ble_err;
        }
        s_ble_active = false;
    }
    if (s_protocol_active) {
        esp_err_t protocol_err = app_protocol_stop();
        if (protocol_err != ESP_OK) {
            ESP_LOGE(TAG, "deinit: protocol stop failed (0x%x), preserving resources",
                     protocol_err);
            return protocol_err;
        }
        s_protocol_active = false;
    }

    /* Phase 1: Stop button producer */
    esp_err_t btn_err = button_service_stop();
    if (btn_err != ESP_OK) {
        ESP_LOGE(TAG, "deinit: button stop failed (0x%x), preserving resources", btn_err);
        return btn_err;
    }

    /* Phase 2: Quiesce executor (reject new programs, keep consuming) */
    wash_exec_bootstrap_quiesce();

    /* Phase 3: Stop services (executor still consuming service terminals) */
    esp_err_t uv_err = uv_service_stop();
    if (uv_err != ESP_OK) {
        ESP_LOGE(TAG, "deinit: uv stop failed (0x%x), preserving resources", uv_err);
        return uv_err;
    }

    esp_err_t dt_err = detergent_service_stop();
    if (dt_err != ESP_OK) {
        ESP_LOGE(TAG, "deinit: detergent stop failed (0x%x), preserving resources", dt_err);
        return dt_err;
    }

    esp_err_t dr_err = drain_service_stop();
    if (dr_err != ESP_OK) {
        ESP_LOGE(TAG, "deinit: drain stop failed (0x%x), preserving resources", dr_err);
        return dr_err;
    }

    esp_err_t ds_err = dry_service_stop();
    if (ds_err != ESP_OK) {
        ESP_LOGE(TAG, "deinit: dry stop failed (0x%x), preserving resources", ds_err);
        return ds_err;
    }

    esp_err_t ws_err = water_service_stop();
    if (ws_err != ESP_OK) {
        ESP_LOGE(TAG, "deinit: water stop failed (0x%x), preserving resources", ws_err);
        return ws_err;
    }

    esp_err_t bs_err = bl50_service_stop();
    if (bs_err != ESP_OK) {
        ESP_LOGE(TAG, "deinit: bl50 stop failed (0x%x), preserving resources", bs_err);
        return bs_err;
    }

    esp_err_t ps_err = position_service_stop();
    if (ps_err != ESP_OK) {
        ESP_LOGE(TAG, "deinit: position stop failed (0x%x), preserving resources", ps_err);
        return ps_err;
    }

    /* Phase 4: Executor stop/join → adapter → safety → queue → HAL */
    esp_err_t es = wash_exec_bootstrap_stop(5000);
    if (es != ESP_OK) {
        ESP_LOGE(TAG, "deinit: executor stop failed (0x%x), preserving resources", es);
        return es;
    }
    esp_err_t destroy_err = wash_exec_bootstrap_destroy();
    if (destroy_err != ESP_OK) {
        ESP_LOGE(TAG, "deinit: executor/adapter destroy failed (0x%x), preserving resources",
                 destroy_err);
        return destroy_err;
    }

    esp_err_t safety_err = safety_manager_stop();
    if (safety_err != ESP_OK) {
        ESP_LOGE(TAG, "deinit: safety stop failed (0x%x), preserving queue/HAL",
                 safety_err);
        return safety_err;
    }

    if (s_event_queue) {
        machine_event_t drain;
        while (xQueueReceive(s_event_queue, &drain, 0) == pdTRUE) {}
        vQueueDelete(s_event_queue); s_event_queue = NULL;
    }
    destroy_hal();

    return ESP_OK;
}
