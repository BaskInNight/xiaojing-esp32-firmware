#include "voice_service.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "wash_planner.h"

static const char *TAG = "voice_svc";

#define VOICE_LOCK_TIMEOUT_MS 100
#define VOICE_PROGRAM_ID_FIRST 0x80000001U
#define VOICE_CONTROL_CONFIDENCE_DEFAULT 750U

typedef struct {
    voice_service_config_t config;
    voice_service_snapshot_t snapshot;
    voice_core_t core;
} voice_runtime_t;

static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static voice_runtime_t s_rt;

static bool take_lock(void)
{
    if (!s_mutex) return false;
    TickType_t ticks = pdMS_TO_TICKS(VOICE_LOCK_TIMEOUT_MS);
    if (ticks == 0) ticks = 1;
    return xSemaphoreTake(s_mutex, ticks) == pdTRUE;
}

static uint32_t next_voice_program_id(uint32_t current)
{
    current++;
    return current < VOICE_PROGRAM_ID_FIRST ?
           VOICE_PROGRAM_ID_FIRST : current;
}

static void refresh_snapshot_locked(void)
{
    s_rt.snapshot.state = s_rt.core.state;
    s_rt.snapshot.generation = s_rt.core.generation;
    s_rt.snapshot.utterance_id = s_rt.core.utterance_id;
}

esp_err_t voice_service_global_init(void)
{
    if (s_mutex) return ESP_OK;
    s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
    return s_mutex ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t voice_service_init(const voice_service_config_t *config)
{
    if (!config || !config->executor || !config->machine_config)
        return ESP_ERR_INVALID_ARG;
    if (config->default_backend != VOICE_WAKE_BACKEND_EDGE_IMPULSE &&
        config->default_backend != VOICE_WAKE_BACKEND_ESP_SR)
        return ESP_ERR_INVALID_ARG;
    esp_err_t err = voice_service_global_init();
    if (err != ESP_OK) return err;
    if (!take_lock()) return ESP_ERR_TIMEOUT;
    if (s_rt.snapshot.initialized) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_rt, 0, sizeof(s_rt));
    s_rt.config = *config;
    voice_core_init(&s_rt.core);
    s_rt.snapshot.initialized = true;
    s_rt.snapshot.backend = config->default_backend;
    s_rt.snapshot.next_program_id = VOICE_PROGRAM_ID_FIRST;
    s_rt.snapshot.invocation_source = VOICE_INVOCATION_WAKE_WORD;
    s_rt.snapshot.route_policy = VOICE_ROUTE_POLICY_AUTO;
    s_rt.snapshot.last_route_domain = VOICE_ROUTE_DOMAIN_REJECT;
    s_rt.snapshot.last_route_decision = VOICE_ROUTE_DECISION_REJECT;
    refresh_snapshot_locked();
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

static esp_err_t finish_start(esp_err_t adapter_result)
{
    if (!take_lock()) return ESP_ERR_TIMEOUT;
    voice_transition_t tr;
    if (adapter_result == ESP_OK) {
        tr = voice_core_process(&s_rt.core, VOICE_EVENT_FRONTEND_READY, 0);
        s_rt.snapshot.running = tr.accepted;
    } else {
        tr = voice_core_process(&s_rt.core, VOICE_EVENT_FAILURE,
                                (uint32_t)adapter_result);
        s_rt.snapshot.last_error = adapter_result;
    }
    refresh_snapshot_locked();
    xSemaphoreGive(s_mutex);
    return adapter_result;
}

esp_err_t voice_service_start(void)
{
    if (!take_lock()) {
        ESP_LOGE(TAG, "start: take_lock failed");
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_rt.snapshot.initialized || s_rt.snapshot.running) {
        ESP_LOGE(TAG, "start: state invalid init=%d running=%d",
                 (int)s_rt.snapshot.initialized, (int)s_rt.snapshot.running);
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    voice_transition_t tr = voice_core_process(
        &s_rt.core, VOICE_EVENT_ENABLE, 0);
    voice_service_config_t cfg = s_rt.config;
    voice_wake_backend_t backend = s_rt.snapshot.backend;
    refresh_snapshot_locked();
    xSemaphoreGive(s_mutex);
    if (!tr.accepted) {
        ESP_LOGE(TAG, "start: VOICE_EVENT_ENABLE rejected (state=%d)",
                 (int)s_rt.core.state);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = cfg.select_backend ?
        cfg.select_backend(backend, cfg.adapter_context) : ESP_OK;
    if (err == ESP_OK && cfg.start_frontend) {
        err = cfg.start_frontend(cfg.adapter_context);
    }
    return finish_start(err);
}

esp_err_t voice_service_stop(void)
{
    if (!s_mutex) return ESP_OK;
    if (!take_lock()) return ESP_ERR_TIMEOUT;
    if (!s_rt.snapshot.initialized) {
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }
    voice_service_config_t cfg = s_rt.config;
    xSemaphoreGive(s_mutex);

    esp_err_t first_err = cfg.stop_frontend ?
        cfg.stop_frontend(cfg.adapter_context) : ESP_OK;
    if (cfg.cancel_io) {
        esp_err_t err = cfg.cancel_io(cfg.adapter_context);
        if (first_err == ESP_OK) first_err = err;
    }
    if (first_err != ESP_OK) return first_err;

    if (!take_lock()) return ESP_ERR_TIMEOUT;
    voice_core_process(&s_rt.core, VOICE_EVENT_DISABLE, 0);
    s_rt.snapshot.running = false;
    s_rt.snapshot.initialized = false;
    s_rt.snapshot.backend_switching = false;
    refresh_snapshot_locked();
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

static esp_err_t execute_transition_action(voice_action_t action)
{
    if (!take_lock()) return ESP_ERR_TIMEOUT;
    voice_service_config_t cfg = s_rt.config;
    xSemaphoreGive(s_mutex);

    esp_err_t err = ESP_OK;
    switch (action) {
    case VOICE_ACTION_NONE:
    case VOICE_ACTION_PARSE_RESPONSE:
    case VOICE_ACTION_SUBMIT_TOOL:
        return ESP_OK;
    case VOICE_ACTION_START_FRONTEND:
        err = cfg.start_frontend ?
              cfg.start_frontend(cfg.adapter_context) : ESP_OK;
        if (err == ESP_OK) {
            return voice_service_handle_event(
                VOICE_EVENT_FRONTEND_READY, 0);
        }
        break;
    case VOICE_ACTION_STOP_FRONTEND:
        err = cfg.stop_frontend ?
              cfg.stop_frontend(cfg.adapter_context) : ESP_OK;
        break;
    case VOICE_ACTION_START_RECORDING:
        /* PTT is also the barge-in path.  Cancel any previous cloud/TTS or
         * capture job before allocating the new recording lease.  The
         * adapter cancel operation is idempotent when no job is active. */
        if (cfg.cancel_io) {
            err = cfg.cancel_io(cfg.adapter_context);
        }
        if (cfg.set_route_context) {
            if (!take_lock()) return ESP_ERR_TIMEOUT;
            voice_invocation_source_t source =
                s_rt.snapshot.invocation_source;
            voice_route_policy_t policy = s_rt.snapshot.route_policy;
            xSemaphoreGive(s_mutex);
            err = cfg.set_route_context(
                source, policy, cfg.adapter_context);
        }
        if (err == ESP_OK) {
            err = cfg.stop_frontend ?
                  cfg.stop_frontend(cfg.adapter_context) : ESP_OK;
        }
        if (err == ESP_OK && cfg.start_recording) {
            err = cfg.start_recording(cfg.adapter_context);
        }
        if (err == ESP_OK) {
            return voice_service_handle_event(
                VOICE_EVENT_RECORDING_STARTED, 0);
        }
        break;
    case VOICE_ACTION_STOP_AND_UPLOAD:
        err = cfg.stop_recording_and_upload ?
              cfg.stop_recording_and_upload(cfg.adapter_context) : ESP_OK;
        if (err == ESP_OK) {
            return voice_service_handle_event(
                VOICE_EVENT_UPLOAD_STARTED, 0);
        }
        break;
    case VOICE_ACTION_PLAY_ACCEPTED:
    case VOICE_ACTION_PLAY_REJECTED:
    case VOICE_ACTION_PLAY_ERROR: {
        voice_prompt_id_t prompt =
            action == VOICE_ACTION_PLAY_ACCEPTED ? VOICE_PROMPT_ACCEPTED :
            action == VOICE_ACTION_PLAY_REJECTED ? VOICE_PROMPT_REJECTED :
                                                   VOICE_PROMPT_ERROR;
        err = cfg.play_prompt ?
              cfg.play_prompt(prompt, cfg.adapter_context) : ESP_OK;
        if (err == ESP_OK) {
            return voice_service_handle_event(
                VOICE_EVENT_PLAYBACK_DONE, 0);
        }
        break;
    }
    case VOICE_ACTION_CANCEL_ALL:
        err = cfg.stop_frontend ?
              cfg.stop_frontend(cfg.adapter_context) : ESP_OK;
        if (cfg.cancel_io) {
            esp_err_t cancel_err = cfg.cancel_io(cfg.adapter_context);
            if (err == ESP_OK) err = cancel_err;
        }
        return err;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    if (err != ESP_OK && action == VOICE_ACTION_PLAY_ERROR) {
        /* Error presentation is best-effort (the amplifier/I2S path may be
         * unavailable while recovering).  Do not leave the core in ERROR if
         * the prompt itself fails: commit the same recovery transition that
         * a successful prompt would have produced, which restarts wake
         * detection and makes BTN2 usable again. */
        ESP_LOGW(TAG, "error prompt failed (0x%x), forcing voice recovery",
                 (unsigned)err);
        (void)voice_service_handle_event(VOICE_EVENT_PLAYBACK_DONE, 0);
    } else if (err != ESP_OK) {
        voice_service_handle_event(
            VOICE_EVENT_FAILURE, (uint32_t)err);
    }
    return err;
}

esp_err_t voice_service_handle_event(voice_event_t event, uint32_t detail)
{
    if (!take_lock()) return ESP_ERR_INVALID_STATE;
    if (!s_rt.snapshot.initialized || !s_rt.snapshot.running ||
        s_rt.snapshot.backend_switching) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (event == VOICE_EVENT_WAKE) {
        s_rt.snapshot.invocation_source = VOICE_INVOCATION_WAKE_WORD;
        s_rt.snapshot.route_policy = VOICE_ROUTE_POLICY_AUTO;
    } else if (event == VOICE_EVENT_PTT_BEGIN) {
        s_rt.snapshot.invocation_source = VOICE_INVOCATION_BUTTON2_PTT;
        s_rt.snapshot.route_policy = VOICE_ROUTE_POLICY_CHAT_PRIORITY;
    }
    voice_transition_t tr = voice_core_process(&s_rt.core, event, detail);
    refresh_snapshot_locked();
    if (!tr.accepted) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(s_mutex);
    return execute_transition_action(tr.action);
}

esp_err_t voice_service_ptt_begin(void)
{
    return voice_service_handle_event(VOICE_EVENT_PTT_BEGIN, 0);
}

esp_err_t voice_service_ptt_end(void)
{
    return voice_service_handle_event(VOICE_EVENT_PTT_END, 0);
}

esp_err_t voice_service_external_recording_begin(
    voice_invocation_source_t source, voice_route_policy_t policy)
{
    if (source < VOICE_INVOCATION_WAKE_WORD ||
        source >= VOICE_INVOCATION_COUNT ||
        policy < VOICE_ROUTE_POLICY_AUTO ||
        policy >= VOICE_ROUTE_POLICY_COUNT)
        return ESP_ERR_INVALID_ARG;
    if (!take_lock()) return ESP_ERR_TIMEOUT;
    if (!s_rt.snapshot.initialized || !s_rt.snapshot.running ||
        s_rt.snapshot.backend_switching) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s_rt.snapshot.invocation_source = source;
    s_rt.snapshot.route_policy = policy;
    voice_transition_t wake = voice_core_process(
        &s_rt.core, VOICE_EVENT_WAKE, 0);
    voice_transition_t recording = wake.accepted
        ? voice_core_process(&s_rt.core, VOICE_EVENT_RECORDING_STARTED, 0)
        : (voice_transition_t){0};
    voice_service_config_t cfg = s_rt.config;
    refresh_snapshot_locked();
    xSemaphoreGive(s_mutex);
    if (!wake.accepted || !recording.accepted)
        return ESP_ERR_INVALID_STATE;
    /* Stop only wake recognition.  audio_controller remains the sole I2S
     * owner and is already transitioning to its dialog recorder. */
    return cfg.stop_frontend
        ? cfg.stop_frontend(cfg.adapter_context) : ESP_OK;
}

esp_err_t voice_service_external_upload_begin(void)
{
    if (!take_lock()) return ESP_ERR_TIMEOUT;
    if (!s_rt.snapshot.initialized || !s_rt.snapshot.running ||
        s_rt.snapshot.backend_switching) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    voice_transition_t ready = voice_core_process(
        &s_rt.core, VOICE_EVENT_UTTERANCE_READY, 0);
    voice_transition_t upload = ready.accepted
        ? voice_core_process(&s_rt.core, VOICE_EVENT_UPLOAD_STARTED, 0)
        : (voice_transition_t){0};
    refresh_snapshot_locked();
    xSemaphoreGive(s_mutex);
    return ready.accepted && upload.accepted
        ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t route_tool(const voice_tool_request_t *request,
                            uint32_t program_id,
                            const voice_service_config_t *cfg)
{
    switch (request->tool) {
    case VOICE_TOOL_START_PROGRAM: {
        wash_intent_t intent;
        if (!voice_tool_to_wash_intent(request, &intent))
            return ESP_ERR_INVALID_ARG;
        wash_program_t program;
        planner_report_t report = wash_planner_compile(
            &intent, cfg->machine_config, program_id, &program);
        if (report.result != PLAN_RESULT_OK) return ESP_ERR_INVALID_ARG;
        return wash_executor_submit_program(
            cfg->executor, &program, program_id);
    }
    case VOICE_TOOL_CANCEL:
        return wash_executor_submit_urgent(cfg->executor, false);
    case VOICE_TOOL_STATUS: {
        wash_exec_snapshot_t snapshot;
        return wash_executor_get_snapshot(cfg->executor, &snapshot);
    }
    case VOICE_TOOL_ACK_LOAD:
        return wash_executor_confirm_load(cfg->executor);
    case VOICE_TOOL_ACK_UNLOAD:
        return wash_executor_confirm_unload(cfg->executor);
    case VOICE_TOOL_SKIP_UV:
        return wash_executor_skip_current_step(cfg->executor);
    case VOICE_TOOL_ACK_FAULT:
        return wash_executor_ack_fault(cfg->executor);
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t voice_service_process_tool_json(const char *json, size_t length)
{
    voice_tool_request_t request;
    voice_parse_result_t parse = voice_tool_parse_json(json, length, &request);

    if (!take_lock()) return ESP_ERR_INVALID_STATE;
    if (!s_rt.snapshot.initialized || !s_rt.snapshot.running ||
        s_rt.snapshot.backend_switching ||
        s_rt.core.state != VOICE_STATE_PARSING) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s_rt.snapshot.last_parse_result = parse;
    if (parse != VOICE_PARSE_OK) {
        s_rt.snapshot.rejected_commands++;
        s_rt.snapshot.last_error = ESP_ERR_INVALID_ARG;
        voice_transition_t tr = voice_core_process(
            &s_rt.core, VOICE_EVENT_FAILURE, (uint32_t)parse);
        refresh_snapshot_locked();
        xSemaphoreGive(s_mutex);
        execute_transition_action(tr.action);
        return ESP_ERR_INVALID_ARG;
    }

    voice_transition_t tr = voice_core_process(
        &s_rt.core, VOICE_EVENT_TOOL_PARSED, 0);
    uint32_t program_id = s_rt.snapshot.next_program_id;
    voice_service_config_t cfg = s_rt.config;
    refresh_snapshot_locked();
    xSemaphoreGive(s_mutex);
    if (!tr.accepted) return ESP_ERR_INVALID_STATE;

    esp_err_t route_err = route_tool(&request, program_id, &cfg);

    if (!take_lock()) return ESP_ERR_TIMEOUT;
    tr = voice_core_process(
        &s_rt.core,
        route_err == ESP_OK ? VOICE_EVENT_TOOL_ACCEPTED :
                              VOICE_EVENT_TOOL_REJECTED,
        (uint32_t)route_err);
    if (route_err == ESP_OK) {
        s_rt.snapshot.accepted_commands++;
        if (request.tool == VOICE_TOOL_START_PROGRAM) {
            s_rt.snapshot.next_program_id =
                next_voice_program_id(program_id);
        }
    } else {
        s_rt.snapshot.rejected_commands++;
    }
    s_rt.snapshot.last_error = route_err;
    refresh_snapshot_locked();
    xSemaphoreGive(s_mutex);
    esp_err_t action_err = execute_transition_action(tr.action);
    return route_err != ESP_OK ? route_err : action_err;
}

static esp_err_t complete_route_without_tool(
    const voice_route_result_t *route,
    voice_route_decision_t decision,
    const voice_service_config_t *cfg)
{
    esp_err_t err = ESP_OK;
    if (route->reply_text[0] != '\0' && cfg->speak_text) {
        err = cfg->speak_text(route->reply_text, cfg->adapter_context);
    } else if (cfg->play_prompt) {
        voice_prompt_id_t prompt =
            decision == VOICE_ROUTE_DECISION_REQUIRE_CONFIRMATION
                ? VOICE_PROMPT_NEED_CONFIRMATION
                : decision == VOICE_ROUTE_DECISION_REJECT
                    ? VOICE_PROMPT_REJECTED
                    : VOICE_PROMPT_ACCEPTED;
        err = cfg->play_prompt(prompt, cfg->adapter_context);
    }
    if (err == ESP_OK)
        return voice_service_handle_event(VOICE_EVENT_PLAYBACK_DONE, 0);
    (void)voice_service_handle_event(
        VOICE_EVENT_FAILURE, (uint32_t)err);
    return err;
}

esp_err_t voice_service_process_route_result(
    const voice_route_result_t *route)
{
    if (!route) return ESP_ERR_INVALID_ARG;
    if (!take_lock()) return ESP_ERR_INVALID_STATE;
    if (!s_rt.snapshot.initialized || !s_rt.snapshot.running ||
        s_rt.snapshot.backend_switching ||
        s_rt.core.state != VOICE_STATE_PARSING) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t threshold = s_rt.config.min_control_confidence_milli
        ? s_rt.config.min_control_confidence_milli
        : VOICE_CONTROL_CONFIDENCE_DEFAULT;
    voice_route_decision_t decision = voice_route_authorize(
        route, s_rt.snapshot.route_policy, threshold);
    s_rt.snapshot.last_route_domain = route->domain;
    s_rt.snapshot.last_route_decision = decision;
    voice_transition_t tr = voice_core_process(
        &s_rt.core, VOICE_EVENT_TOOL_PARSED, 0);
    if (!tr.accepted) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t program_id = s_rt.snapshot.next_program_id;
    voice_service_config_t cfg = s_rt.config;
    if (decision != VOICE_ROUTE_DECISION_EXECUTE_TOOL) {
        tr = voice_core_process(
            &s_rt.core,
            decision == VOICE_ROUTE_DECISION_REJECT
                ? VOICE_EVENT_TOOL_REJECTED
                : VOICE_EVENT_TOOL_ACCEPTED,
            0);
        if (decision == VOICE_ROUTE_DECISION_REJECT)
            s_rt.snapshot.rejected_commands++;
        refresh_snapshot_locked();
        xSemaphoreGive(s_mutex);
        if (!tr.accepted) return ESP_ERR_INVALID_STATE;
        return complete_route_without_tool(route, decision, &cfg);
    }
    xSemaphoreGive(s_mutex);

    esp_err_t route_err = route_tool(&route->tool, program_id, &cfg);
    if (!take_lock()) return ESP_ERR_TIMEOUT;
    tr = voice_core_process(
        &s_rt.core,
        route_err == ESP_OK ? VOICE_EVENT_TOOL_ACCEPTED
                            : VOICE_EVENT_TOOL_REJECTED,
        (uint32_t)route_err);
    if (route_err == ESP_OK) {
        s_rt.snapshot.accepted_commands++;
        if (route->tool.tool == VOICE_TOOL_START_PROGRAM) {
            s_rt.snapshot.next_program_id =
                next_voice_program_id(program_id);
        }
    } else {
        s_rt.snapshot.rejected_commands++;
    }
    s_rt.snapshot.last_error = route_err;
    refresh_snapshot_locked();
    xSemaphoreGive(s_mutex);
    esp_err_t action_err = execute_transition_action(tr.action);
    return route_err != ESP_OK ? route_err : action_err;
}

esp_err_t voice_service_toggle_backend(void)
{
    if (!take_lock()) return ESP_ERR_INVALID_STATE;
    if (!s_rt.snapshot.initialized || !s_rt.snapshot.running ||
        s_rt.snapshot.backend_switching ||
        (s_rt.snapshot.state != VOICE_STATE_LISTENING &&
         s_rt.snapshot.state != VOICE_STATE_IDLE &&
         s_rt.snapshot.state != VOICE_STATE_ERROR)) {
        ESP_LOGW(TAG, "backend switch rejected: init=%d running=%d switching=%d state=%d",
                 (int)s_rt.snapshot.initialized, (int)s_rt.snapshot.running,
                 (int)s_rt.snapshot.backend_switching,
                 (int)s_rt.snapshot.state);
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    /* A completed reply normally returns to LISTENING.  If a prompt or a
     * recoverable backend error left the core in IDLE/ERROR, normalize it
     * before the stop/select/start transaction so BTN1 remains usable. */
    voice_state_t prior_state = s_rt.snapshot.state;
    if (prior_state == VOICE_STATE_ERROR) {
        voice_core_process(&s_rt.core, VOICE_EVENT_CANCEL, 0);
        refresh_snapshot_locked();
    }
    s_rt.snapshot.backend_switching = true;
    voice_service_config_t cfg = s_rt.config;
    voice_wake_backend_t old_backend = s_rt.snapshot.backend;
    voice_wake_backend_t new_backend =
        old_backend == VOICE_WAKE_BACKEND_EDGE_IMPULSE ?
        VOICE_WAKE_BACKEND_ESP_SR : VOICE_WAKE_BACKEND_EDGE_IMPULSE;
    xSemaphoreGive(s_mutex);

    /*
     * PTT/cloud routing can run before either wake engine is linked, but BTN1
     * must never report a successful backend switch when there is no backend
     * selector.  Also keep audio-front-end reconfiguration out of an active
     * wash program.
     */
    esp_err_t err = cfg.select_backend ?
        ESP_OK : ESP_ERR_NOT_SUPPORTED;
    wash_exec_snapshot_t exec_snapshot;
    if (err == ESP_OK) {
        err = wash_executor_get_snapshot(cfg.executor, &exec_snapshot);
    }
    if (err == ESP_OK &&
        (exec_snapshot.program_active ||
         exec_snapshot.state != WASH_EXEC_STATE_IDLE)) {
        err = ESP_ERR_INVALID_STATE;
    }

    if (err == ESP_OK && cfg.stop_frontend) {
        err = cfg.stop_frontend(cfg.adapter_context);
    }
    if (err == ESP_OK && cfg.select_backend) {
        err = cfg.select_backend(new_backend, cfg.adapter_context);
    }
    if (err == ESP_OK && cfg.play_prompt) {
        /* Audio feedback is best-effort.  A disconnected/disabled amplifier
         * must not roll a successfully selected wake backend back to the old
         * model.  The following start_frontend result is the authoritative
         * switch outcome. */
        esp_err_t prompt_err = cfg.play_prompt(
            new_backend == VOICE_WAKE_BACKEND_EDGE_IMPULSE ?
            VOICE_PROMPT_BACKEND_EDGE : VOICE_PROMPT_BACKEND_ESP_SR,
            cfg.adapter_context);
        if (prompt_err != ESP_OK) {
            ESP_LOGW(TAG, "backend prompt failed (continuing switch): 0x%x",
                     (unsigned)prompt_err);
        }
    }
    if (err == ESP_OK && cfg.start_frontend) {
        err = cfg.start_frontend(cfg.adapter_context);
    }
    if (err != ESP_OK) {
        if (cfg.select_backend) {
            cfg.select_backend(old_backend, cfg.adapter_context);
        }
        if (cfg.start_frontend) {
            cfg.start_frontend(cfg.adapter_context);
        }
    }

    if (!take_lock()) {
        /* Never leave the public state permanently stuck in SWITCHING when
         * the final bookkeeping lock is temporarily unavailable. */
        if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            s_rt.snapshot.backend_switching = false;
            s_rt.snapshot.last_error = ESP_ERR_TIMEOUT;
            xSemaphoreGive(s_mutex);
        }
        return ESP_ERR_TIMEOUT;
    }
    if (err == ESP_OK) {
        s_rt.snapshot.backend = new_backend;
        s_rt.snapshot.backend_switches++;
        if (prior_state == VOICE_STATE_IDLE || prior_state == VOICE_STATE_ERROR) {
            /* start_frontend succeeded, so publish readiness for an idle
             * core that had lost the normal FRONTEND_READY transition. */
            (void)voice_core_process(&s_rt.core,
                                     VOICE_EVENT_FRONTEND_READY, 0);
        }
    } else {
        s_rt.snapshot.last_error = err;
    }
    s_rt.snapshot.backend_switching = false;
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t voice_service_get_snapshot(voice_service_snapshot_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!take_lock()) return ESP_ERR_INVALID_STATE;
    voice_service_snapshot_t tmp = s_rt.snapshot;
    tmp.state = s_rt.core.state;
    tmp.generation = s_rt.core.generation;
    tmp.utterance_id = s_rt.core.utterance_id;
    *out = tmp;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

#ifdef XIAOJING_TESTING
void voice_service_test_reset(void)
{
    if (!s_mutex || !take_lock()) return;
    memset(&s_rt, 0, sizeof(s_rt));
    xSemaphoreGive(s_mutex);
}
#endif
