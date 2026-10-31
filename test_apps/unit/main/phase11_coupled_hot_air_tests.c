/*
 * phase11_coupled_hot_air_tests.c — Phase 11 Group B 单继电器耦合热风模块
 * 服务层回归测试（FreeRTOS + Fake HAL，无实物通电）。
 *
 * 本文件覆盖独立代码评审提出的服务层缺陷：
 *   - P0: process_fsm_output 在动作失败后不得丢弃 FSM 的补偿/重试动作。Fake
 *     HAL 以 XIAOJING_MODE_FAKE 运行，safety 层按设计拒绝 RELAY_ON（模式门禁），
 *     服务必须仍然执行补偿 RELAY_OFF 并收敛到终态，绝不卡死在 pending_action。
 *   - P1-1: 请求保留位（reservation）在首次请求完成前不得释放；完成后必须释放
 *     以允许下一请求。并发 run_async 在保留期被同步拒绝（BUSY）。
 *
 * 诚实验证：终态为 FAULT（APPLY_FAILED），且补偿 OFF 已确认
 * （output_confirmed_off == true）—— 继电器被可靠带回 OFF。
 */

#include <string.h>
#include <stdint.h>

#include "unity.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fake_hal.h"
#include "safety_manager.h"
#include "machine_config.h"
#include "coupled_hot_air_service.h"

#define P11C_TAG "phase11_cha"
#define P11C_POLL_ITERS 200
#define P11C_POLL_DELAY_MS 50

static fake_hal_ctx_t *s_ctx = NULL;
static const xiaojing_hal_t *s_hal = NULL;
static bool s_up = false;

static esp_err_t p11c_event_sink(const machine_event_t *event,
                                 uint32_t timeout_ms, void *context)
{
    (void)event; (void)timeout_ms; (void)context;
    return ESP_OK;
}

/* 让 FSM 的 VALIDATING 门禁全部通过：270° 稳定新鲜 + 新鲜有效 SHT。
 * 这样请求能进入 STARTING 并真正发起 RELAY_ON（随后被 safety 模式门禁拒绝）。 */
static void p11c_set_safe_env(void)
{
    int64_t now = fake_hal_get_time(s_ctx);
    safety_manager_update_position(DRUM_POS_270, true, false, true,
                                   (uint32_t)now);
    sht_sample_t sht = {
        .temperature_c = 25.0f,
        .humidity_rh = 50.0f,
        .valid = true,
        .timestamp_ms = now,
        .age_ms = 0,
    };
    fake_hal_set_sht(s_ctx, &sht);
}

static esp_err_t p11c_setup(void)
{
    coupled_hot_air_service_global_init();
    s_ctx = fake_hal_create();
    if (!s_ctx) return ESP_ERR_NO_MEM;
    s_hal = fake_hal_get_interface(s_ctx);
    fake_hal_set_time(s_ctx, 10000);
    safety_manager_config_t sm_cfg = {
        .hal = s_hal, .config = machine_config_get(),
        .output_mode = XIAOJING_MODE_FAKE, .ptc_enabled = false,
        .board_identity_confirmed = true,
    };
    if (safety_manager_init(&sm_cfg) != ESP_OK ||
        safety_manager_start() != ESP_OK) {
        fake_hal_destroy(s_ctx); s_ctx = NULL; s_hal = NULL;
        safety_manager_test_reset();
        return ESP_ERR_INVALID_STATE;
    }
    p11c_set_safe_env();
    fake_hal_clear_history(s_ctx);

    machine_event_sink_t sink = { .publish = p11c_event_sink, .context = NULL };
    if (coupled_hot_air_service_init(s_hal, sink) != ESP_OK ||
        coupled_hot_air_service_start() != ESP_OK) {
        coupled_hot_air_service_stop();
        safety_manager_stop(); safety_manager_test_reset();
        fake_hal_destroy(s_ctx); s_ctx = NULL; s_hal = NULL;
        return ESP_ERR_INVALID_STATE;
    }
    s_up = true;
    return ESP_OK;
}

static void p11c_teardown(void)
{
    if (s_up) coupled_hot_air_service_stop();
    s_up = false;
    safety_manager_stop();
    if (s_ctx) { fake_hal_destroy(s_ctx); s_ctx = NULL; s_hal = NULL; }
    safety_manager_test_reset();
}

/* 轮询快照直到出现终态（或超时），返回真表示在超时前到达终态。 */
static bool p11c_poll_terminal(coupled_hot_air_snapshot_t *snap_out)
{
    coupled_hot_air_snapshot_t snap;
    for (int i = 0; i < P11C_POLL_ITERS; i++) {
        if (coupled_hot_air_service_get_snapshot(&snap) == ESP_OK &&
            snap.terminal != CHA_TERMINAL_NONE) {
            if (snap_out) *snap_out = snap;
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(P11C_POLL_DELAY_MS));
    }
    return false;
}

/* Fake HAL 历史中是否出现过 PTC 继电器 OFF 写入。 */
static bool p11c_history_has_ptc_off(void)
{
    fake_output_record_t recs[FAKE_HAL_HISTORY_CAPACITY];
    size_t n = fake_hal_get_history(s_ctx, recs, FAKE_HAL_HISTORY_CAPACITY);
    for (size_t i = 0; i < n; i++)
        if (recs[i].type == FAKE_OUTPUT_SET_SAFE &&
            recs[i].data.safe.output == SAFE_OUTPUT_PTC_HEATER &&
            !recs[i].data.safe.enable)
            return true;
    return false;
}

/* P0: RELAY_ON 被 safety 拒绝（FAKE 模式）后，服务必须执行补偿 RELAY_OFF 并
 * 收敛到终态 —— 不得因动作失败丢弃补偿动作而把 FSM 卡死在 pending_action。 */
TEST_CASE("P11C-01: RELAY_ON rejected -> compensating RELAY_OFF runs + terminal",
          "[phase11][coupled_hot_air][regression]")
{
    TEST_ASSERT_EQUAL(ESP_OK, p11c_setup());

    coupled_hot_air_request_t req = { .request_id = 101, .duration_ms = 1000 };
    TEST_ASSERT_EQUAL(ESP_OK, coupled_hot_air_service_run_async(&req));

    coupled_hot_air_snapshot_t snap;
    bool got = p11c_poll_terminal(&snap);
    TEST_ASSERT_TRUE_MESSAGE(got,
        "P0: 服务在超时内收敛到终态（不得卡死）");
    TEST_ASSERT_EQUAL(CHA_TERMINAL_FAULT, snap.terminal);
    TEST_ASSERT_FALSE(snap.command_on);
    /* 补偿 OFF 成功 → 继电器已确认 OFF。 */
    TEST_ASSERT_TRUE(snap.output_confirmed_off);

    /* 决定性证据：Fake HAL 历史里必须有 PTC 继电器 OFF 写入。 */
    TEST_ASSERT_TRUE_MESSAGE(p11c_history_has_ptc_off(),
        "P0: 补偿 RELAY_OFF 实际被执行（写入 PTC OFF）");

    p11c_teardown();
}

/* P1-1: 保留位在请求完成前不得释放（同步 BUSY），完成后必须释放以接受下一请求。 */
TEST_CASE("P11C-02: reservation held while active, released after terminal",
          "[phase11][coupled_hot_air][regression]")
{
    TEST_ASSERT_EQUAL(ESP_OK, p11c_setup());

    coupled_hot_air_request_t req1 = { .request_id = 201, .duration_ms = 1000 };
    TEST_ASSERT_EQUAL(ESP_OK, coupled_hot_air_service_run_async(&req1));

    /* 同步保留：run_async 在返回前已占用保留位，第二个请求必然 BUSY。
     * 无时序依赖，确定性断言。 */
    coupled_hot_air_request_t req2 = { .request_id = 202, .duration_ms = 1000 };
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      coupled_hot_air_service_run_async(&req2));

    coupled_hot_air_snapshot_t snap;
    TEST_ASSERT_TRUE(p11c_poll_terminal(&snap));

    /* 完成后保留位必须释放：下一请求可被接受（P1-1）。 */
    bool accepted = false;
    for (int i = 0; i < P11C_POLL_ITERS; i++) {
        coupled_hot_air_request_t req3 = { .request_id = 203, .duration_ms = 1000 };
        if (coupled_hot_air_service_run_async(&req3) == ESP_OK) {
            accepted = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(P11C_POLL_DELAY_MS));
    }
    TEST_ASSERT_TRUE_MESSAGE(accepted,
        "P1-1: 首请求终态后保留位已释放，第二请求可接受");

    p11c_teardown();
}
