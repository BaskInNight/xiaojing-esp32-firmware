/*
 * uv_self_test.c — UV 灯安全自检运行时组件
 *
 * 调用链（受 12 项安全门禁保护）：
 *   BLE uv_self_test → uv_self_test_request()
 *     → 采集 executor/uv/safety/position 快照 → uv_self_test_gate_check()
 *     → uv_service_run_async(request_id, 3000) → uv_fsm → safety_manager_apply
 *     → HAL/MCP23017 → UV 灯
 *
 * 自检终态通过 uv_service_get_snapshot() 轮询同步，不改动 uv_service
 * 的事件 sink（其仍归正式洗涤路径）。
 */

#include "uv_self_test.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "safety_manager.h"
#include "uv_service.h"
#include "wash_executor.h"

/* uv_snapshot_t.output_state 取值（与 uv_service/uv_fsm.h 的
 * uv_output_state_t 一致：UNKNOWN=0 / KNOWN_OFF=1 / KNOWN_ON=2） */
#define UV_ST_OUTPUT_UNKNOWN   0
#define UV_ST_OUTPUT_KNOWN_OFF 1
#define UV_ST_OUTPUT_KNOWN_ON  2

/* uv_snapshot_t.state 现在携带公开的 uv_state_t（uv_service 通过显式
 * uv_fsm_state_to_public() 映射）。本组件一律使用公开枚举，不依赖任何
 * 内部 fsm 数值。 */
#define UV_ST_PUBLIC_IDLE       UV_STATE_IDLE
#define UV_ST_PUBLIC_VALIDATING UV_STATE_VALIDATING
#define UV_ST_PUBLIC_RUNNING    UV_STATE_RUNNING
#define UV_ST_PUBLIC_STOPPING   UV_STATE_STOPPING
#define UV_ST_PUBLIC_COMPLETE   UV_STATE_COMPLETE
#define UV_ST_PUBLIC_CANCELING  UV_STATE_CANCELING
#define UV_ST_PUBLIC_SKIPPING   UV_STATE_SKIPPING
#define UV_ST_PUBLIC_FAULT      UV_STATE_FAULT

static const char *TAG = "uv_self_test";

typedef struct {
    uv_self_test_config_t cfg;
    bool initialized;
    bool started;
    machine_request_id_t next_request_id;
    machine_request_id_t request_id;   /* 最近一次受理的自检 request_id */
    uv_self_test_state_t state;
    uint32_t requested_duration_ms;
    uint32_t elapsed_ms;
    uint32_t submit_tick_ms;      /* 受理时间（自检提交服务后的宽限窗口） */
    uv_terminal_t terminal;
    const char *last_code;
    bool lamp_on;
} uv_self_test_rt_t;

static uv_self_test_rt_t s_rt;
static SemaphoreHandle_t s_mtx;

static bool lock(void)
{
    return s_mtx && xSemaphoreTake(s_mtx, pdMS_TO_TICKS(500)) == pdTRUE;
}

static void collect_gate_input(uv_self_test_gate_input_t *in)
{
    memset(in, 0, sizeof(*in));
    in->pos = DRUM_POS_UNKNOWN;

    if (s_rt.cfg.executor) {
        wash_exec_snapshot_t snap;
        if (wash_executor_get_snapshot(s_rt.cfg.executor, &snap) == ESP_OK) {
            in->exec_idle = snap.state == WASH_EXEC_STATE_IDLE &&
                            !snap.program_active && !snap.terminal_pending;
        }
    }

    uv_snapshot_t uv;
    if (uv_service_get_snapshot(&uv) == ESP_OK) {
        in->uv_idle = uv.state == UV_ST_PUBLIC_IDLE;
        in->uv_confirmed_off = uv.output_state == UV_ST_OUTPUT_KNOWN_OFF;
    }

    safety_snapshot_t safety;
    if (safety_manager_get_snapshot(&safety) == ESP_OK) {
        in->fault_active = safety.fault_active;
        in->emergency_active = safety.emergency_stop_active;
        in->mcp_uv_known =
            (safety.mcp_known_mask & (1U << SAFE_OUTPUT_UV)) != 0U;
    }

    safety_position_view_t view;
    if (safety_manager_get_position_view(&view) == ESP_OK) {
        /* pos_valid 只表示样本是否存在；新鲜度单独用 pos_fresh 表达。
         * 二者必须分开，否则 sample_valid=true 且 fresh=false 会永远被
         * 折叠成 POSITION_UNKNOWN，POSITION_STALE 门禁不可达。 */
        in->pos_valid = view.sample_valid;
        in->pos_fresh = view.fresh;
        in->pos_stable = view.stable;
        in->pos_moving = view.motor_moving;
        in->pos = view.position;
    }
}

/* 将 uv_service 的 terminal 显式映射到自检终态（纯函数见
 * uv_self_test_core.c）。CANCELED/SKIPPED/TIMEOUT 绝不折叠成 COMPLETE。 */
#define map_uv_terminal uv_self_test_map_terminal

esp_err_t uv_self_test_init(const uv_self_test_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    if (!s_mtx) {
        s_mtx = xSemaphoreCreateMutex();
        if (!s_mtx) return ESP_ERR_NO_MEM;
    }
    if (!lock()) return ESP_ERR_TIMEOUT;
    if (s_rt.initialized) {
        xSemaphoreGive(s_mtx);
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_rt, 0, sizeof(s_rt));
    s_rt.cfg = *config;
    s_rt.next_request_id = 1;
    s_rt.state = UV_STATE_SELFTEST_INACTIVE;
    s_rt.last_code = "UV_SELF_TEST_ACCEPTED";
    s_rt.initialized = true;
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}

esp_err_t uv_self_test_start(void)
{
    if (!lock()) return ESP_ERR_INVALID_STATE;
    if (!s_rt.initialized || s_rt.started) {
        xSemaphoreGive(s_mtx);
        return ESP_ERR_INVALID_STATE;
    }
    s_rt.started = true;
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}

#define UV_SELFTEST_STOP_OFF_TIMEOUT_MS 2500U
/* 受理后服务尚未受理的宽限窗口（uv_task 每 50ms 处理 CMD_RUN）。 */
#define UV_SUBMIT_GRACE_MS 2000U

/* 仅通过 uv_service 快照确认 UV 灯输出已知关闭（KNOWN_OFF）。
 * 禁止把"已发送关灯请求"当作"已关闭"。 */
static bool uv_output_confirmed_off(void)
{
    uv_snapshot_t uv;
    if (uv_service_get_snapshot(&uv) != ESP_OK) return false;
    return uv.output_state == UV_ST_OUTPUT_KNOWN_OFF;
}

esp_err_t uv_self_test_stop(void)
{
    if (!s_mtx) return ESP_ERR_INVALID_STATE;
    if (!lock()) return ESP_ERR_INVALID_STATE;
    if (!s_rt.initialized) {
        xSemaphoreGive(s_mtx);
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(s_mtx);

    /* stop 前必须确保 UV 已关闭并从快照确认。只有在 output_state 已
     * 确认 KNOWN_OFF 时才允许销毁生命周期资源。 */
    if (!uv_output_confirmed_off()) {
        (void)uv_service_emergency_stop();
        TickType_t deadline =
            xTaskGetTickCount() + pdMS_TO_TICKS(UV_SELFTEST_STOP_OFF_TIMEOUT_MS);
        bool confirmed = false;
        while (xTaskGetTickCount() < deadline) {
            if (uv_output_confirmed_off()) { confirmed = true; break; }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (!confirmed) {
            ESP_LOGE(TAG,
                     "stop: UV OFF not confirmed within %u ms; "
                     "preserving lifecycle for retry",
                     UV_SELFTEST_STOP_OFF_TIMEOUT_MS);
            return ESP_ERR_TIMEOUT;
        }
    }

    if (!lock()) return ESP_ERR_INVALID_STATE;
    s_rt.started = false;
    s_rt.initialized = false;
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}

esp_err_t uv_self_test_request(const char **out_code, uint32_t *out_duration_ms)
{
    if (out_code) *out_code = NULL;
    if (out_duration_ms) *out_duration_ms = 0;
    if (!lock()) return ESP_ERR_INVALID_STATE;
    if (!s_rt.initialized || !s_rt.started) {
        xSemaphoreGive(s_mtx);
        return ESP_ERR_INVALID_STATE;
    }

    uv_self_test_gate_input_t in;
    collect_gate_input(&in);
    uv_self_test_gate_result_t gate = uv_self_test_gate_check(&in);

    if (gate != UV_ST_GATE_OK) {
        /* 自检运行中再提交 → UV_BUSY（不得延长点亮时间） */
        const char *code = uv_self_test_gate_code(gate);
        ESP_LOGI(TAG,
                 "UV_SELFTEST reject code=%s pos=%d valid=%d fresh=%d "
                 "stable=%d moving=%d fault=%d emerg=%d mcp_known=%d "
                 "uv_idle=%d exec_idle=%d uv_off=%d",
                 code, (int)in.pos, in.pos_valid ? 1 : 0,
                 in.pos_fresh ? 1 : 0, in.pos_stable ? 1 : 0,
                 in.pos_moving ? 1 : 0, in.fault_active ? 1 : 0,
                 in.emergency_active ? 1 : 0, in.mcp_uv_known ? 1 : 0,
                 in.uv_idle ? 1 : 0, in.exec_idle ? 1 : 0,
                 in.uv_confirmed_off ? 1 : 0);
        s_rt.state = UV_STATE_SELFTEST_REJECTED;
        s_rt.last_code = code;
        s_rt.requested_duration_ms = 0;
        s_rt.elapsed_ms = 0;
        xSemaphoreGive(s_mtx);
        if (out_code) *out_code = code;
        return ESP_OK;
    }

    /* 受理：分配单调 request_id，异步提交固定 3000 ms */
    machine_request_id_t rid =
        uv_self_test_next_request_id(s_rt.next_request_id);
    s_rt.next_request_id = rid;
    ESP_LOGI(TAG,
             "UV_SELFTEST request request_id=%" PRIu32 " duration=%u pos=%d "
             "valid=%d fresh=%d stable=%d moving=%d",
             rid, UV_SELF_TEST_DURATION_MS, (int)in.pos,
             in.pos_valid ? 1 : 0, in.pos_fresh ? 1 : 0,
             in.pos_stable ? 1 : 0, in.pos_moving ? 1 : 0);

    uv_request_t req = {
        .request_id = rid,
        .duration_ms = UV_SELF_TEST_DURATION_MS,
    };
    esp_err_t err = uv_service_run_async(&req);
    ESP_LOGI(TAG, "UV_SELFTEST submit result=0x%x", (unsigned)err);
    if (err != ESP_OK) {
        s_rt.state = UV_STATE_SELFTEST_FAULT;
        s_rt.last_code = "UV_SUBMIT_FAILED";
        s_rt.requested_duration_ms = 0;
        xSemaphoreGive(s_mtx);
        if (out_code) *out_code = "UV_SUBMIT_FAILED";
        return ESP_OK;
    }

    s_rt.state = UV_STATE_SELFTEST_VALIDATING;
    s_rt.last_code = "UV_SELF_TEST_ACCEPTED";
    s_rt.request_id = rid;
    s_rt.requested_duration_ms = UV_SELF_TEST_DURATION_MS;
    s_rt.elapsed_ms = 0;
    s_rt.submit_tick_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    s_rt.terminal = UV_TERMINAL_NONE;
    s_rt.lamp_on = false;
    xSemaphoreGive(s_mtx);

    if (out_code) *out_code = "UV_SELF_TEST_ACCEPTED";
    if (out_duration_ms) *out_duration_ms = UV_SELF_TEST_DURATION_MS;
    return ESP_OK;
}

esp_err_t uv_self_test_emergency(void)
{
    if (!s_mtx) return ESP_ERR_INVALID_STATE;
    esp_err_t off_err = uv_service_emergency_stop();
    if (lock()) {
        if (s_rt.initialized && s_rt.state == UV_STATE_SELFTEST_RUNNING) {
            s_rt.state = UV_STATE_SELFTEST_STOPPING;
            s_rt.lamp_on = false;
        }
        xSemaphoreGive(s_mtx);
    }
    return off_err;
}

static void sync_uv_state_locked(const uv_snapshot_t *uv)
{
    /* 仅推进运行中的自检；终态保持，直到下次 request 重置 */
    switch (s_rt.state) {
    case UV_STATE_SELFTEST_VALIDATING:
    case UV_STATE_SELFTEST_RUNNING:
    case UV_STATE_SELFTEST_STOPPING:
        break;
    default:
        s_rt.lamp_on = (uv->output_state == UV_ST_OUTPUT_KNOWN_ON);
        s_rt.elapsed_ms = uv->elapsed_ms;
        return;
    }

    s_rt.lamp_on = (uv->output_state == UV_ST_OUTPUT_KNOWN_ON);
    s_rt.elapsed_ms = uv->elapsed_ms;

    /* HW-FIX-3：uv_service 在发布 terminal 后回 IDLE，但 fsm.terminal 与
     * fsm.output_state 保留。轮询在 IDLE 看到保留的 terminal 即视作已到终态
     * （正常 COMPLETE 后回 IDLE 不再误报"状态丢失"）。契约：
     *   命令受理 → 服务到达终态（terminal != NONE）→ MCP/服务锁存 OFF
     *   （output_state == KNOWN_OFF，经真实可用来源，非光学/电流证据）。
     * 若无电流回读，此证明的是"命令/锁存 OFF"，须如实标注。 */
    if (uv->terminal != UV_TERMINAL_NONE) {
        s_rt.terminal = uv->terminal;
        if (uv->request_id != s_rt.request_id &&
            uv->request_id != MACHINE_REQUEST_ID_INVALID) {
            /* 残留他请求的 terminal：非本自检，fail-closed。 */
            s_rt.state = UV_STATE_SELFTEST_FAULT;
            s_rt.last_code = "UV_STATE_LOST";
            s_rt.terminal = UV_TERMINAL_FAULT;
            return;
        }
        if (uv->output_state != UV_ST_OUTPUT_KNOWN_OFF) {
            s_rt.state = UV_STATE_SELFTEST_FAULT;
            s_rt.last_code = "UV_OFF_FAILED";
            ESP_LOGI(TAG,
                     "UV_SELFTEST lamp OFF NOT confirmed (output=%d terminal=%d) "
                     "elapsed=%" PRIu32,
                     uv->output_state, (int)uv->terminal, uv->elapsed_ms);
        } else {
            s_rt.state = map_uv_terminal(uv->terminal);
            ESP_LOGI(TAG,
                     "UV_SELFTEST terminal=%d state=%d elapsed=%" PRIu32
                     " lamp_off_confirmed",
                     (int)uv->terminal, (int)s_rt.state, uv->elapsed_ms);
        }
        return;
    }

    switch (uv->state) {
    case UV_ST_PUBLIC_VALIDATING:
        s_rt.state = UV_STATE_SELFTEST_VALIDATING;
        break;
    case UV_ST_PUBLIC_RUNNING:
        if (s_rt.state != UV_STATE_SELFTEST_RUNNING)
            ESP_LOGI(TAG, "UV_SELFTEST lamp ON confirmed (elapsed=%" PRIu32 ")",
                     uv->elapsed_ms);
        s_rt.state = UV_STATE_SELFTEST_RUNNING;
        break;
    case UV_ST_PUBLIC_STOPPING:
    case UV_ST_PUBLIC_CANCELING:
    case UV_ST_PUBLIC_SKIPPING:
        s_rt.state = UV_STATE_SELFTEST_STOPPING;
        break;
    case UV_ST_PUBLIC_COMPLETE:
    case UV_ST_PUBLIC_FAULT:
        /* state==COMPLETE/FAULT 时 terminal 必保留，应已被上方处理。此处为
         * 防御：terminal==NONE 异常到达终态 → fail-closed。 */
        s_rt.terminal = uv->terminal;
        s_rt.state = UV_STATE_SELFTEST_FAULT;
        s_rt.last_code = "UV_STATE_LOST";
        break;
    default:
        /* fsm IDLE：尚未受理（宽限窗口内）保持 VALIDATING；超过宽限仍未到
         * 终态/未亮灯 → 状态丢失（fail-closed）。 */
        if (s_rt.state == UV_STATE_SELFTEST_VALIDATING ||
            s_rt.state == UV_STATE_SELFTEST_RUNNING) {
            uint32_t now_ms =
                (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            uint32_t since = (now_ms >= s_rt.submit_tick_ms)
                ? (now_ms - s_rt.submit_tick_ms) : 0U;
            if (since <= UV_SUBMIT_GRACE_MS) {
                s_rt.state = UV_STATE_SELFTEST_VALIDATING;
            } else {
                s_rt.state = UV_STATE_SELFTEST_FAULT;
                s_rt.terminal = UV_TERMINAL_FAULT;
                s_rt.last_code = "UV_STATE_LOST";
            }
        }
        break;
    }
}

esp_err_t uv_self_test_get_snapshot(uv_self_test_snapshot_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!lock()) return ESP_ERR_INVALID_STATE;
    if (!s_rt.initialized) {
        memset(out, 0, sizeof(*out));
        xSemaphoreGive(s_mtx);
        return ESP_ERR_INVALID_STATE;
    }

    uv_snapshot_t uv;
    if (uv_service_get_snapshot(&uv) == ESP_OK) {
        sync_uv_state_locked(&uv);
    }

    out->state = s_rt.state;
    out->request_id = s_rt.request_id;
    out->requested_duration_ms = s_rt.requested_duration_ms;
    out->elapsed_ms = s_rt.elapsed_ms;
    out->terminal = s_rt.terminal;
    out->last_code = s_rt.last_code ? s_rt.last_code : "";
    out->lamp_on = s_rt.lamp_on;
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}

uint8_t uv_self_test_state_code(void)
{
    uv_self_test_snapshot_t snap;
    if (uv_self_test_get_snapshot(&snap) != ESP_OK)
        return (uint8_t)UV_STATE_SELFTEST_INACTIVE;
    return (uint8_t)snap.state;
}
