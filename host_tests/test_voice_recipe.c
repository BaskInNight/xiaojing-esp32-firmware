#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "voice_route.h"
#include "wash_planner.h"
#include "wash_executor_core.h"

static voice_route_parse_result_t parse(const char *actions, voice_route_result_t *route)
{
    char json[4097];
    snprintf(json, sizeof(json), "{\"domain\":\"control\",\"transcript\":\"wash gently\","
        "\"confidence_milli\":950,\"requires_confirmation\":true,\"tool\":\"start_program\","
        "\"program\":\"custom\",\"allow_uv\":false,\"allow_dry\":false,"
        "\"speech_reply\":\"proposal\",\"updated_preferences\":\"\",\"pipeline\":%s}", actions);
    return voice_route_parse_json(json, strlen(json), route);
}

int main(int argc, char **argv)
{
    voice_route_result_t route;
    if (argc > 1) {
        char json[4097]; FILE *file = fopen(argv[1], "rb"); assert(file);
        size_t length = fread(json, 1, sizeof(json), file); fclose(file);
        voice_route_parse_result_t r = voice_route_parse_json(json, length, &route);
        printf("Local live-response validation: %d\n", r);
        assert(r == VOICE_ROUTE_PARSE_OK && route.tool.has_recipe);
        wash_program_t p; assert(wash_recipe_compile(&route.tool.recipe,42,&p).result == PLAN_RESULT_OK);
        printf("actions=%u estimated_ms=%u steps=%u\n",route.tool.recipe.count,(unsigned)p.estimated_total_ms,p.step_count);
        return 0;
    }
    assert(parse("[{\"cmd\":\"CMD_WASH_PULSATOR\",\"val\":1,\"time\":20},"
        "{\"cmd\":\"CMD_SPIN_DRY\",\"val\":2,\"time\":8}]", &route) == VOICE_ROUTE_PARSE_OK);
    assert(route.tool.has_recipe && route.tool.recipe.count == 2);
    assert(voice_route_authorize(&route, VOICE_ROUTE_POLICY_AUTO, 750) == VOICE_ROUTE_DECISION_REQUIRE_CONFIRMATION);
    route.requires_confirmation = false;
    assert(voice_route_authorize(&route, VOICE_ROUTE_POLICY_AUTO, 750) == VOICE_ROUTE_DECISION_EXECUTE_TOOL);
    assert(voice_route_authorize(&route, VOICE_ROUTE_POLICY_CHAT_PRIORITY, 750) == VOICE_ROUTE_DECISION_REQUIRE_CONFIRMATION);
    wash_program_t program;
    assert(wash_recipe_compile(&route.tool.recipe, 42, &program).result == PLAN_RESULT_OK);
    assert(program.recipe.actions[0].level == 1 && program.kind == WASH_PROGRAM_CUSTOM);
    assert(program.step_count == 3 && program.steps[0].required_position == DRUM_POS_45);
    assert(program.steps[1].type == STEP_WAIT_LOAD_CONFIRM && !program.steps[1].timeout_ms);
    assert(program.steps[2].type == STEP_COMPETITION_DEMO && program.estimated_total_ms < 180000);
    char summary[513];
    assert(wash_recipe_summary(&program.recipe, true, summary, sizeof(summary)));
    assert(!strstr(summary, "CMD_") && !strchr(summary, '{'));
    /* The decoded request owns its data, independent of the source JSON. */
    route.tool.recipe.actions[0].seconds = 99;
    assert(program.recipe.actions[0].seconds == 20);

    wash_executor_core_t core; core_init(&core); core.program = &program; core.total_steps = 3;
    core_cmd_t commands[16]; core_event_t event = {.type = CORE_EVENT_PROGRAM_ACCEPTED};
    event.token.program_id = 42;
    assert(core_process_event(&core, &event, commands, 16) == 1);
    event = (core_event_t){.type = CORE_EVENT_STEP_DISPATCH_OK};
    core_process_event(&core, &event, commands, 16);
    event = (core_event_t){.type = CORE_EVENT_SERVICE_TERMINAL, .token = core.active_token, .success = true};
    core_process_event(&core, &event, commands, 16);
    assert(core.phase == CORE_PHASE_WAIT_LOAD);
    event = (core_event_t){.type = CORE_EVENT_LOAD_CONFIRMED};
    assert(!core_process_event(&core, &event, commands, 16)); /* No stale confirmation. */
    event.token.program_id = core.program_id; event.token.generation = core.generation;
    assert(core_process_event(&core, &event, commands, 16) == 1);
    assert(commands[0].type == CORE_CMD_DISPATCH_STEP && core.current_step_index == 2);

    const char *bad[] = {
        "[]", "[{\"cmd\":\"standard wash\",\"val\":1,\"time\":20}]",
        "[{\"cmd\":\"CMD_WASH_PULSATOR\",\"val\":80,\"time\":20}]",
        "[{\"cmd\":\"CMD_WASH_PULSATOR\",\"val\":1,\"time\":31}]",
        "[{\"cmd\":\"CMD_WASH_PULSATOR\",\"val\":1,\"time\":-1}]",
        "[{\"cmd\":\"CMD_WASH_PULSATOR\",\"val\":1,\"time\":20.5}]",
        "[{\"cmd\":\"CMD_WASH_PULSATOR\",\"val\":1,\"time\":\"20\"}]",
        "[{\"cmd\":\"CMD_WASH_PULSATOR\",\"val\":true,\"time\":20}]",
        "[{\"cmd\":\"CMD_WASH_PULSATOR\",\"val\":01,\"time\":20}]",
        "[{\"cmd\":\"CMD_WASH_PULSATOR\",\"val\":1,\"time\":20,\"time\":21}]",
        "[{\"cmd\":\"CMD_WASH_PULSATOR\",\"val\":1}]",
        "[{\"cmd\":\"CMD_WASH_PULSATOR\",\"val\":1,\"time\":20,\"gpio\":5}]",
        "[{\"cmd\":\"CMD_SPIN_DRY\",\"val\":3,\"time\":12}]",
        "[{\"cmd\":\"CMD_TILT_BARREL\",\"val\":46,\"time\":0}]",
        "[{\"cmd\":\"CMD_WASH_PULSATOR\",\"val\":1,\"time\":20},"
          "{\"cmd\":\"CMD_DRY\",\"val\":1,\"time\":10}]",
        "[{\"cmd\":\"CMD_TURBIDITY_CHECK\",\"val\":0,\"time\":1}]"
    };
    for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i)
        assert(parse(bad[i], &route) == VOICE_ROUTE_PARSE_INVALID_PIPELINE);
    wash_recipe_t too_long = {.count = 4};
    for (unsigned i=0; i<4; ++i) too_long.actions[i] = (wash_recipe_action_t){.op=RECIPE_DRUM,.level=1,.seconds=30};
    assert(!wash_recipe_validate(&too_long, NULL));
    too_long.count = 9; assert(!wash_recipe_validate(&too_long, NULL));
    puts("Voice recipe parser/compiler/executor integration passed");
    return 0;
}
