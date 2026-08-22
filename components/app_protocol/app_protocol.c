#include "app_protocol.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "app_protocol_core.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "machine_status_store.h"
#include "wash_planner.h"
#include "program_control.h"

#define APP_FRAME_MAX_BYTES 2048U
#define APP_REPLY_MAX_BYTES 640U
#define APP_STATUS_MAX_BYTES 880U
#define APP_TX_TIMEOUT_MS 50U

typedef struct {
    app_protocol_config_t config;
    app_seq_cache_t cache;
    app_protocol_snapshot_t snapshot;
} app_runtime_t;

static app_runtime_t s_rt;
static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;

static bool lock_runtime(void)
{
    return s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) == pdTRUE;
}

static uint32_t next_nonzero(uint32_t value)
{
    value++;
    return value == 0 ? 1 : value;
}

static bool json_u32(const cJSON *object, const char *name,
                     uint32_t default_value, uint32_t *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!item) { *out = default_value; return true; }
    if (!cJSON_IsNumber(item) || item->valuedouble < 0.0 ||
        item->valuedouble > (double)UINT32_MAX) return false;
    uint32_t value = (uint32_t)item->valuedouble;
    if ((double)value != item->valuedouble) return false;
    *out = value;
    return true;
}

static bool json_bool(const cJSON *object, const char *name,
                      bool default_value, bool *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!item) { *out = default_value; return true; }
    if (!cJSON_IsBool(item)) return false;
    *out = cJSON_IsTrue(item);
    return true;
}

static bool json_string(const cJSON *object, const char *name,
                        bool required, const char **out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!item) {
        if (required) return false;
        *out = "";
        return true;
    }
    if (!cJSON_IsString(item) || !item->valuestring) return false;
    *out = item->valuestring;
    return true;
}

static bool action_type_from_json(const cJSON *item, wash_action_type_t *out)
{
    if (cJSON_IsNumber(item) && item->valueint >= WASH_ACTION_MOVE_POSITION &&
        item->valueint <= WASH_ACTION_UV) {
        *out = (wash_action_type_t)item->valueint;
        return true;
    }
    if (!cJSON_IsString(item) || !item->valuestring) return false;
    static const struct { const char *name; wash_action_type_t type; } map[] = {
        {"move_position", WASH_ACTION_MOVE_POSITION},
        {"water_in", WASH_ACTION_WATER_IN},
        {"detergent", WASH_ACTION_DETERGENT},
        {"pulsator_wash", WASH_ACTION_PULSATOR_WASH},
        {"drum_wash", WASH_ACTION_DRUM_WASH},
        {"drain", WASH_ACTION_DRAIN}, {"spin", WASH_ACTION_SPIN},
        {"dry", WASH_ACTION_DRY}, {"uv", WASH_ACTION_UV},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(item->valuestring, map[i].name) == 0) {
            *out = map[i].type;
            return true;
        }
    }
    return false;
}

static bool intensity_from_json(const cJSON *item, wash_intensity_t *out)
{
    if (!item) { *out = WASH_INTENSITY_NORMAL; return true; }
    if (cJSON_IsNumber(item) && item->valueint >= WASH_INTENSITY_GENTLE &&
        item->valueint <= WASH_INTENSITY_STRONG) {
        *out = (wash_intensity_t)item->valueint;
        return true;
    }
    if (!cJSON_IsString(item) || !item->valuestring) return false;
    static const char *names[] = {"gentle", "low", "normal", "strong"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(item->valuestring, names[i]) == 0) {
            *out = (wash_intensity_t)i;
            return true;
        }
    }
    return false;
}

static bool build_intent(const app_protocol_message_t *message,
                         const cJSON *root, wash_intent_t *intent,
                         const char **error_code)
{
    memset(intent, 0, sizeof(*intent));
    intent->source = APP_SOURCE_BLE;
    intent->allow_uv = APP_PROTOCOL_ALLOW_UV_DEFAULT;
    intent->allow_dry = true;
    if (message->command == APP_CMD_START_FORMAL ||
        message->command == APP_CMD_LEGACY_START) {
        intent->kind = WASH_PROGRAM_FORMAL;
    } else if (message->command == APP_CMD_START_DEMO) {
        intent->kind = WASH_PROGRAM_DEMO;
    } else {
        intent->kind = WASH_PROGRAM_CUSTOM;
    }

    if (root) {
        if (!json_bool(root, "allow_uv", APP_PROTOCOL_ALLOW_UV_DEFAULT,
                       &intent->allow_uv) ||
            !json_bool(root, "allow_dry", true, &intent->allow_dry)) {
            *error_code = "BAD_PAYLOAD";
            return false;
        }
    }
    if (intent->kind != WASH_PROGRAM_CUSTOM) return true;
    if (!root) { *error_code = "BAD_PAYLOAD"; return false; }

    const cJSON *actions = cJSON_GetObjectItemCaseSensitive(root, "actions");
    if (!cJSON_IsArray(actions)) { *error_code = "MISSING_ACTIONS"; return false; }
    int count = cJSON_GetArraySize(actions);
    if (count <= 0 || count > WASH_INTENT_MAX_ACTIONS) {
        *error_code = "ACTION_COUNT";
        return false;
    }
    intent->action_count = (size_t)count;
    for (int i = 0; i < count; i++) {
        const cJSON *src = cJSON_GetArrayItem(actions, i);
        wash_action_t *dst = &intent->actions[i];
        if (!cJSON_IsObject(src) ||
            !action_type_from_json(cJSON_GetObjectItemCaseSensitive(src, "type"),
                                   &dst->type)) {
            *error_code = "BAD_ACTION_TYPE";
            return false;
        }
        dst->enabled = true;
        if (!json_bool(src, "enabled", true, &dst->enabled) ||
            !json_u32(src, "duration_ms", 0, &dst->duration_ms)) {
            *error_code = "BAD_ACTION_FIELD";
            return false;
        }
        uint32_t rounds = 1;
        if (!json_u32(src, "rounds", 1, &rounds) || rounds > UINT16_MAX) {
            *error_code = "BAD_ROUNDS";
            return false;
        }
        dst->rounds = (uint16_t)rounds;
        if (!intensity_from_json(cJSON_GetObjectItemCaseSensitive(src, "intensity"),
                                 &dst->intensity)) {
            *error_code = "BAD_INTENSITY";
            return false;
        }
        uint32_t position = 0;
        if (!json_u32(src, "position", 0, &position) || position > INT32_MAX) {
            *error_code = "BAD_POSITION";
            return false;
        }
        dst->position = (drum_position_t)position;
    }
    return true;
}

/* Routing, encoding, and lifecycle follow below. */
static esp_err_t tx_critical_locked(const char *json, size_t length)
{
    esp_err_t err = s_rt.config.critical_tx(json, length, APP_TX_TIMEOUT_MS,
                                             s_rt.config.tx_context);
    if (err != ESP_OK) {
        s_rt.snapshot.tx_errors++;
        s_rt.snapshot.last_error = err;
    }
    return err;
}

static esp_err_t publish_status_locked(void)
{
    machine_status_t status;
    esp_err_t err = machine_status_store_get(&status);
    if (err != ESP_OK) return err;
    uint8_t self_test_state = s_rt.config.uv_self_test_state
        ? s_rt.config.uv_self_test_state() : 0U;

    uint32_t act_st = 0, act_tg = 0, act_rid = 0, act_dur = 0, act_off = 0,
             act_cd = 0;
    if (s_rt.config.actuator_self_test_snapshot) {
        app_protocol_actuator_snapshot_t act;
        if (s_rt.config.actuator_self_test_snapshot(&act)) {
            act_st = act.state; act_tg = act.target; act_rid = act.request_id;
            act_dur = act.duration_ms; act_off = act.output_confirmed_off ? 1U : 0U;
            act_cd = act.hot_air_cooldown_ms;
        }
    }

    uint32_t mb_st = 0, mb_type = 0, mb_rid = 0, mb_step = 0, mb_steps = 0,
             mb_ready = 0, mb_term = 0, mb_off = 0, mb_elapsed = 0;
    int32_t mb_pos = 0;
    uint32_t mb_act = 0;
    const char *mb_lc = "";   /* last_code：ACCEPTED/拒绝码/等待原因/终态码 */
    if (s_rt.config.motor_bench_snapshot) {
        app_protocol_motor_bench_snapshot_t mb;
        if (s_rt.config.motor_bench_snapshot(&mb)) {
            mb_st = mb.state; mb_type = mb.type; mb_rid = mb.request_id;
            mb_step = mb.current_step; mb_steps = mb.step_count;
            mb_pos = mb.required_position; mb_act = mb.current_action;
            mb_ready = mb.position_ready ? 1U : 0U;
            mb_term = mb.terminal; mb_off = mb.output_confirmed_off ? 1U : 0U;
            mb_elapsed = mb.elapsed_ms;
            mb_lc = mb.last_code ? mb.last_code : "";
        }
    }

    uint32_t sw_st = 0, sw_rid = 0, sw_seg = 0, sw_dir = 0, sw_off = 0,
             sw_fl = 0, sw_elapsed = 0, sw_emerg = 0, sw_rounds = 0,
             sw_dwell = 0;
    uint32_t sw_term = 0;
    const char *sw_lc = "";
    if (s_rt.config.position_swing_snapshot) {
        app_protocol_position_swing_snapshot_t sw;
        if (s_rt.config.position_swing_snapshot(&sw)) {
            sw_st = sw.state; sw_term = sw.terminal; sw_rid = sw.request_id;
            sw_seg = sw.segment_index; sw_dir = sw.dir_cw;
            sw_off = sw.off; sw_fl = sw.fl; sw_elapsed = sw.elapsed_ms;
            sw_emerg = sw.emergency; sw_rounds = sw.rounds;
            sw_dwell = sw.dwell_ms;
            sw_lc = sw.last_code ? sw.last_code : "";
        }
    }

    /* V4 板测滑杆参数回显：小程序 status.config.benchtst 初始化/刷新滑杆。 */
    const benchtst_params_t bt = machine_config_get()->benchtst;

    char json[APP_STATUS_MAX_BYTES];
    int n = snprintf(json, sizeof(json),
        "{\"v\":1,\"type\":\"status\",\"revision\":%" PRIu32
        ",\"state\":%d,\"phase\":%d,\"program_id\":%" PRIu32
        ",\"current_step\":%u,\"total_steps\":%u,\"progress\":%u"
        ",\"position\":%d,\"target_position\":%d,\"water_full\":%s"
        ",\"heater_on\":%s,\"uv_on\":%s,\"ble_connected\":%s"
        ",\"fault_code\":%d,\"fault_detail\":%" PRIu32
        ",\"uv_self_test\":%u,\"actuator_self_test\":{\"st\":%" PRIu32
        ",\"tg\":%" PRIu32 ",\"rid\":%" PRIu32 ",\"dur\":%" PRIu32
        ",\"off\":%" PRIu32 ",\"cd\":%" PRIu32 "}"
        ",\"motor_bench\":{\"st\":%" PRIu32 ",\"type\":%" PRIu32
        ",\"rid\":%" PRIu32 ",\"step\":%" PRIu32 ",\"steps\":%" PRIu32
        ",\"pos\":%d,\"act\":%" PRIu32 ",\"ready\":%" PRIu32
        ",\"term\":%" PRIu32 ",\"off\":%" PRIu32
        ",\"elapsed\":%" PRIu32 ",\"lc\":\"%.32s\"}"
        ",\"position_swing\":{\"st\":%" PRIu32 ",\"term\":%" PRIu32
        ",\"rid\":%" PRIu32 ",\"seg\":%" PRIu32 ",\"dir\":%" PRIu32
        ",\"off\":%" PRIu32 ",\"fl\":%" PRIu32 ",\"elapsed\":%" PRIu32
        ",\"emerg\":%" PRIu32 ",\"rounds\":%" PRIu32 ",\"dwell\":%" PRIu32
        ",\"lc\":\"%.24s\"}"
        ",\"pc\":%d"
        ",\"config\":{\"benchtst\":{\"bl50_pwm\":%u,\"ibt2_pwm\":%u"
        ",\"act_duration_ms\":%" PRIu32 "}}}",
        status.revision, (int)status.state, (int)status.phase,
        status.program_id, status.current_step, status.total_steps,
        status.progress_percent, (int)status.position,
        (int)status.target_position, status.water_full ? "true" : "false",
        status.heater_on ? "true" : "false", status.uv_on ? "true" : "false",
        status.ble_connected ? "true" : "false", (int)status.fault.code,
        status.fault.detail, self_test_state,
        act_st, act_tg, act_rid, act_dur, act_off, act_cd,
        mb_st, mb_type, mb_rid, mb_step, mb_steps, (int)mb_pos, mb_act, mb_ready,
        mb_term, mb_off, mb_elapsed, mb_lc,
        sw_st, sw_term, sw_rid, sw_seg, sw_dir, sw_off, sw_fl, sw_elapsed,
        sw_emerg, sw_rounds, sw_dwell, sw_lc,
        (int)program_control_get_state(),
        (unsigned)bt.bl50_pwm, (unsigned)bt.ibt2_pwm, bt.act_duration_ms);
    if (n < 0 || (size_t)n >= sizeof(json)) return ESP_ERR_INVALID_SIZE;
    err = s_rt.config.status_tx(json, (size_t)n, 0, s_rt.config.tx_context);
    if (err != ESP_OK) {
        s_rt.snapshot.tx_errors++;
        s_rt.snapshot.last_error = err;
    }
    return err;
}

static esp_err_t publish_capabilities_locked(void)
{
    char json[APP_REPLY_MAX_BYTES];
    int n = snprintf(json, sizeof(json),
        "{\"v\":1,\"type\":\"capabilities\",\"max_frame\":2048,"
        "\"legacy\":%s,\"commands\":[\"hello\",\"get_status\","
        "\"start_formal\",\"start_demo\",\"submit_plan\",\"ack_load\","
        "\"ack_unload\",\"skip_uv\",\"abort_reset\",\"ack_fault\"%s],"
        "\"diagnostics\":[\"uv_self_test\",\"actuator_self_test\","
        "\"motor_bench_self_test\",\"set_direction_calibration\","
        "\"position_move_test\",\"position_swing_test\","
        "\"set_diag_prefs\",\"reset_diag_prefs\"],"
        "\"actuator_self_test_targets\":["
        "\"source_valve\",\"transfer_valve\",\"detergent_pump\","
        "\"drain_pump\",\"hot_air_coupled\"],"
        "\"motor_bench_types\":[\"pulsator\",\"drum\",\"hall_sequence\","
        "\"pulsator_behavior\"]}",
        s_rt.config.legacy_enabled ? "true" : "false",
        s_rt.config.provision_wifi ? ",\"provision_wifi\"" : "");
    if (n < 0 || (size_t)n >= sizeof(json)) return ESP_ERR_INVALID_SIZE;
    /* 板测诊断：日志显示 capabilities 全文，便于串口核对固件服务的诊断集。 */
    ESP_LOGI("app_protocol", "capabilities published: %.160s", json);
    return tx_critical_locked(json, (size_t)n);
}

typedef struct {
    bool ok;
    const char *code;
    uint32_t detail;
    uint32_t program_id;
    bool send_capabilities;
    bool send_status;
    bool send_duration;      /* ack 附加 duration_ms 字段 */
    uint32_t duration_ms;
    /* ack 附加 target 字段（执行器板测）。target_name 指向 target_buf，
     * 该缓冲区生命周期独立于 cJSON 树 —— 绝不保存 cJSON valuestring
     * 裸指针到 root 之外。 */
    char target_buf[APP_PROTOCOL_TARGET_NAME_MAX];
    bool target_present;
    const char *target_name;
} route_result_t;

static route_result_t route_command_locked(const app_protocol_message_t *message,
                                            const cJSON *root,
                                            const char *frame, size_t frame_len)
{
    route_result_t result = { .ok = false, .code = "INTERNAL" };
    esp_err_t err = ESP_OK;
    switch (message->command) {
    case APP_CMD_HELLO:
        result.ok = true; result.code = "OK";
        result.send_capabilities = true; result.send_status = true;
        break;
    case APP_CMD_GET_STATUS:
        result.ok = true; result.code = "OK"; result.send_status = true;
        break;
    case APP_CMD_START_FORMAL:
    case APP_CMD_SUBMIT_PLAN:
    case APP_CMD_LEGACY_START: {
        if (message->legacy && !s_rt.config.legacy_enabled) {
            result.code = "LEGACY_DISABLED";
            break;
        }
        wash_intent_t intent;
        const char *payload_error = NULL;
        if (!build_intent(message, root, &intent, &payload_error)) {
            result.code = payload_error ? payload_error : "BAD_PAYLOAD";
            break;
        }
        uint32_t program_id = s_rt.snapshot.next_program_id;
        s_rt.snapshot.next_program_id = next_nonzero(program_id);
        wash_program_t program;
        planner_report_t report = wash_planner_compile(
            &intent, s_rt.config.machine_config, program_id, &program);
        if (report.result != PLAN_RESULT_OK) {
            result.code = "PLAN_REJECTED";
            result.detail = (uint32_t)report.reject_reason;
            s_rt.snapshot.planner_rejections++;
            break;
        }
        err = wash_executor_submit_program(s_rt.config.executor, &program,
                                           program_id);
        if (err != ESP_OK) {
            result.code = err == ESP_ERR_INVALID_STATE ? "BUSY" : "EXECUTOR_ERROR";
            result.detail = (uint32_t)err;
            break;
        }
        result.ok = true; result.code = "ACCEPTED";
        result.program_id = program_id;
        break;
    }
    case APP_CMD_START_DEMO: {
        /* DEFAULT_DEMO_V1（26 步）经 program_control 仲裁后提交。 */
        if (message->legacy && !s_rt.config.legacy_enabled) {
            result.code = "LEGACY_DISABLED";
            break;
        }
        pc_action_t action = PC_ACTION_NONE;
        err = program_control_request(PC_REQ_START_DEMO, &action);
        if (err != ESP_OK) {
            /* 仲裁已放行但 executor 拒绝（如诊断组件正持有 executor） */
            result.code = err == ESP_ERR_INVALID_STATE ? "BUSY" : "EXECUTOR_ERROR";
            result.detail = (uint32_t)err;
            break;
        }
        if (action == PC_ACTION_START_DEMO) {
            result.ok = true; result.code = "ACCEPTED";
            result.program_id = program_control_last_program_id();
        } else {
            /* 仲裁拒绝：运行中 / 故障 / 诊断占用 / stop 竞态 */
            result.code = "BUSY";
        }
        break;
    }
    case APP_CMD_ACK_LOAD:
        err = wash_executor_confirm_load(s_rt.config.executor);
        break;
    case APP_CMD_ACK_UNLOAD:
        err = wash_executor_confirm_unload(s_rt.config.executor);
        break;
    case APP_CMD_SKIP_UV:
        err = wash_executor_skip_current_step(s_rt.config.executor);
        break;
    case APP_CMD_ABORT_RESET: {
        /* 经 program_control 仲裁后立即中止（保持原回复语义：无程序可中止
         * 时报 INVALID_STATE）。 */
        pc_action_t action = PC_ACTION_NONE;
        err = program_control_request(PC_REQ_ABORT, &action);
        if (err == ESP_OK && action != PC_ACTION_ABORT_PROGRAM) {
            err = ESP_ERR_INVALID_STATE;
        }
        break;
    }
    case APP_CMD_ACK_FAULT:
        err = wash_executor_ack_fault(s_rt.config.executor);
        break;
    case APP_CMD_PROVISION_WIFI: {
        const char *ssid = NULL;
        const char *password = NULL;
        if (!s_rt.config.provision_wifi) {
            result.code = "NOT_SUPPORTED";
            break;
        }
        if (!root ||
            !json_string(root, "ssid", true, &ssid) ||
            !json_string(root, "password", false, &password)) {
            result.code = "BAD_WIFI_PAYLOAD";
            break;
        }
        err = s_rt.config.provision_wifi(
            ssid, password, s_rt.config.provision_context);
        if (err == ESP_OK) {
            result.ok = true;
            result.code = "WIFI_CONNECTING";
        } else {
            result.code = err == ESP_ERR_INVALID_ARG
                ? "BAD_WIFI_CREDENTIALS"
                : (err == ESP_ERR_INVALID_STATE
                    ? "WIFI_BUSY" : "WIFI_ERROR");
            result.detail = (uint32_t)err;
        }
        break;
    }
    case APP_CMD_LEGACY_STOP:
        if (!s_rt.config.legacy_enabled) {
            result.code = "LEGACY_DISABLED";
            break;
        }
        err = wash_executor_submit_urgent(s_rt.config.executor, true);
        break;
    case APP_CMD_UV_SELF_TEST: {
        if (!s_rt.config.uv_self_test) {
            result.code = "NOT_SUPPORTED";
            break;
        }
        const char *code = NULL;
        uint32_t duration = 0;
        esp_err_t se = s_rt.config.uv_self_test(&code, &duration);
        if (se != ESP_OK) {
            result.code = "INTERNAL";
            result.detail = (uint32_t)se;
            break;
        }
        if (code && strcmp(code, "UV_SELF_TEST_ACCEPTED") == 0) {
            result.ok = true;
            result.code = "UV_SELF_TEST_ACCEPTED";
            result.send_duration = true;
            result.duration_ms = duration;
        } else {
            result.ok = false;
            result.code = (code && code[0] != '\0') ? code : "REJECTED";
        }
        break;
    }
    case APP_CMD_ACTUATOR_SELF_TEST: {
        if (!s_rt.config.actuator_self_test) {
            result.code = "NOT_SUPPORTED";
            break;
        }
        const char *target = NULL;
        if (!root || !json_string(root, "target", true, &target) ||
            target[0] == '\0') {
            result.code = "BAD_TARGET";
            break;
        }
        /* 立即快照进有界缓冲区；之后 cJSON_Delete(root) 会释放
         * valuestring，encode_reply 只能读取 result.target_buf。 */
        if (!app_protocol_snapshot_target(
                target, result.target_buf, sizeof(result.target_buf))) {
            result.code = "BAD_TARGET";
            break;
        }
        result.target_present = true;
        const char *code = NULL;
        uint32_t duration = 0;
        esp_err_t se = s_rt.config.actuator_self_test(target, &code, &duration);
        if (se != ESP_OK) {
            result.code = "INTERNAL";
            result.detail = (uint32_t)se;
            break;
        }
        if (code && strcmp(code, "ACTUATOR_SELF_TEST_ACCEPTED") == 0) {
            result.ok = true;
            result.code = "ACTUATOR_SELF_TEST_ACCEPTED";
            result.send_duration = true;
            result.duration_ms = duration;
            result.target_name =
                result.target_present ? result.target_buf : NULL;
        } else {
            result.ok = false;
            result.code = (code && code[0] != '\0') ? code : "REJECTED";
        }
        break;
    }
    case APP_CMD_MOTOR_BENCH_START: {
        if (!s_rt.config.motor_bench_start) {
            result.code = "NOT_SUPPORTED";
            break;
        }
        char bench_type[APP_PROTOCOL_TARGET_NAME_MAX];
        /* P0-2: top-level "type" is the parser's command discriminator and
         * must equal "cmd". The bench type rides on "target" (same key as the
         * sibling actuator_self_test route), read here from the raw frame via
         * the pure host-testable scanner so the contract is pinned by CI. */
        if (!frame ||
            !app_protocol_field_string(frame, frame_len, "target",
                                       bench_type, sizeof(bench_type))) {
            result.code = "BAD_TYPE";
            break;
        }
        const char *type_name = bench_type;
        const char *code = NULL;
        esp_err_t se = s_rt.config.motor_bench_start(type_name, &code);
        if (se != ESP_OK) {
            result.code = "INTERNAL";
            result.detail = (uint32_t)se;
            break;
        }
        if (code && strcmp(code, "MOTOR_BENCH_ACCEPTED") == 0) {
            result.ok = true;
            result.code = "MOTOR_BENCH_ACCEPTED";
        } else {
            result.ok = false;
            result.code = (code && code[0] != '\0') ? code : "REJECTED";
        }
        break;
    }
    case APP_CMD_MOTOR_BENCH_CONFIRM: {
        if (!s_rt.config.motor_bench_confirm) {
            result.code = "NOT_SUPPORTED";
            break;
        }
        const char *code = NULL;
        esp_err_t se = s_rt.config.motor_bench_confirm(&code);
        if (se != ESP_OK) {
            result.code = "INTERNAL";
            result.detail = (uint32_t)se;
            break;
        }
        result.ok = true;
        result.code = (code && code[0] != '\0') ? code : "OK";
        break;
    }
    case APP_CMD_MOTOR_BENCH_CANCEL: {
        if (!s_rt.config.motor_bench_cancel) {
            result.code = "NOT_SUPPORTED";
            break;
        }
        const char *code = NULL;
        esp_err_t se = s_rt.config.motor_bench_cancel(&code);
        if (se != ESP_OK) {
            result.code = "INTERNAL";
            result.detail = (uint32_t)se;
            break;
        }
        result.ok = true;
        result.code = (code && code[0] != '\0') ? code : "OK";
        break;
    }
    case APP_CMD_SET_DIRECTION_CALIBRATION: {
        /* 方向标定（开发/板级诊断）：置 direction_calibrated=true 并记录
         * rpwm_is_cw（IBT-2 有刷位置电机）+ bl50_reverse_dir（BL50 无刷
         * 方向翻转，软件 CW=物理 CW 修正），异步持久化 NVS。BL50 服务持
         * 实时配置指针，下一命令立即生效，无需重启。 */
        bool rpwm_is_cw = true;
        bool bl50_reverse_dir = false;
        if (!root ||
            !json_bool(root, "rpwm_is_cw", true, &rpwm_is_cw) ||
            !json_bool(root, "bl50_reverse_dir", false, &bl50_reverse_dir)) {
            result.code = "BAD_PAYLOAD";
            break;
        }
        esp_err_t se = machine_config_set_direction_calibration(rpwm_is_cw);
        if (se != ESP_OK) {
            result.code = "INTERNAL";
            result.detail = (uint32_t)se;
            break;
        }
        se = machine_config_set_bl50_direction_reverse(bl50_reverse_dir);
        if (se != ESP_OK) {
            result.code = "INTERNAL";
            result.detail = (uint32_t)se;
            break;
        }
        /* NVS 持久化异步委托给内部 RAM 栈的 config_persist 任务：BLE rx
         * 任务栈在 PSRAM，直接 nvs/flash 写会触发 cache 禁用断言崩溃。 */
        se = machine_config_request_persist();
        if (se != ESP_OK) {
            result.code = "INTERNAL";
            result.detail = (uint32_t)se;
            break;
        }
        result.ok = true;
        result.code = "DIRECTION_CALIBRATED";
        result.send_status = true;
        break;
    }
    case APP_CMD_POSITION_MOVE_TEST: {
        /* 换位电机（IBT-2）板测诊断：target 为角度名 "0"/"45"/"90"/"180"/"270"。 */
        if (!s_rt.config.position_move_test) {
            result.code = "NOT_SUPPORTED";
            break;
        }
        char pos_name[APP_PROTOCOL_TARGET_NAME_MAX];
        if (!frame ||
            !app_protocol_field_string(frame, frame_len, "target",
                                       pos_name, sizeof(pos_name))) {
            result.code = "BAD_POSITION";
            break;
        }
        const char *code = NULL;
        esp_err_t se = s_rt.config.position_move_test(pos_name, &code);
        if (se != ESP_OK) {
            result.code = "INTERNAL";
            result.detail = (uint32_t)se;
            break;
        }
        if (code && strcmp(code, "POSITION_MOVE_ACCEPTED") == 0) {
            result.ok = true;
            result.code = "POSITION_MOVE_ACCEPTED";
        } else {
            result.code = (code && code[0] != '\0') ? code : "REJECTED";
        }
        result.send_status = true;
        break;
    }
    case APP_CMD_POSITION_SWING_TEST: {
        if (!s_rt.config.position_swing_test) {
            result.code = "NOT_SUPPORTED";
            break;
        }
        uint32_t rounds = 0, dwell_ms = 0;
        if (root) {
            (void)json_u32(root, "rounds", 0, &rounds);
            (void)json_u32(root, "duration_ms", 0, &dwell_ms);
        }
        const char *code = NULL;
        esp_err_t se = s_rt.config.position_swing_test(
            rounds, dwell_ms, &code);
        if (se != ESP_OK) {
            result.code = "INTERNAL";
            result.detail = (uint32_t)se;
            break;
        }
        if (code && strcmp(code, "POSITION_SWING_ACCEPTED") == 0) {
            result.ok = true;
            result.code = "POSITION_SWING_ACCEPTED";
        } else {
            result.code = (code && code[0] != '\0') ? code : "REJECTED";
        }
        result.send_status = true;
        break;
    }
    case APP_CMD_POSITION_SWING_CANCEL: {
        if (!s_rt.config.position_swing_cancel) {
            result.code = "NOT_SUPPORTED";
            break;
        }
        const char *code = NULL;
        esp_err_t se = s_rt.config.position_swing_cancel(&code);
        if (se != ESP_OK) {
            result.code = "INTERNAL";
            result.detail = (uint32_t)se;
            break;
        }
        result.code = (code && code[0] != '\0') ? code : "CANCELLED";
        result.send_status = true;
        break;
    }
    case APP_CMD_SET_DIAG_PREFS: {
        uint32_t blwm = 0, ibt2 = 0, act_dur = 0;
        bool have_something = false;
        if (root) {
            have_something =
                cJSON_GetObjectItem(root, "bl50_pwm") != NULL ||
                cJSON_GetObjectItem(root, "ibt2_pwm") != NULL ||
                cJSON_GetObjectItem(root, "act_duration_ms") != NULL;
            (void)json_u32(root, "bl50_pwm", 0, &blwm);
            (void)json_u32(root, "ibt2_pwm", 0, &ibt2);
            (void)json_u32(root, "act_duration_ms", 0, &act_dur);
        }
        if (!have_something) { result.code = "BAD_PAYLOAD"; break; }
        const machine_config_t *cfg = machine_config_get();
        benchtst_params_t prefs = cfg->benchtst;
        if (cJSON_GetObjectItem(root, "bl50_pwm") != NULL)         prefs.bl50_pwm = (uint8_t)blwm;
        if (cJSON_GetObjectItem(root, "ibt2_pwm") != NULL)       prefs.ibt2_pwm = (uint8_t)ibt2;
        if (cJSON_GetObjectItem(root, "act_duration_ms") != NULL) prefs.act_duration_ms = act_dur;
        esp_err_t se = machine_config_set_benchtst(&prefs);
        if (se != ESP_OK) { result.code = se == ESP_ERR_INVALID_ARG ? "BAD_PWM" : "INTERNAL";
                            result.detail = (uint32_t)se; break; }
        se = machine_config_request_persist();
        if (se != ESP_OK) { result.code = "INTERNAL"; result.detail = (uint32_t)se; break; }
        result.ok = true;
        result.code = "DIAG_PREFS_SET";
        result.send_status = true;
        break;
    }
    case APP_CMD_RESET_DIAG_PREFS: {
        /* 诊断偏好重置必须只影响 benchtst。禁止清除已验收的电机方向标定
         * 或覆盖任何生产参数；旧实现 restore_defaults() 会造成 BL50 异步拒绝。 */
        esp_err_t se = machine_config_reset_benchtst_defaults();
        if (se != ESP_OK) { result.code = "INTERNAL"; result.detail = (uint32_t)se; break; }
        se = machine_config_request_persist();
        if (se != ESP_OK) { result.code = "INTERNAL"; result.detail = (uint32_t)se; break; }
        result.ok = true;
        result.code = "DIAG_PREFS_RESET";
        result.send_status = true;
        break;
    }
    case APP_CMD_PAUSE:
    case APP_CMD_RESUME:
        result.code = "NOT_SUPPORTED";
        break;
    default:
        result.code = "UNKNOWN_CMD";
        break;
    }
    if (message->command == APP_CMD_ACK_LOAD ||
        message->command == APP_CMD_ACK_UNLOAD ||
        message->command == APP_CMD_SKIP_UV ||
        message->command == APP_CMD_ABORT_RESET ||
        message->command == APP_CMD_ACK_FAULT ||
        message->command == APP_CMD_LEGACY_STOP) {
        if (err == ESP_OK) {
            result.ok = true;
            result.code = "OK";
        } else if (!result.code || strcmp(result.code, "INTERNAL") == 0) {
            result.code = err == ESP_ERR_INVALID_STATE ? "INVALID_STATE" :
                                                           "EXECUTOR_ERROR";
            result.detail = (uint32_t)err;
        }
    }
    if (result.ok) s_rt.snapshot.commands_executed++;
    return result;
}

static size_t encode_reply(char *reply, size_t capacity, uint32_t seq,
                           const route_result_t *result)
{
    int n;
    if (result->program_id != 0) {
        n = snprintf(reply, capacity,
            "{\"v\":1,\"type\":\"ack\",\"seq\":%" PRIu32
            ",\"ok\":true,\"code\":\"%s\",\"program_id\":%" PRIu32 "}",
            seq, result->code, result->program_id);
    } else if (result->target_name && result->send_duration) {
        n = snprintf(reply, capacity,
            "{\"v\":1,\"type\":\"ack\",\"seq\":%" PRIu32
            ",\"ok\":true,\"code\":\"%s\",\"target\":\"%s\",\"duration_ms\":%"
            PRIu32 "}",
            seq, result->code, result->target_name, result->duration_ms);
    } else if (result->send_duration) {
        n = snprintf(reply, capacity,
            "{\"v\":1,\"type\":\"ack\",\"seq\":%" PRIu32
            ",\"ok\":true,\"code\":\"%s\",\"duration_ms\":%" PRIu32 "}",
            seq, result->code, result->duration_ms);
    } else {
        n = snprintf(reply, capacity,
            "{\"v\":1,\"type\":\"%s\",\"seq\":%" PRIu32
            ",\"ok\":%s,\"code\":\"%s\",\"detail\":%" PRIu32 "}",
            result->ok ? "ack" : "nack", seq,
            result->ok ? "true" : "false", result->code, result->detail);
    }
    return n > 0 && (size_t)n < capacity ? (size_t)n : 0;
}

static const char *parse_error_code(app_parse_result_t parse)
{
    switch (parse) {
    case APP_PARSE_BAD_JSON: return "BAD_JSON";
    case APP_PARSE_BAD_VERSION: return "BAD_VERSION";
    case APP_PARSE_MISSING_FIELD: return "MISSING_FIELD";
    case APP_PARSE_UNKNOWN_CMD: return "UNKNOWN_CMD";
    case APP_PARSE_FORBIDDEN_CMD: return "FORBIDDEN_HARDWARE";
    default: return "BAD_FRAME";
    }
}

/* Public lifecycle follows below. */
esp_err_t app_protocol_init(const app_protocol_config_t *config)
{
    if (!config || !config->executor || !config->machine_config ||
        !config->critical_tx || !config->status_tx)
        return ESP_ERR_INVALID_ARG;
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
        if (!s_mutex) return ESP_ERR_NO_MEM;
    }
    if (!lock_runtime()) return ESP_ERR_TIMEOUT;
    if (s_rt.snapshot.initialized) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_rt, 0, sizeof(s_rt));
    s_rt.config = *config;
    app_seq_cache_init(&s_rt.cache);
    s_rt.snapshot.initialized = true;
    s_rt.snapshot.next_program_id = 1;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t app_protocol_start(void)
{
    if (!lock_runtime()) return ESP_ERR_INVALID_STATE;
    if (!s_rt.snapshot.initialized || s_rt.snapshot.running) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s_rt.snapshot.running = true;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t app_protocol_stop(void)
{
    if (!s_mutex || !lock_runtime()) return ESP_ERR_INVALID_STATE;
    if (!s_rt.snapshot.initialized) {
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }
    bool connected = s_rt.snapshot.connected;
    memset(&s_rt, 0, sizeof(s_rt));
    xSemaphoreGive(s_mutex);
    if (connected) (void)machine_status_store_set_ble_connected(false);
    return ESP_OK;
}

void app_protocol_set_connected(bool connected)
{
    if (s_mutex && lock_runtime()) {
        if (s_rt.snapshot.initialized) s_rt.snapshot.connected = connected;
        xSemaphoreGive(s_mutex);
    }
    (void)machine_status_store_set_ble_connected(connected);
}

esp_err_t app_protocol_publish_status(void)
{
    if (!s_mutex || !lock_runtime()) return ESP_ERR_INVALID_STATE;
    if (!s_rt.snapshot.running) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = publish_status_locked();
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t app_protocol_process_frame(const char *frame, size_t length)
{
    if (!frame || length == 0 || length > APP_FRAME_MAX_BYTES)
        return ESP_ERR_INVALID_ARG;
    if (!s_mutex || !lock_runtime()) return ESP_ERR_INVALID_STATE;
    if (!s_rt.snapshot.running) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s_rt.snapshot.frames_received++;

    app_protocol_message_t message;
    app_parse_result_t parse = app_protocol_parse_frame(frame, length, &message);
    if (parse != APP_PARSE_OK) {
        s_rt.snapshot.parse_errors++;
        route_result_t rejected = {
            .ok = false, .code = parse_error_code(parse), .detail = (uint32_t)parse
        };
        char reply[APP_REPLY_MAX_BYTES];
        size_t reply_len = encode_reply(reply, sizeof(reply), message.seq, &rejected);
        esp_err_t err = reply_len ? tx_critical_locked(reply, reply_len)
                                  : ESP_ERR_INVALID_SIZE;
        xSemaphoreGive(s_mutex);
        return err;
    }

    const char *cached_reply = NULL;
    size_t cached_len = 0;
    app_seq_lookup_t lookup = app_seq_cache_lookup(
        &s_rt.cache, &message, &cached_reply, &cached_len);
    if (lookup == APP_SEQ_DUPLICATE) {
        s_rt.snapshot.duplicates_replayed++;
        esp_err_t err = tx_critical_locked(cached_reply, cached_len);
        if (message.command == APP_CMD_HELLO) {
            esp_err_t e = publish_capabilities_locked();
            if (err == ESP_OK) err = e;
        }
        if (message.command == APP_CMD_HELLO ||
            message.command == APP_CMD_GET_STATUS) {
            esp_err_t e = publish_status_locked();
            if (err == ESP_OK) err = e;
        }
        xSemaphoreGive(s_mutex);
        return err;
    }
    if (lookup == APP_SEQ_CONFLICT) {
        s_rt.snapshot.sequence_conflicts++;
        route_result_t conflict = { .ok = false, .code = "SEQ_CONFLICT" };
        char reply[APP_REPLY_MAX_BYTES];
        size_t reply_len = encode_reply(reply, sizeof(reply), message.seq, &conflict);
        esp_err_t err = reply_len ? tx_critical_locked(reply, reply_len)
                                  : ESP_ERR_INVALID_SIZE;
        xSemaphoreGive(s_mutex);
        return err;
    }

    cJSON *root = NULL;
    if (!message.legacy) {
        root = cJSON_ParseWithLengthOpts(frame, length, NULL, false);
        if (!root || !cJSON_IsObject(root)) {
            if (root) cJSON_Delete(root);
            s_rt.snapshot.parse_errors++;
            route_result_t rejected = { .ok = false, .code = "BAD_JSON" };
            char reply[APP_REPLY_MAX_BYTES];
            size_t reply_len = encode_reply(reply, sizeof(reply), message.seq,
                                            &rejected);
            esp_err_t err = reply_len ? tx_critical_locked(reply, reply_len)
                                      : ESP_ERR_INVALID_SIZE;
            xSemaphoreGive(s_mutex);
            return err;
        }
    }

    route_result_t result = route_command_locked(&message, root, frame, length);
    if (root) cJSON_Delete(root);
    char reply[APP_REPLY_MAX_BYTES];
    size_t reply_len = encode_reply(reply, sizeof(reply), message.seq, &result);
    if (reply_len == 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_SIZE;
    }
    if (!message.legacy)
        (void)app_seq_cache_store(&s_rt.cache, &message, reply, reply_len);

    esp_err_t err = tx_critical_locked(reply, reply_len);
    if (result.send_capabilities) {
        esp_err_t e = publish_capabilities_locked();
        if (err == ESP_OK) err = e;
    }
    if (result.send_status) {
        esp_err_t e = publish_status_locked();
        if (err == ESP_OK) err = e;
    }
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t app_protocol_get_snapshot(app_protocol_snapshot_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_mutex || !lock_runtime()) return ESP_ERR_INVALID_STATE;
    if (!s_rt.snapshot.initialized) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    *out = s_rt.snapshot;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}
