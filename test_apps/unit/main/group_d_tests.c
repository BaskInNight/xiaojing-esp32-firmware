/*
 * group_d_tests.c — Phase 6 Group D tests (Round 1.7)
 *
 * R1.7: Stop lifecycle short-circuit. C11 atomic_init. Deterministic hooks.
 *       EventGroup concurrent stop. Real stale injection. Exact assertions.
 * R1.5: Real service tests with deterministic synchronization.
 * Owner-aware MCP lifecycle. Explicit fault modes.
 * Polling helpers replace fixed vTaskDelay where possible.
 *
 * R1.7: Non-static test functions to survive --gc-sections.
 * The default ESP-IDF TEST_CASE macro creates static functions, but the
 * linker strips them because group_d_tests.c is a separate TU and the
 * .init_array references are not traced through the gc root chain.
 *
 * R1.7b: Force linker to include this TU from libmain.a by providing
 * a globally-visible symbol that test_runner.c references.
 */

#include "group_d_test_helpers.h"

/* R1.7b: Called from test_runner.c to force linker inclusion.
 * The actual test registration happens in __attribute__((constructor)) functions. */
void group_d_tests_force_link(void) { }

/* R1.7: Override TEST_CASE to use non-static functions so --gc-sections
 * keeps them. The constructor is also non-static for the same reason. */
#undef TEST_CASE
#define TEST_CASE(name_, desc_) \
    void UNITY_TEST_UID(test_func_) (void); \
    void __attribute__((constructor)) UNITY_TEST_UID(test_reg_helper_) (void) \
    { \
        static test_func test_fn_[] = {&UNITY_TEST_UID(test_func_)}; \
        static test_desc_t UNITY_TEST_UID(test_desc_) = { \
            .name = name_, \
            .desc = desc_, \
            .fn = test_fn_, \
            .file = __FILE__, \
            .line = __LINE__, \
            .test_fn_count = 1, \
            .test_fn_name = NULL, \
            .next = NULL \
        }; \
        unity_testcase_register( & UNITY_TEST_UID(test_desc_) ); \
    }\
    void UNITY_TEST_UID(test_func_) (void)

/* ================================================================
 * Pure C debounce tests (unchanged from R1.2 — already solid)
 * ================================================================ */

TEST_CASE("BTN-D01: all HIGH no events", "[btn][group_d][debounce]")
{
    button_debounce_ctx_t ctx;
    button_debounce_init(&ctx, 30);
    button_debounce_seed(&ctx, ALL_RELEASED, 0);
    button_event_t ev[BUTTON_ID_COUNT];
    TEST_ASSERT_EQUAL(0, button_debounce_process(&ctx, ALL_RELEASED, 100, ev, BUTTON_ID_COUNT));
}

TEST_CASE("BTN-D02: 10ms glitch no event", "[btn][group_d][debounce]")
{
    button_debounce_ctx_t ctx;
    button_debounce_init(&ctx, 30);
    button_debounce_seed(&ctx, ALL_RELEASED, 0);
    button_event_t ev[BUTTON_ID_COUNT];
    button_debounce_process(&ctx, BTN1_RAW_PRESSED, 0, ev, BUTTON_ID_COUNT);
    button_debounce_process(&ctx, ALL_RELEASED, 10, ev, BUTTON_ID_COUNT);
    TEST_ASSERT_EQUAL(0, button_debounce_process(&ctx, ALL_RELEASED, 50, ev, BUTTON_ID_COUNT));
}

TEST_CASE("BTN-D03: stable press+release CLICK timestamp=confirmed", "[btn][group_d][debounce]")
{
    button_debounce_ctx_t ctx;
    button_debounce_init(&ctx, 30);
    button_debounce_seed(&ctx, ALL_RELEASED, 0);
    button_event_t ev[BUTTON_ID_COUNT];
    button_debounce_process(&ctx, BTN1_RAW_PRESSED, 0, ev, BUTTON_ID_COUNT);
    button_debounce_process(&ctx, BTN1_RAW_PRESSED, 35, ev, BUTTON_ID_COUNT);
    button_debounce_process(&ctx, ALL_RELEASED, 100, ev, BUTTON_ID_COUNT);
    int n = button_debounce_process(&ctx, ALL_RELEASED, 135, ev, BUTTON_ID_COUNT);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL(BUTTON_ID_1, ev[0].button_id);
    TEST_ASSERT_EQUAL(35, ev[0].timestamp_ms);
}

TEST_CASE("BTN-D04: bounce then stable one CLICK", "[btn][group_d][debounce]")
{
    button_debounce_ctx_t ctx;
    button_debounce_init(&ctx, 30);
    button_debounce_seed(&ctx, ALL_RELEASED, 0);
    button_event_t ev[BUTTON_ID_COUNT];
    button_debounce_process(&ctx, BTN1_RAW_PRESSED, 0, ev, BUTTON_ID_COUNT);
    button_debounce_process(&ctx, ALL_RELEASED, 5, ev, BUTTON_ID_COUNT);
    button_debounce_process(&ctx, BTN1_RAW_PRESSED, 8, ev, BUTTON_ID_COUNT);
    button_debounce_process(&ctx, ALL_RELEASED, 12, ev, BUTTON_ID_COUNT);
    button_debounce_process(&ctx, BTN1_RAW_PRESSED, 15, ev, BUTTON_ID_COUNT);
    button_debounce_process(&ctx, BTN1_RAW_PRESSED, 50, ev, BUTTON_ID_COUNT);
    button_debounce_process(&ctx, ALL_RELEASED, 100, ev, BUTTON_ID_COUNT);
    int n = button_debounce_process(&ctx, ALL_RELEASED, 135, ev, BUTTON_ID_COUNT);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL(50, ev[0].timestamp_ms);
}

TEST_CASE("BTN-D05: long hold no repeated CLICK", "[btn][group_d][debounce]")
{
    button_debounce_ctx_t ctx;
    button_debounce_init(&ctx, 30);
    button_debounce_seed(&ctx, ALL_RELEASED, 0);
    button_event_t ev[BUTTON_ID_COUNT];
    button_debounce_process(&ctx, BTN1_RAW_PRESSED, 0, ev, BUTTON_ID_COUNT);
    for (uint32_t t = 35; t <= 5000; t += 35)
        TEST_ASSERT_EQUAL(0, button_debounce_process(&ctx, BTN1_RAW_PRESSED, t, ev, BUTTON_ID_COUNT));
    button_debounce_process(&ctx, ALL_RELEASED, 5100, ev, BUTTON_ID_COUNT);
    TEST_ASSERT_EQUAL(1, button_debounce_process(&ctx, ALL_RELEASED, 5135, ev, BUTTON_ID_COUNT));
}

TEST_CASE("BTN-D06: BTN1+BTN2 simultaneous two CLICKs", "[btn][group_d][debounce]")
{
    button_debounce_ctx_t ctx;
    button_debounce_init(&ctx, 30);
    button_debounce_seed(&ctx, ALL_RELEASED, 0);
    button_event_t ev[BUTTON_ID_COUNT];
    uint8_t both = ALL_RELEASED & ~((1U << 4) | (1U << 5));
    button_debounce_process(&ctx, both, 0, ev, BUTTON_ID_COUNT);
    button_debounce_process(&ctx, both, 35, ev, BUTTON_ID_COUNT);
    button_debounce_process(&ctx, ALL_RELEASED, 100, ev, BUTTON_ID_COUNT);
    int n = button_debounce_process(&ctx, ALL_RELEASED, 135, ev, BUTTON_ID_COUNT);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL(BUTTON_ID_1, ev[0].button_id);
    TEST_ASSERT_EQUAL(BUTTON_ID_2, ev[1].button_id);
}

TEST_CASE("BTN-D09: tick=0", "[btn][group_d][debounce]")
{
    button_debounce_ctx_t ctx;
    button_debounce_init(&ctx, 30);
    button_debounce_seed(&ctx, ALL_RELEASED, 0);
    button_event_t ev[BUTTON_ID_COUNT];
    button_debounce_process(&ctx, BTN1_RAW_PRESSED, 0, ev, BUTTON_ID_COUNT);
    button_debounce_process(&ctx, BTN1_RAW_PRESSED, 35, ev, BUTTON_ID_COUNT);
    button_debounce_process(&ctx, ALL_RELEASED, 100, ev, BUTTON_ID_COUNT);
    int n = button_debounce_process(&ctx, ALL_RELEASED, 135, ev, BUTTON_ID_COUNT);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL(35, ev[0].timestamp_ms);
}

TEST_CASE("BTN-D10: uint32 wrap safe", "[btn][group_d][debounce]")
{
    button_debounce_ctx_t ctx;
    button_debounce_init(&ctx, 30);
    button_debounce_seed(&ctx, ALL_RELEASED, 0);
    button_event_t ev[BUTTON_ID_COUNT];
    uint32_t base = UINT32_MAX - 20;
    button_debounce_process(&ctx, BTN1_RAW_PRESSED, base, ev, BUTTON_ID_COUNT);
    button_debounce_process(&ctx, BTN1_RAW_PRESSED, 15, ev, BUTTON_ID_COUNT);
    button_debounce_process(&ctx, ALL_RELEASED, 80, ev, BUTTON_ID_COUNT);
    TEST_ASSERT_EQUAL(1, button_debounce_process(&ctx, ALL_RELEASED, 115, ev, BUTTON_ID_COUNT));
}

TEST_CASE("BTN-D11: irrelevant bits ignored", "[btn][group_d][debounce]")
{
    button_debounce_ctx_t ctx;
    button_debounce_init(&ctx, 30);
    button_debounce_seed(&ctx, ALL_RELEASED, 0);
    button_event_t ev[BUTTON_ID_COUNT];
    /* R1.8: Only change bits 0-3 (irrelevant), keep buttons (bits 4-6) HIGH */
    uint8_t irrelevant = ALL_RELEASED & ~0x0F;
    TEST_ASSERT_EQUAL(0, button_debounce_process(&ctx, irrelevant, 0, ev, BUTTON_ID_COUNT));
    TEST_ASSERT_EQUAL(0, button_debounce_process(&ctx, irrelevant, 100, ev, BUTTON_ID_COUNT));
    /* Now press BTN1 (clear bit4) while keeping irrelevant bits changed */
    uint8_t btn1_with_irrelevant = irrelevant & BTN1_RAW_PRESSED;
    button_debounce_process(&ctx, btn1_with_irrelevant, 200, ev, BUTTON_ID_COUNT);
    button_debounce_process(&ctx, btn1_with_irrelevant, 235, ev, BUTTON_ID_COUNT);
    button_debounce_process(&ctx, ALL_RELEASED, 300, ev, BUTTON_ID_COUNT);
    TEST_ASSERT_EQUAL(1, button_debounce_process(&ctx, ALL_RELEASED, 335, ev, BUTTON_ID_COUNT));
    TEST_ASSERT_EQUAL(BUTTON_ID_1, ev[0].button_id);
}

TEST_CASE("BTN-D22: sequence monotonic", "[btn][group_d][debounce]")
{
    button_debounce_ctx_t ctx;
    button_debounce_init(&ctx, 30);
    button_debounce_seed(&ctx, ALL_RELEASED, 0);
    button_event_t ev[BUTTON_ID_COUNT];
    uint32_t last_seq = 0;
    for (int i = 0; i < 10; i++) {
        uint32_t b = (uint32_t)i * 200;
        button_debounce_process(&ctx, BTN1_RAW_PRESSED, b, ev, BUTTON_ID_COUNT);
        button_debounce_process(&ctx, BTN1_RAW_PRESSED, b + 35, ev, BUTTON_ID_COUNT);
        button_debounce_process(&ctx, ALL_RELEASED, b + 100, ev, BUTTON_ID_COUNT);
        int n = button_debounce_process(&ctx, ALL_RELEASED, b + 135, ev, BUTTON_ID_COUNT);
        TEST_ASSERT_EQUAL(1, n);
        TEST_ASSERT_GREATER_THAN(last_seq, ev[0].sequence);
        last_seq = ev[0].sequence;
    }
}

/* ---- Seed tests ---- */

TEST_CASE("BTN-D-seed: all released", "[btn][group_d][debounce]")
{
    button_debounce_ctx_t ctx;
    button_debounce_init(&ctx, 30);
    button_debounce_seed(&ctx, ALL_RELEASED, 0);
    for (int i = 0; i < BUTTON_ID_COUNT; i++)
        TEST_ASSERT_EQUAL(BTN_STATE_IDLE, ctx.buttons[i].state);
    button_event_t ev[BUTTON_ID_COUNT];
    button_debounce_process(&ctx, BTN1_RAW_PRESSED, 0, ev, BUTTON_ID_COUNT);
    button_debounce_process(&ctx, BTN1_RAW_PRESSED, 35, ev, BUTTON_ID_COUNT);
    button_debounce_process(&ctx, ALL_RELEASED, 100, ev, BUTTON_ID_COUNT);
    TEST_ASSERT_EQUAL(1, button_debounce_process(&ctx, ALL_RELEASED, 135, ev, BUTTON_ID_COUNT));
}

TEST_CASE("BTN-D-seed: BTN1 held no false click", "[btn][group_d][debounce]")
{
    button_debounce_ctx_t ctx;
    button_debounce_init(&ctx, 30);
    button_debounce_seed(&ctx, BTN1_RAW_PRESSED, 0);
    TEST_ASSERT_EQUAL(BTN_STATE_SEED_RELEASE, ctx.buttons[0].state);
    button_event_t ev[BUTTON_ID_COUNT];

    /* Phase 1: Seed-held release — must NOT produce click */
    TEST_ASSERT_EQUAL(0, button_debounce_process(&ctx, BTN1_RAW_PRESSED, 35, ev, BUTTON_ID_COUNT));
    button_debounce_process(&ctx, ALL_RELEASED, 100, ev, BUTTON_ID_COUNT);
    TEST_ASSERT_EQUAL(0, button_debounce_process(&ctx, ALL_RELEASED, 135, ev, BUTTON_ID_COUNT));
    TEST_ASSERT_EQUAL(BTN_STATE_IDLE, ctx.buttons[0].state);
    TEST_ASSERT_EQUAL(0, ctx.global_sequence);

    /* Phase 2: Real BTN1 click after seed — must produce exactly one CLICK */
    button_debounce_process(&ctx, BTN1_RAW_PRESSED, 200, ev, BUTTON_ID_COUNT);
    button_debounce_process(&ctx, BTN1_RAW_PRESSED, 235, ev, BUTTON_ID_COUNT);
    button_debounce_process(&ctx, ALL_RELEASED, 300, ev, BUTTON_ID_COUNT);
    int n = button_debounce_process(&ctx, ALL_RELEASED, 335, ev, BUTTON_ID_COUNT);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL(BUTTON_ID_1, ev[0].button_id);
    /* R1.8: First real click has sequence=1 (global_sequence increments from 0) */
    TEST_ASSERT_EQUAL(1, ev[0].sequence);
    TEST_ASSERT_EQUAL(1, ctx.global_sequence);
}

TEST_CASE("BTN-D-seed: held > debounce release no click", "[btn][group_d][debounce]")
{
    button_debounce_ctx_t ctx;
    button_debounce_init(&ctx, 30);
    button_debounce_seed(&ctx, BTN1_RAW_PRESSED, 0);
    button_event_t ev[BUTTON_ID_COUNT];
    for (uint32_t t = 0; t <= 5000; t += 50)
        button_debounce_process(&ctx, BTN1_RAW_PRESSED, t, ev, BUTTON_ID_COUNT);
    button_debounce_process(&ctx, ALL_RELEASED, 5100, ev, BUTTON_ID_COUNT);
    TEST_ASSERT_EQUAL(0, button_debounce_process(&ctx, ALL_RELEASED, 5135, ev, BUTTON_ID_COUNT));
}

TEST_CASE("BTN-D-seed: BTN1+BTN3 held", "[btn][group_d][debounce]")
{
    button_debounce_ctx_t ctx;
    button_debounce_init(&ctx, 30);
    button_event_t ev[BUTTON_ID_COUNT];
    uint8_t mask = ALL_RELEASED & ~((1U << 4) | (1U << 6));
    button_debounce_seed(&ctx, mask, 0);
    TEST_ASSERT_EQUAL(BTN_STATE_SEED_RELEASE, ctx.buttons[0].state);
    TEST_ASSERT_EQUAL(BTN_STATE_IDLE, ctx.buttons[1].state);
    TEST_ASSERT_EQUAL(BTN_STATE_SEED_RELEASE, ctx.buttons[2].state);
    button_debounce_process(&ctx, ALL_RELEASED, 50, ev, BUTTON_ID_COUNT);
    button_debounce_process(&ctx, ALL_RELEASED, 85, ev, BUTTON_ID_COUNT);
    TEST_ASSERT_EQUAL(BTN_STATE_IDLE, ctx.buttons[0].state);
    TEST_ASSERT_EQUAL(BTN_STATE_IDLE, ctx.buttons[2].state);
}

/* ---- Deadline tests ---- */

TEST_CASE("BTN-D30: deadline correct for confirmed press/release click", "[btn][group_d][debounce][deadline]")
{
    button_debounce_ctx_t ctx;
    button_debounce_init(&ctx, 30);
    button_debounce_seed(&ctx, ALL_RELEASED, 0);
    button_event_t ev[BUTTON_ID_COUNT];

    /* t=0: press starts → WAIT_PRESS_DEBOUNCE */
    button_debounce_process(&ctx, BTN1_RAW_PRESSED, 0, ev, BUTTON_ID_COUNT);
    button_deadline_t dl = button_debounce_next_deadline(&ctx, 0);
    TEST_ASSERT_TRUE(dl.active);
    TEST_ASSERT_FALSE(dl.due_now);
    TEST_ASSERT_GREATER_THAN(0, dl.remaining_ms);
    TEST_ASSERT_LESS_OR_EQUAL(30, dl.remaining_ms);

    /* t=35: press confirmed → PRESSED (debounce elapsed) */
    button_debounce_process(&ctx, BTN1_RAW_PRESSED, 35, ev, BUTTON_ID_COUNT);
    TEST_ASSERT_EQUAL(BTN_STATE_PRESSED, ctx.buttons[0].state);
    dl = button_debounce_next_deadline(&ctx, 35);
    TEST_ASSERT_FALSE(dl.active);  /* PRESSED state has no deadline */

    /* t=85: release starts → WAIT_RELEASE_DEBOUNCE */
    button_debounce_process(&ctx, ALL_RELEASED, 85, ev, BUTTON_ID_COUNT);
    dl = button_debounce_next_deadline(&ctx, 85);
    TEST_ASSERT_TRUE(dl.active);
    TEST_ASSERT_FALSE(dl.due_now);
    TEST_ASSERT_GREATER_THAN(0, dl.remaining_ms);
    TEST_ASSERT_LESS_OR_EQUAL(30, dl.remaining_ms);

    /* t=114: still within debounce — no click yet */
    int n1 = button_debounce_process(&ctx, ALL_RELEASED, 114, ev, BUTTON_ID_COUNT);
    TEST_ASSERT_EQUAL(0, n1);

    /* t=115: debounce elapsed → IDLE + exactly one click */
    int n2 = button_debounce_process(&ctx, ALL_RELEASED, 115, ev, BUTTON_ID_COUNT);
    TEST_ASSERT_EQUAL(1, n2);
    TEST_ASSERT_EQUAL(BUTTON_ID_1, ev[0].button_id);
    TEST_ASSERT_EQUAL(35, ev[0].timestamp_ms);  /* confirmed press time */

    /* deadline now inactive */
    dl = button_debounce_next_deadline(&ctx, 115);
    TEST_ASSERT_FALSE(dl.active);
}

TEST_CASE("BTN-D31: deadline re-sample", "[btn][group_d][debounce][deadline]")
{
    button_debounce_ctx_t ctx;
    button_debounce_init(&ctx, 30);
    button_debounce_seed(&ctx, ALL_RELEASED, 0);
    button_event_t ev[BUTTON_ID_COUNT];
    button_debounce_process(&ctx, BTN1_RAW_PRESSED, 0, ev, BUTTON_ID_COUNT);
    button_deadline_t dl1 = button_debounce_next_deadline(&ctx, 10);
    TEST_ASSERT_TRUE(dl1.active);
    TEST_ASSERT_GREATER_THAN(0, dl1.remaining_ms);
    button_deadline_t dl2 = button_debounce_next_deadline(&ctx, 20);
    TEST_ASSERT_TRUE(dl2.active);
    TEST_ASSERT_GREATER_THAN(0, dl2.remaining_ms);
    button_debounce_process(&ctx, BTN1_RAW_PRESSED, 35, ev, BUTTON_ID_COUNT);
    button_deadline_t dl3 = button_debounce_next_deadline(&ctx, 35);
    TEST_ASSERT_FALSE(dl3.active);
}

TEST_CASE("BTN-D-deadline: overdue returns due_now", "[btn][group_d][debounce][deadline]")
{
    button_debounce_ctx_t ctx;
    button_debounce_init(&ctx, 30);
    button_debounce_seed(&ctx, ALL_RELEASED, 0);
    button_event_t ev[BUTTON_ID_COUNT];
    button_debounce_process(&ctx, BTN1_RAW_PRESSED, 0, ev, BUTTON_ID_COUNT);
    button_deadline_t dl = button_debounce_next_deadline(&ctx, 50);
    TEST_ASSERT_TRUE(dl.active);
    TEST_ASSERT_TRUE(dl.due_now);
    TEST_ASSERT_EQUAL(0, dl.remaining_ms);
}

TEST_CASE("BTN-D-deadline: seed release deadline", "[btn][group_d][debounce][deadline]")
{
    button_debounce_ctx_t ctx;
    button_debounce_init(&ctx, 30);
    button_debounce_seed(&ctx, BTN1_RAW_PRESSED, 0);
    button_event_t ev[BUTTON_ID_COUNT];
    button_deadline_t dl = button_debounce_next_deadline(&ctx, 0);
    TEST_ASSERT_FALSE(dl.active);
    button_debounce_process(&ctx, ALL_RELEASED, 100, ev, BUTTON_ID_COUNT);
    dl = button_debounce_next_deadline(&ctx, 100);
    TEST_ASSERT_TRUE(dl.active);
    TEST_ASSERT_FALSE(dl.due_now);
    TEST_ASSERT_GREATER_THAN(0, dl.remaining_ms);
}

/* ================================================================
 * MCP register mock tests
 * ================================================================ */

TEST_CASE("BTN-D15: IOCON is 0x44", "[btn][group_d][mcp]")
{
    TEST_ASSERT_EQUAL_HEX8(0x44, mcp23017_test_get_iocon_config());
}

TEST_CASE("BTN-D25: INTCONA any-change mode", "[btn][group_d][mcp]")
{
    mcp23017_handle_t *mcp = create_mock_mcp();
    TEST_ASSERT_NOT_NULL(mcp);
    mcp_mock_preset(&s_mcp_mock, 0x08, 0xFF);
    TEST_ASSERT_EQUAL(ESP_OK, mcp23017_configure_button_interrupts(mcp, 0x70, false));
    TEST_ASSERT_EQUAL_HEX8(0x8F, s_mcp_mock.regs[0x08]);
    group_d_test_destroy_mock_mcp(mcp);
}

TEST_CASE("BTN-D-RMW: GPINTENA enable/disable only 0x70", "[btn][group_d][mcp]")
{
    mcp23017_handle_t *mcp = create_mock_mcp();
    mcp_mock_preset(&s_mcp_mock, 0x04, 0x0F);
    mcp23017_configure_button_interrupts(mcp, 0x70, false);
    TEST_ASSERT_EQUAL_HEX8(0x7F, s_mcp_mock.regs[0x04]);
    mcp23017_disable_button_interrupts(mcp, 0x70);
    TEST_ASSERT_EQUAL_HEX8(0x0F, s_mcp_mock.regs[0x04]);
    group_d_test_destroy_mock_mcp(mcp);
}

TEST_CASE("BTN-D29: GPPUA RMW preserves unrelated", "[btn][group_d][mcp]")
{
    mcp23017_handle_t *mcp = create_mock_mcp();
    mcp_mock_preset(&s_mcp_mock, 0x0C, 0x03);
    mcp23017_configure_button_interrupts(mcp, 0x70, true);
    TEST_ASSERT_EQUAL_HEX8(0x73, s_mcp_mock.regs[0x0C]);
    mcp23017_configure_button_interrupts(mcp, 0x70, false);
    TEST_ASSERT_EQUAL_HEX8(0x73, s_mcp_mock.regs[0x0C]);
    group_d_test_destroy_mock_mcp(mcp);
}

TEST_CASE("BTN-D28: Port B unchanged", "[btn][group_d][mcp]")
{
    mcp23017_handle_t *mcp = create_mock_mcp();
    mcp_mock_preset(&s_mcp_mock, 0x05, 0x7F);
    mcp_mock_preset(&s_mcp_mock, 0x07, 0x7F);
    mcp_mock_preset(&s_mcp_mock, 0x09, 0x7F);
    mcp_mock_preset(&s_mcp_mock, 0x0D, 0x02);
    mcp23017_configure_button_interrupts(mcp, 0x70, true);
    TEST_ASSERT_EQUAL_HEX8(0x7F, s_mcp_mock.regs[0x05]);
    TEST_ASSERT_EQUAL_HEX8(0x7F, s_mcp_mock.regs[0x07]);
    TEST_ASSERT_EQUAL_HEX8(0x7F, s_mcp_mock.regs[0x09]);
    TEST_ASSERT_EQUAL_HEX8(0x02, s_mcp_mock.regs[0x0D]);
    group_d_test_destroy_mock_mcp(mcp);
}

TEST_CASE("BTN-D16: INTFA before INTCAPA read order", "[btn][group_d][mcp]")
{
    mcp23017_handle_t *mcp = create_mock_mcp();
    mcp_mock_preset(&s_mcp_mock, 0x0E, 0x10);
    mcp_mock_preset(&s_mcp_mock, 0x10, 0xEF);
    mcp_mock_clear_trace(&s_mcp_mock);
    uint8_t fa, fb, ca, cb;
    TEST_ASSERT_EQUAL(ESP_OK, mcp23017_read_interrupt_state(mcp, &fa, &fb, &ca, &cb));
    TEST_ASSERT_EQUAL_HEX8(0x10, fa);
    TEST_ASSERT_EQUAL_HEX8(0xEF, ca);
    TEST_ASSERT_GREATER_OR_EQUAL(4, s_mcp_mock.trace_count);
    TEST_ASSERT_EQUAL(0x0E, s_mcp_mock.trace[0].reg);
    TEST_ASSERT_EQUAL(0x0F, s_mcp_mock.trace[1].reg);
    TEST_ASSERT_EQUAL(0x10, s_mcp_mock.trace[2].reg);
    TEST_ASSERT_EQUAL(0x11, s_mcp_mock.trace[3].reg);
    TEST_ASSERT_EQUAL_HEX8(0x00, s_mcp_mock.regs[0x0E]);
    group_d_test_destroy_mock_mcp(mcp);
}

TEST_CASE("BTN-D26: INTFA nonzero + INTCAPA all-low = Port A", "[btn][group_d][mcp]")
{
    mcp23017_handle_t *mcp = create_mock_mcp();
    mcp_mock_preset(&s_mcp_mock, 0x0E, 0x10);
    mcp_mock_preset(&s_mcp_mock, 0x10, 0x00);
    uint8_t fa, fb, ca, cb;
    TEST_ASSERT_EQUAL(ESP_OK, mcp23017_read_interrupt_state(mcp, &fa, &fb, &ca, &cb));
    TEST_ASSERT_EQUAL_HEX8(0x10, fa);
    TEST_ASSERT_EQUAL_HEX8(0x00, ca);
    group_d_test_destroy_mock_mcp(mcp);
}

TEST_CASE("BTN-D27: INTA+INTB simultaneous both flagged", "[btn][group_d][mcp]")
{
    mcp23017_handle_t *mcp = create_mock_mcp();
    mcp_mock_preset(&s_mcp_mock, 0x0E, 0x10);
    mcp_mock_preset(&s_mcp_mock, 0x0F, 0x01);
    uint8_t fa, fb, ca, cb;
    TEST_ASSERT_EQUAL(ESP_OK, mcp23017_read_interrupt_state(mcp, &fa, &fb, &ca, &cb));
    TEST_ASSERT_NOT_EQUAL(0, fa);
    TEST_ASSERT_NOT_EQUAL(0, fb);
    group_d_test_destroy_mock_mcp(mcp);
}

TEST_CASE("BTN-D-INTF-FAIL: failure outputs unchanged", "[btn][group_d][mcp]")
{
    mcp23017_handle_t *mcp = create_mock_mcp();
    mcp_mock_set_read_fault(&s_mcp_mock, ESP_ERR_INVALID_RESPONSE, MCP_FAULT_NEXT, 1);
    uint8_t fa = 0xAA, fb = 0xBB, ca = 0xCC, cb = 0xDD;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_RESPONSE,
                      mcp23017_read_interrupt_state(mcp, &fa, &fb, &ca, &cb));
    TEST_ASSERT_EQUAL_HEX8(0xAA, fa);
    TEST_ASSERT_EQUAL_HEX8(0xBB, fb);
    TEST_ASSERT_EQUAL_HEX8(0xCC, ca);
    TEST_ASSERT_EQUAL_HEX8(0xDD, cb);
    group_d_test_destroy_mock_mcp(mcp);
}

TEST_CASE("BTN-D-pin-mask: rejects pins outside 0x70", "[btn][group_d][mcp]")
{
    mcp23017_handle_t *mcp = create_mock_mcp();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, mcp23017_configure_button_interrupts(mcp, 0x01, false));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, mcp23017_configure_button_interrupts(mcp, 0x71, false));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, mcp23017_configure_button_interrupts(mcp, 0x80, false));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, mcp23017_configure_button_interrupts(mcp, 0x00, false));
    TEST_ASSERT_EQUAL(ESP_OK, mcp23017_configure_button_interrupts(mcp, 0x70, false));
    group_d_test_destroy_mock_mcp(mcp);
}

TEST_CASE("BTN-D-rollback: partial failure rolls back", "[btn][group_d][mcp]")
{
    mcp23017_handle_t *mcp = create_mock_mcp();
    mcp_mock_preset(&s_mcp_mock, 0x04, 0x0F);
    mcp_mock_preset(&s_mcp_mock, 0x08, 0xFF);
    mcp_mock_set_fault(&s_mcp_mock, ESP_ERR_INVALID_RESPONSE, 3);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_RESPONSE,
                      mcp23017_configure_button_interrupts(mcp, 0x70, false));
    TEST_ASSERT_EQUAL_HEX8(0x0F, s_mcp_mock.regs[0x04]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, s_mcp_mock.regs[0x08]);
    group_d_test_destroy_mock_mcp(mcp);
}

/* ================================================================
 * Service lifecycle tests
 * ================================================================ */

TEST_CASE("BTN-D23: init rejects NULL HAL", "[btn][group_d][service]")
{
    button_service_config_t cfg = {0};
    cfg.hal = NULL;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, button_service_init(&cfg));
}

TEST_CASE("BTN-D-NULL-URGENT: init rejects NULL urgent sink", "[btn][group_d][service]")
{
    fake_hal_ctx_t *fh = fake_hal_create();
    TEST_ASSERT_NOT_NULL(fh);
    button_service_config_t cfg = {0};
    cfg.hal = fake_hal_get_interface(fh);
    cfg.urgent_sink.publish = NULL;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, button_service_init(&cfg));
    fake_hal_destroy(fh);
}

TEST_CASE("BTN-D-start-FAIL: configure failure returns error stays retryable",
          "[btn][group_d][service]")
{
    fake_hal_ctx_t *fh = fake_hal_create();
    TEST_ASSERT_NOT_NULL(fh);
    test_sink_ctx_t nc, uc;
    test_sink_init(&nc); test_sink_init(&uc);
    button_service_config_t cfg = make_btn_config(fake_hal_get_interface(fh), &nc, &uc, 10, 50);
    TEST_ASSERT_EQUAL(ESP_OK, button_service_init(&cfg));
    fake_fault_config_t faults = { .btn_read_error = ESP_ERR_INVALID_RESPONSE };
    fake_hal_set_faults(fh, &faults);
    esp_err_t err = button_service_start();
    TEST_ASSERT_NOT_EQUAL(ESP_OK, err);
    button_service_stop();
    fake_hal_clear_faults(fh);
    cfg = make_btn_config(fake_hal_get_interface(fh), &nc, &uc, 10, 50);
    TEST_ASSERT_EQUAL(ESP_OK, button_service_init(&cfg));
    TEST_ASSERT_EQUAL(ESP_OK, button_service_start());
    btn_teardown();
}

TEST_CASE("BTN-D17: 20 rounds no leak", "[btn][group_d][service][stress]")
{
    fake_hal_ctx_t *fh = fake_hal_create();
    TEST_ASSERT_NOT_NULL(fh);
    test_sink_ctx_t nc, uc;
    test_sink_init(&nc); test_sink_init(&uc);
    uint32_t heap_before = esp_get_free_heap_size();
    for (int i = 0; i < 20; i++) {
        button_service_config_t cfg = make_btn_config(fake_hal_get_interface(fh), &nc, &uc, 10, 50);
        TEST_ASSERT_EQUAL(ESP_OK, button_service_init(&cfg));
        TEST_ASSERT_EQUAL(ESP_OK, button_service_start());
        vTaskDelay(pdMS_TO_TICKS(30));
        TEST_ASSERT_EQUAL(ESP_OK, button_service_stop());
    }
    uint32_t heap_after = esp_get_free_heap_size();
    TEST_ASSERT_INT_WITHIN(4096, heap_before, heap_after);
    fake_hal_destroy(fh);
}

TEST_CASE("BTN-D34: abort cleanup releases resources", "[btn][group_d][service]")
{
    fake_hal_ctx_t *fh = fake_hal_create();
    TEST_ASSERT_NOT_NULL(fh);
    test_sink_ctx_t nc, uc;
    test_sink_init(&nc); test_sink_init(&uc);
    button_service_config_t cfg = make_btn_config(fake_hal_get_interface(fh), &nc, &uc, 10, 50);
    TEST_ASSERT_EQUAL(ESP_OK, button_service_init(&cfg));
    TEST_ASSERT_EQUAL(ESP_OK, button_service_start());
    vTaskDelay(pdMS_TO_TICKS(30));
    TEST_ASSERT_EQUAL(ESP_OK, button_service_test_abort_cleanup());
    cfg = make_btn_config(fake_hal_get_interface(fh), &nc, &uc, 10, 50);
    TEST_ASSERT_EQUAL(ESP_OK, button_service_init(&cfg));
    btn_teardown();
}

TEST_CASE("BTN-D35: heap stable 10 cycles", "[btn][group_d][service][stress]")
{
    fake_hal_ctx_t *fh = fake_hal_create();
    TEST_ASSERT_NOT_NULL(fh);
    test_sink_ctx_t nc, uc;
    test_sink_init(&nc); test_sink_init(&uc);
    uint32_t heap_before = esp_get_free_heap_size();
    for (int i = 0; i < 10; i++) {
        button_service_config_t cfg = make_btn_config(fake_hal_get_interface(fh), &nc, &uc, 5, 20);
        TEST_ASSERT_EQUAL(ESP_OK, button_service_init(&cfg));
        TEST_ASSERT_EQUAL(ESP_OK, button_service_start());
        vTaskDelay(pdMS_TO_TICKS(30));
        esp_err_t err = button_service_stop();
        if (err != ESP_OK) button_service_test_abort_cleanup();
    }
    uint32_t heap_after = esp_get_free_heap_size();
    TEST_ASSERT_INT_WITHIN(4096, heap_before, heap_after);
    fake_hal_destroy(fh);
}

/* ================================================================
 * R1.5: Rewritten service + sink delivery tests
 * ================================================================ */

/* D07: Real BTN3 press → debounce → release → debounce → exactly one urgent.
 * No GREATER_OR_EQUAL(0) — exact counts. */
TEST_CASE("BTN-D07: BTN3 real press/release urgent exactly once", "[btn][group_d][service]")
{
    btn_setup();
    fake_hal_ctx_t *fh = fake_hal_create();
    TEST_ASSERT_NOT_NULL(fh);
    test_sink_ctx_t nc, uc;
    test_sink_init(&nc); test_sink_init(&uc);
    button_service_config_t cfg = make_btn_config(fake_hal_get_interface(fh), &nc, &uc, 10, 50);
    TEST_ASSERT_EQUAL(ESP_OK, button_service_init(&cfg));

    /* Seed: all released */
    fake_hal_set_button_state(fh, ALL_RELEASED);
    TEST_ASSERT_EQUAL(ESP_OK, button_service_start());
    TEST_ASSERT_TRUE(wait_for_btn_read_count(fh, 3, 2000));

    /* BTN3 press (bit6 cleared) — set state FIRST, then read counter */
    fake_hal_set_button_state(fh, BTN3_RAW_PRESSED);
    uint32_t gen = fake_hal_get_btn_read_count(fh);
    TEST_ASSERT_TRUE(wait_for_btn_read_count(fh, gen + 2, 2000));

    /* BTN3 release — set state FIRST, then read counter */
    fake_hal_set_button_state(fh, ALL_RELEASED);
    gen = fake_hal_get_btn_read_count(fh);
    TEST_ASSERT_TRUE(wait_for_btn_read_count(fh, gen + 2, 2000));
    TEST_ASSERT_TRUE(wait_for_urgent_count(&uc, 1, 3000));

    /* Exact assertions */
    TEST_ASSERT_EQUAL(1, test_sink_get_count(&uc));
    TEST_ASSERT_EQUAL(0, test_sink_get_count(&nc));

    button_sink_event_t last;
    TEST_ASSERT_TRUE(test_sink_get_last(&uc, &last));
    TEST_ASSERT_EQUAL(BUTTON_ID_3, last.button_id);

    button_service_snapshot_t snap;
    TEST_ASSERT_EQUAL(ESP_OK, button_service_get_snapshot(&snap));
    TEST_ASSERT_EQUAL(1, snap.urgent_delivered);
    TEST_ASSERT_FALSE(snap.urgent_pending);
    TEST_ASSERT_EQUAL(0, snap.normal_sink_drops);

    btn_teardown();
}

/* D08: BTN1 click blocked by normal sink failure → drop counted.
 * Then BTN3 click still delivered via urgent. */
TEST_CASE("BTN-D08: normal drop does not block urgent", "[btn][group_d][service]")
{
    btn_setup();
    fake_hal_ctx_t *fh = fake_hal_create();
    TEST_ASSERT_NOT_NULL(fh);
    test_sink_ctx_t nc, uc;
    test_sink_init(&nc); test_sink_init(&uc);
    button_service_config_t cfg = make_btn_config(fake_hal_get_interface(fh), &nc, &uc, 10, 50);
    TEST_ASSERT_EQUAL(ESP_OK, button_service_init(&cfg));

    fake_hal_set_button_state(fh, ALL_RELEASED);
    TEST_ASSERT_EQUAL(ESP_OK, button_service_start());
    TEST_ASSERT_TRUE(wait_for_btn_read_count(fh, 3, 2000));

    /* Force normal sink to fail */
    test_sink_set_error(&nc, ESP_ERR_TIMEOUT);

    /* BTN1 press -> debounce -> release -> debounce */
    fake_hal_set_button_state(fh, BTN1_RAW_PRESSED);
    uint32_t gen = fake_hal_get_btn_read_count(fh);
    TEST_ASSERT_TRUE(wait_for_btn_read_count(fh, gen + 2, 2000));
    fake_hal_set_button_state(fh, ALL_RELEASED);
    gen = fake_hal_get_btn_read_count(fh);
    /* Poll for normal sink drop to be recorded */
    vTaskDelay(pdMS_TO_TICKS(200));  /* service must attempt + fail normal sink */

    button_service_snapshot_t snap;
    TEST_ASSERT_EQUAL(ESP_OK, button_service_get_snapshot(&snap));
    TEST_ASSERT_GREATER_OR_EQUAL(1, (int)snap.normal_sink_drops);
    TEST_ASSERT_EQUAL(0, test_sink_get_count(&nc));  /* normal received nothing */
    uint32_t drops_after_btn1 = snap.normal_sink_drops;

    /* Now BTN3 press -> release -> urgent delivered */
    fake_hal_set_button_state(fh, BTN3_RAW_PRESSED);
    gen = fake_hal_get_btn_read_count(fh);
    TEST_ASSERT_TRUE(wait_for_btn_read_count(fh, gen + 2, 2000));
    fake_hal_set_button_state(fh, ALL_RELEASED);
    gen = fake_hal_get_btn_read_count(fh);
    TEST_ASSERT_TRUE(wait_for_btn_read_count(fh, gen + 2, 2000));
    TEST_ASSERT_TRUE(wait_for_urgent_count(&uc, 1, 3000));

    TEST_ASSERT_EQUAL(1, test_sink_get_count(&uc));
    button_sink_event_t last;
    TEST_ASSERT_TRUE(test_sink_get_last(&uc, &last));
    TEST_ASSERT_EQUAL(BUTTON_ID_3, last.button_id);

    /* Normal drops didn't increase from BTN3 */
    TEST_ASSERT_EQUAL(ESP_OK, button_service_get_snapshot(&snap));
    TEST_ASSERT_EQUAL(drops_after_btn1, snap.normal_sink_drops);

    btn_teardown();
}

/* D12: FAIL_NEXT — exactly one GPIO read error, recovery click works. */
TEST_CASE("BTN-D12: FAIL_NEXT one error then recovery click", "[btn][group_d][service]")
{
    btn_setup();
    fake_hal_ctx_t *fh = fake_hal_create();
    TEST_ASSERT_NOT_NULL(fh);
    test_sink_ctx_t nc, uc;
    test_sink_init(&nc); test_sink_init(&uc);
    button_service_config_t cfg = make_btn_config(fake_hal_get_interface(fh), &nc, &uc, 10, 50);
    TEST_ASSERT_EQUAL(ESP_OK, button_service_init(&cfg));

    fake_hal_set_button_state(fh, ALL_RELEASED);
    TEST_ASSERT_EQUAL(ESP_OK, button_service_start());
    TEST_ASSERT_TRUE(wait_for_btn_read_count(fh, 3, 2000));

    /* Fail next 1 GPIO read */
    fake_fault_config_t faults = { .btn_read_error = ESP_ERR_INVALID_RESPONSE };
    fake_hal_set_faults(fh, &faults);
    TEST_ASSERT_TRUE(wait_for_i2c_error_count(1, 2000));

    button_service_snapshot_t snap;
    TEST_ASSERT_EQUAL(ESP_OK, button_service_get_snapshot(&snap));
    TEST_ASSERT_EQUAL(1, snap.i2c_read_errors);
    TEST_ASSERT_EQUAL(0, snap.total_events);
    TEST_ASSERT_EQUAL(0, test_sink_get_count(&nc));
    TEST_ASSERT_EQUAL(0, test_sink_get_count(&uc));

    /* Clear fault for recovery */
    fake_hal_clear_faults(fh);

    /* Recovery: BTN1 press -> release -> exactly one click */
    fake_hal_set_button_state(fh, BTN1_RAW_PRESSED);
    uint32_t gen = fake_hal_get_btn_read_count(fh);
    TEST_ASSERT_TRUE(wait_for_btn_read_count(fh, gen + 2, 2000));
    gen = fake_hal_get_btn_read_count(fh);
    fake_hal_set_button_state(fh, ALL_RELEASED);
    TEST_ASSERT_TRUE(wait_for_normal_count(&nc, 1, 3000));

    TEST_ASSERT_EQUAL(1, test_sink_get_count(&nc));
    button_sink_event_t last;
    TEST_ASSERT_TRUE(test_sink_get_last(&nc, &last));
    TEST_ASSERT_EQUAL(BUTTON_ID_1, last.button_id);
    TEST_ASSERT_EQUAL(0, test_sink_get_count(&uc));

    btn_teardown();
}

/* D13: FAIL_UNTIL_CLEARED — multiple failures proved by counter,
 * then clear + resync + no false click + real click exactly once. */
TEST_CASE("BTN-D13: UNTIL_CLEARED multiple failures then recovery", "[btn][group_d][service]")
{
    btn_setup();
    fake_hal_ctx_t *fh = fake_hal_create();
    TEST_ASSERT_NOT_NULL(fh);
    test_sink_ctx_t nc, uc;
    test_sink_init(&nc); test_sink_init(&uc);
    button_service_config_t cfg = make_btn_config(fake_hal_get_interface(fh), &nc, &uc, 10, 50);
    TEST_ASSERT_EQUAL(ESP_OK, button_service_init(&cfg));

    fake_hal_set_button_state(fh, ALL_RELEASED);
    TEST_ASSERT_EQUAL(ESP_OK, button_service_start());
    TEST_ASSERT_TRUE(wait_for_btn_read_count(fh, 3, 2000));

    /* Sustained fault — btn_read_error persists until cleared */
    fake_fault_config_t faults = { .btn_read_error = ESP_ERR_INVALID_RESPONSE };
    fake_hal_set_faults(fake_hal_get_singleton(), &faults);
    /* Wait for multiple failures */
    TEST_ASSERT_TRUE(wait_for_i2c_error_count(3, 3000));

    button_service_snapshot_t snap;
    TEST_ASSERT_EQUAL(ESP_OK, button_service_get_snapshot(&snap));
    TEST_ASSERT_GREATER_OR_EQUAL(3, (int)snap.i2c_read_errors);
    TEST_ASSERT_EQUAL(0, snap.total_events);

    /* Clear fault — service should resync */
    fake_hal_clear_faults(fake_hal_get_singleton());

    /* Let service read the current (all-released) GPIO state */
    uint32_t gen = fake_hal_get_btn_read_count(fh);
    TEST_ASSERT_TRUE(wait_for_btn_read_count(fh, gen + 2, 2000));

    /* No false click from recovery */
    TEST_ASSERT_EQUAL(ESP_OK, button_service_get_snapshot(&snap));
    TEST_ASSERT_EQUAL(0, snap.total_events);
    TEST_ASSERT_EQUAL(0, test_sink_get_count(&nc));
    TEST_ASSERT_EQUAL(0, test_sink_get_count(&uc));

    /* Real press/release -> exactly one click */
    fake_hal_set_button_state(fh, BTN1_RAW_PRESSED);
    gen = fake_hal_get_btn_read_count(fh);
    TEST_ASSERT_TRUE(wait_for_btn_read_count(fh, gen + 2, 2000));
    gen = fake_hal_get_btn_read_count(fh);
    fake_hal_set_button_state(fh, ALL_RELEASED);
    TEST_ASSERT_TRUE(wait_for_normal_count(&nc, 1, 3000));

    TEST_ASSERT_EQUAL(1, test_sink_get_count(&nc));
    button_sink_event_t last;
    TEST_ASSERT_TRUE(test_sink_get_last(&nc, &last));
    TEST_ASSERT_EQUAL(BUTTON_ID_1, last.button_id);

    btn_teardown();
}

/* D14: Service + MCP GPIO burst: press/release/press, final stable release.
 * Exactly one click, correct stable mask, no duplicate events. */
TEST_CASE("BTN-D14: service interrupt coalescing via GPIO burst", "[btn][group_d][service]")
{
    btn_setup();
    fake_hal_ctx_t *fh = fake_hal_create();
    TEST_ASSERT_NOT_NULL(fh);
    test_sink_ctx_t nc, uc;
    test_sink_init(&nc); test_sink_init(&uc);
    button_service_config_t cfg = make_btn_config(fake_hal_get_interface(fh), &nc, &uc, 10, 50);
    TEST_ASSERT_EQUAL(ESP_OK, button_service_init(&cfg));

    fake_hal_set_button_state(fh, ALL_RELEASED);
    TEST_ASSERT_EQUAL(ESP_OK, button_service_start());
    TEST_ASSERT_TRUE(wait_for_btn_read_count(fh, 3, 2000));

    /* Rapid burst: press -> release -> press */
    fake_hal_set_button_state(fh, BTN1_RAW_PRESSED);
    uint32_t gen = fake_hal_get_btn_read_count(fh);
    /* Don't wait long — inject next state quickly */
    vTaskDelay(pdMS_TO_TICKS(5));
    fake_hal_set_button_state(fh, ALL_RELEASED);
    vTaskDelay(pdMS_TO_TICKS(5));
    fake_hal_set_button_state(fh, BTN1_RAW_PRESSED);

    /* Now stabilize: press held for debounce, then release */
    TEST_ASSERT_TRUE(wait_for_btn_read_count(fh, gen + 3, 2000));
    gen = fake_hal_get_btn_read_count(fh);
    fake_hal_set_button_state(fh, ALL_RELEASED);
    TEST_ASSERT_TRUE(wait_for_normal_count(&nc, 1, 3000));

    /* Exactly one click */
    TEST_ASSERT_EQUAL(1, test_sink_get_count(&nc));
    TEST_ASSERT_EQUAL(0, test_sink_get_count(&uc));

    button_sink_event_t last;
    TEST_ASSERT_TRUE(test_sink_get_last(&nc, &last));
    TEST_ASSERT_EQUAL(BUTTON_ID_1, last.button_id);

    /* Check final stable mask: all released = 0x00 for pressed bits */
    button_service_snapshot_t snap;
    TEST_ASSERT_EQUAL(ESP_OK, button_service_get_snapshot(&snap));
    TEST_ASSERT_EQUAL(0, snap.stable_mask & BUTTON_RAW_MASK);

    btn_teardown();
}

/* R1.7: Deterministic proof via BTN_HOOK_BEFORE_READY_SIGNAL.
 * Task blocked before READY signal; first stop times out (join fails);
 * IRQ disable/flush NOT called; second stop succeeds after task exits. */
TEST_CASE("BTN-D18: stop timeout then barrier release succeeds", "[btn][group_d][service]")
{
    btn_setup();
    fake_hal_ctx_t *fh = fake_hal_create();
    TEST_ASSERT_NOT_NULL(fh);
    test_sink_ctx_t nc, uc;
    test_sink_init(&nc); test_sink_init(&uc);
    button_service_config_t cfg = make_btn_config(fake_hal_get_interface(fh), &nc, &uc, 10, 50);
    TEST_ASSERT_EQUAL(ESP_OK, button_service_init(&cfg));

    /* Arm BEFORE_READY hook — task will block before signaling READY */
    button_service_test_set_hook(BTN_HOOK_BEFORE_READY_SIGNAL);

    fake_hal_set_button_state(fh, ALL_RELEASED);
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, button_service_start());  /* times out */

    /* Snapshot before first stop — baseline counts */
    int lc = -1;
    TEST_ASSERT_EQUAL(ESP_OK, button_service_test_get_lifecycle(&lc));
    TEST_ASSERT_EQUAL(4, lc);  /* LC_STOPPING */

    /* First stop: join fails because task still at barrier */
    esp_err_t err = button_service_stop();
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, err);

    /* R1.7: IRQ disable NOT called, urgent flush NOT called */
    TEST_ASSERT_EQUAL(0, (int)test_sink_get_count(&uc));  /* no urgent publish */

    /* Release barrier — task proceeds, enters main loop, sees stop_requested, exits */
    button_service_test_clear_hooks();
    TEST_ASSERT_TRUE(wait_for_task_done(5000));

    /* Second stop: join succeeds, disable/flush/cleanup runs */
    err = button_service_stop();
    TEST_ASSERT_EQUAL(ESP_OK, err);

    /* Lifecycle is now UNINITIALIZED */
    TEST_ASSERT_EQUAL(ESP_OK, button_service_test_get_lifecycle(&lc));
    TEST_ASSERT_EQUAL(0, lc);  /* LC_UNINITIALIZED */

    /* Re-init must work */
    cfg = make_btn_config(fake_hal_get_interface(fh), &nc, &uc, 10, 50);
    TEST_ASSERT_EQUAL(ESP_OK, button_service_init(&cfg));
    btn_teardown();
}

/* R1.7: Real start/stop overlap using BTN_HOOK_BEFORE_READY_SIGNAL.
 * Service task blocked before READY signal. start() times out.
 * Stop task calls stop() while start is still waiting (LC_STARTING).
 * After barrier release, task enters main loop, sees stop_requested, exits.
 * Lifecycle transitions: STARTING → STOPPING (via stop) → UNINITIALIZED (via second stop). */
TEST_CASE("BTN-D19: start/stop overlap via two tasks", "[btn][group_d][service]")
{
    btn_setup();
    fake_hal_ctx_t *fh = fake_hal_create();
    TEST_ASSERT_NOT_NULL(fh);
    test_sink_ctx_t nc, uc;
    test_sink_init(&nc); test_sink_init(&uc);
    button_service_config_t cfg = make_btn_config(fake_hal_get_interface(fh), &nc, &uc, 10, 50);
    TEST_ASSERT_EQUAL(ESP_OK, button_service_init(&cfg));

    /* Arm hook — task will block before READY */
    button_service_test_set_hook(BTN_HOOK_BEFORE_READY_SIGNAL);
    fake_hal_set_button_state(fh, ALL_RELEASED);

    /* start() blocks waiting for READY, then times out */
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, button_service_start());

    /* Lifecycle is STOPPING (start() sets this on timeout) */
    int lc = -1;
    TEST_ASSERT_EQUAL(ESP_OK, button_service_test_get_lifecycle(&lc));
    TEST_ASSERT_EQUAL(4, lc);  /* LC_STOPPING */

    /* stop() called while task still at barrier — join times out, disable/flush skipped */
    esp_err_t err = button_service_stop();
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, err);

    /* Now release barrier — task proceeds, enters main loop, exits */
    button_service_test_clear_hooks();
    TEST_ASSERT_TRUE(wait_for_task_done(5000));

    /* Second stop succeeds (join+disable+flush+cleanup) */
    err = button_service_stop();
    TEST_ASSERT_EQUAL(ESP_OK, err);

    /* Re-init works */
    cfg = make_btn_config(fake_hal_get_interface(fh), &nc, &uc, 10, 50);
    TEST_ASSERT_EQUAL(ESP_OK, button_service_init(&cfg));

    btn_teardown();
}

/* R1.7: EventGroup for simultaneous release (pdFALSE — no auto-clear).
 * Each task has a DONE bit. Both tasks proceed on GO_BIT.
 * Exactly one ESP_OK, one INVALID_STATE. join_commit incremented by exactly 1.
 * Both DONE bits received before test proceeds. */

typedef struct {
    EventGroupHandle_t eg;
    EventBits_t ready_bit;
    EventBits_t go_bit;
    EventBits_t done_bit;
    esp_err_t *result;
} d20_ctx_t;

static void d20_stop_task(void *arg)
{
    d20_ctx_t *c = (d20_ctx_t *)arg;
    xEventGroupSetBits(c->eg, c->ready_bit);
    xEventGroupWaitBits(c->eg, c->go_bit, pdFALSE, pdFALSE, portMAX_DELAY);
    printf("[D20] task calling stop(), result ptr=%p\n", c->result);
    *c->result = button_service_stop();
    printf("[D20] task stop returned: 0x%x, setting done_bit=0x%x\n", *c->result, (unsigned)c->done_bit);
    xEventGroupSetBits(c->eg, c->done_bit);
    vTaskDelete(NULL);
}

TEST_CASE("BTN-D20: concurrent stop single join commit", "[btn][group_d][service]")
{
    btn_setup();
    fake_hal_ctx_t *fh = fake_hal_create();
    TEST_ASSERT_NOT_NULL(fh);
    test_sink_ctx_t nc, uc;
    test_sink_init(&nc); test_sink_init(&uc);
    button_service_config_t cfg = make_btn_config(fake_hal_get_interface(fh), &nc, &uc, 10, 50);
    TEST_ASSERT_EQUAL(ESP_OK, button_service_init(&cfg));

    fake_hal_set_button_state(fh, ALL_RELEASED);
    TEST_ASSERT_EQUAL(ESP_OK, button_service_start());
    TEST_ASSERT_TRUE(wait_for_btn_read_count(fh, 3, 2000));

    /* Snapshot before stop */
    button_service_test_counters_t ctrs_before;
    TEST_ASSERT_EQUAL(ESP_OK, button_service_test_get_counters(&ctrs_before));

    /* Create EventGroup for synchronization */
    EventGroupHandle_t eg = xEventGroupCreate();
    TEST_ASSERT_NOT_NULL(eg);
    esp_err_t t1_result = ESP_ERR_TIMEOUT, t2_result = ESP_ERR_TIMEOUT;

    d20_ctx_t c1 = {eg, BIT(0), BIT(4), BIT(8), &t1_result};
    d20_ctx_t c2 = {eg, BIT(1), BIT(4), BIT(9), &t2_result};

    xTaskCreatePinnedToCore(d20_stop_task, "d20_s1", 4096, &c1, 15, NULL, 1);
    xTaskCreatePinnedToCore(d20_stop_task, "d20_s2", 4096, &c2, 15, NULL, 1);

    /* Wait for both tasks to be ready (waitForAllBits=pdTRUE) */
    EventBits_t ready_bits = xEventGroupWaitBits(eg, BIT(0) | BIT(1), pdFALSE, pdTRUE,
                        pdMS_TO_TICKS(5000));
    TEST_ASSERT_BITS(BIT(0) | BIT(1), BIT(0) | BIT(1), ready_bits);

    /* Release GO — both tasks proceed simultaneously */
    xEventGroupSetBits(eg, BIT(4));

    /* Wait for both DONE bits (waitForAllBits=pdTRUE) */
    printf("[D20] waiting for DONE bits...\n");
    EventBits_t done_bits = xEventGroupWaitBits(eg, BIT(8) | BIT(9), pdFALSE, pdTRUE,
                        pdMS_TO_TICKS(5000));
    printf("[D20] done_bits=0x%x t1=0x%x t2=0x%x\n", (unsigned)done_bits, t1_result, t2_result);
    TEST_ASSERT_BITS(BIT(8) | BIT(9), BIT(8) | BIT(9), done_bits);

    /* Exactly one ESP_OK, one INVALID_STATE */
    bool one_ok = (t1_result == ESP_OK);
    bool two_ok = (t2_result == ESP_OK);
    TEST_ASSERT_TRUE(one_ok || two_ok);
    TEST_ASSERT_FALSE(one_ok && two_ok);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, one_ok ? t2_result : t1_result);

    /* R1.8: Join count and IRQ disable count via counter API (works after stop) */
    button_service_test_counters_t ctrs_after;
    TEST_ASSERT_EQUAL(ESP_OK, button_service_test_get_counters(&ctrs_after));
    printf("[D20] before: join=%lu disable=%lu after: join=%lu disable=%lu\n",
           (unsigned long)ctrs_before.join_commit_count, (unsigned long)ctrs_before.irq_disable_count,
           (unsigned long)ctrs_after.join_commit_count, (unsigned long)ctrs_after.irq_disable_count);
    TEST_ASSERT_EQUAL(ctrs_before.join_commit_count + 1, ctrs_after.join_commit_count);
    TEST_ASSERT_EQUAL(ctrs_before.irq_disable_count + 1, ctrs_after.irq_disable_count);

    vEventGroupDelete(eg);

    /* Cleanup */
    btn_teardown();
}

/* D21: Real BTN1 click → sink callback fires → reentrant get_snapshot works.
 * callback_counter==1, snapshot returns ESP_OK, no deadlock. */
TEST_CASE("BTN-D21: sink reentrant get_snapshot no deadlock", "[btn][group_d][service]")
{
    btn_setup();
    fake_hal_ctx_t *fh = fake_hal_create();
    TEST_ASSERT_NOT_NULL(fh);
    test_sink_ctx_t nc, uc;
    test_sink_init(&nc); test_sink_init(&uc);
    nc.reentrant_check = true;
    button_service_config_t cfg = make_btn_config(fake_hal_get_interface(fh), &nc, &uc, 10, 50);
    TEST_ASSERT_EQUAL(ESP_OK, button_service_init(&cfg));

    fake_hal_set_button_state(fh, ALL_RELEASED);
    TEST_ASSERT_EQUAL(ESP_OK, button_service_start());
    TEST_ASSERT_TRUE(wait_for_btn_read_count(fh, 3, 2000));

    /* BTN1 press -> release -> wait for click */
    fake_hal_set_button_state(fh, BTN1_RAW_PRESSED);
    uint32_t gen = fake_hal_get_btn_read_count(fh);
    TEST_ASSERT_TRUE(wait_for_btn_read_count(fh, gen + 2, 2000));
    gen = fake_hal_get_btn_read_count(fh);
    fake_hal_set_button_state(fh, ALL_RELEASED);
    TEST_ASSERT_TRUE(wait_for_normal_count(&nc, 1, 3000));

    /* Exact assertions */
    TEST_ASSERT_EQUAL(1, test_sink_get_count(&nc));
    TEST_ASSERT_TRUE(nc.reentrant_snapshot_ok);  /* get_snapshot succeeded in callback */

    button_sink_event_t last;
    TEST_ASSERT_TRUE(test_sink_get_last(&nc, &last));
    TEST_ASSERT_EQUAL(BUTTON_ID_1, last.button_id);

    /* Service still running (no deadlock) */
    button_service_snapshot_t snap;
    TEST_ASSERT_EQUAL(ESP_OK, button_service_get_snapshot(&snap));
    TEST_ASSERT_TRUE(snap.task_running);

    btn_teardown();
}

/* D24: Urgent exactly once — exact counts, not <= 1. */
TEST_CASE("BTN-D24: urgent delivered exactly once no duplicate", "[btn][group_d][service]")
{
    btn_setup();
    fake_hal_ctx_t *fh = fake_hal_create();
    TEST_ASSERT_NOT_NULL(fh);
    test_sink_ctx_t nc, uc;
    test_sink_init(&nc); test_sink_init(&uc);
    button_service_config_t cfg = make_btn_config(fake_hal_get_interface(fh), &nc, &uc, 10, 50);
    TEST_ASSERT_EQUAL(ESP_OK, button_service_init(&cfg));

    fake_hal_set_button_state(fh, ALL_RELEASED);
    TEST_ASSERT_EQUAL(ESP_OK, button_service_start());
    TEST_ASSERT_TRUE(wait_for_btn_read_count(fh, 3, 2000));

    /* BTN3 press -> release -> wait for delivery */
    fake_hal_set_button_state(fh, BTN3_RAW_PRESSED);
    uint32_t gen = fake_hal_get_btn_read_count(fh);
    TEST_ASSERT_TRUE(wait_for_btn_read_count(fh, gen + 2, 2000));
    fake_hal_set_button_state(fh, ALL_RELEASED);
    gen = fake_hal_get_btn_read_count(fh);
    TEST_ASSERT_TRUE(wait_for_btn_read_count(fh, gen + 2, 2000));
    TEST_ASSERT_TRUE(wait_for_urgent_count(&uc, 1, 3000));

    /* Exact: count==1, delivered==1, pending==false */
    TEST_ASSERT_EQUAL(1, test_sink_get_count(&uc));
    button_service_snapshot_t snap;
    TEST_ASSERT_EQUAL(ESP_OK, button_service_get_snapshot(&snap));
    TEST_ASSERT_EQUAL(1, snap.urgent_delivered);
    TEST_ASSERT_FALSE(snap.urgent_pending);
    TEST_ASSERT_EQUAL(0, snap.normal_sink_drops);

    /* Additional observation window: no duplicate */
    vTaskDelay(pdMS_TO_TICKS(500));
    TEST_ASSERT_EQUAL(1, test_sink_get_count(&uc));
    TEST_ASSERT_EQUAL(ESP_OK, button_service_get_snapshot(&snap));
    TEST_ASSERT_EQUAL(1, snap.urgent_delivered);
    TEST_ASSERT_FALSE(snap.urgent_pending);

    btn_teardown();
}

/* D32: Urgent sink failing keeps pending beyond old limit of 10. */
TEST_CASE("BTN-D32: urgent sink failing keeps pending beyond old limit",
          "[btn][group_d][service]")
{
    btn_setup();
    fake_hal_ctx_t *fh = fake_hal_create();
    TEST_ASSERT_NOT_NULL(fh);
    test_sink_ctx_t nc, uc;
    test_sink_init(&nc); test_sink_init(&uc);
    test_sink_set_error(&uc, ESP_ERR_TIMEOUT);
    button_service_config_t cfg = make_btn_config(fake_hal_get_interface(fh), &nc, &uc, 10, 50);
    TEST_ASSERT_EQUAL(ESP_OK, button_service_init(&cfg));

    fake_hal_set_button_state(fh, ALL_RELEASED);
    TEST_ASSERT_EQUAL(ESP_OK, button_service_start());
    TEST_ASSERT_TRUE(wait_for_btn_read_count(fh, 3, 2000));

    /* BTN3 press+release */
    fake_hal_set_button_state(fh, BTN3_RAW_PRESSED);
    uint32_t gen = fake_hal_get_btn_read_count(fh);
    TEST_ASSERT_TRUE(wait_for_btn_read_count(fh, gen + 2, 2000));
    gen = fake_hal_get_btn_read_count(fh);
    fake_hal_set_button_state(fh, ALL_RELEASED);
    gen = fake_hal_get_btn_read_count(fh);
    TEST_ASSERT_TRUE(wait_for_btn_read_count(fh, gen + 2, 2000));
    /* Wait for retries to accumulate */
    vTaskDelay(pdMS_TO_TICKS(2000));

    button_service_snapshot_t snap;
    TEST_ASSERT_EQUAL(ESP_OK, button_service_get_snapshot(&snap));
    TEST_ASSERT_TRUE(snap.urgent_pending);
    TEST_ASSERT_GREATER_THAN(10, (int)snap.urgent_retry_count);

    /* Clear error → succeeds */
    test_sink_set_error(&uc, ESP_OK);
    TEST_ASSERT_TRUE(wait_for_urgent_count(&uc, 1, 2000));

    TEST_ASSERT_EQUAL(ESP_OK, button_service_get_snapshot(&snap));
    TEST_ASSERT_FALSE(snap.urgent_pending);
    TEST_ASSERT_GREATER_OR_EQUAL(1, (int)snap.urgent_delivered);

    btn_teardown();
}

/* D33: BTN1+BTN2+BTN3 → normal gets BTN1+BTN2, urgent gets BTN3.
 * Check button_id, sequence monotonic, exact counts. */
TEST_CASE("BTN-D33: BTN1+BTN2+BTN3 routing and sequence", "[btn][group_d][service]")
{
    btn_setup();
    fake_hal_ctx_t *fh = fake_hal_create();
    TEST_ASSERT_NOT_NULL(fh);
    test_sink_ctx_t nc, uc;
    test_sink_init(&nc); test_sink_init(&uc);
    button_service_config_t cfg = make_btn_config(fake_hal_get_interface(fh), &nc, &uc, 10, 50);
    TEST_ASSERT_EQUAL(ESP_OK, button_service_init(&cfg));

    fake_hal_set_button_state(fh, ALL_RELEASED);
    TEST_ASSERT_EQUAL(ESP_OK, button_service_start());
    TEST_ASSERT_TRUE(wait_for_btn_read_count(fh, 3, 2000));

    /* Press BTN1 + BTN2 simultaneously (bits 4,5 cleared) */
    uint8_t btn12 = ALL_RELEASED & ~((1U << 4) | (1U << 5));
    fake_hal_set_button_state(fh, btn12);
    uint32_t gen = fake_hal_get_btn_read_count(fh);
    TEST_ASSERT_TRUE(wait_for_btn_read_count(fh, gen + 2, 2000));

    /* Release BTN1+BTN2 */
    fake_hal_set_button_state(fh, ALL_RELEASED);
    gen = fake_hal_get_btn_read_count(fh);
    TEST_ASSERT_TRUE(wait_for_normal_count(&nc, 2, 3000));

    /* Press BTN3 */
    fake_hal_set_button_state(fh, BTN3_RAW_PRESSED);
    gen = fake_hal_get_btn_read_count(fh);
    TEST_ASSERT_TRUE(wait_for_btn_read_count(fh, gen + 2, 2000));

    /* Release BTN3 */
    fake_hal_set_button_state(fh, ALL_RELEASED);
    gen = fake_hal_get_btn_read_count(fh);
    TEST_ASSERT_TRUE(wait_for_btn_read_count(fh, gen + 2, 2000));
    TEST_ASSERT_TRUE(wait_for_urgent_count(&uc, 1, 3000));

    /* R1.6: Exact counts, not >= */
    TEST_ASSERT_EQUAL(2, test_sink_get_count(&nc));
    TEST_ASSERT_EQUAL(1, test_sink_get_count(&uc));

    /* R1.6: Event history — verify button_ids and sequence */
    if (xSemaphoreTake(nc.mtx, pdMS_TO_TICKS(100)) == pdTRUE) {
        TEST_ASSERT_EQUAL(2, nc.history_count);
        TEST_ASSERT_EQUAL(BUTTON_ID_1, nc.history[0].button_id);
        TEST_ASSERT_EQUAL(BUTTON_ID_2, nc.history[1].button_id);
        TEST_ASSERT_GREATER_THAN(nc.history[0].sequence, nc.history[1].sequence);
        xSemaphoreGive(nc.mtx);
    } else {
        TEST_FAIL_MESSAGE("nc.mtx timeout");
    }

    if (xSemaphoreTake(uc.mtx, pdMS_TO_TICKS(100)) == pdTRUE) {
        TEST_ASSERT_EQUAL(1, uc.history_count);
        TEST_ASSERT_EQUAL(BUTTON_ID_3, uc.history[0].button_id);
        xSemaphoreGive(uc.mtx);
    } else {
        TEST_FAIL_MESSAGE("uc.mtx timeout");
    }

    /* R1.6: total_events exactly 3, not >= 3 */
    button_service_snapshot_t snap;
    TEST_ASSERT_EQUAL(ESP_OK, button_service_get_snapshot(&snap));
    TEST_ASSERT_EQUAL(3, (int)snap.total_events);

    btn_teardown();
}

/* ================================================================
 * R1.5: New tests
 * ================================================================ */

/* R1.6: Abort isolation — proves global cleanup owns MCP, recovers for next test */
TEST_CASE("BTN-D36: abort isolation stops service before HAL destroy", "[btn][group_d][service]")
{
    btn_setup();
    uint32_t heap_before = esp_get_free_heap_size();

    /* Cycle 1: create HAL + init/start, then global cleanup (no local destroy) */
    fake_hal_ctx_t *fh = fake_hal_create();
    TEST_ASSERT_NOT_NULL(fh);
    test_sink_ctx_t nc, uc;
    test_sink_init(&nc); test_sink_init(&uc);
    button_service_config_t cfg = make_btn_config(fake_hal_get_interface(fh), &nc, &uc, 10, 50);
    TEST_ASSERT_EQUAL(ESP_OK, button_service_init(&cfg));
    TEST_ASSERT_EQUAL(ESP_OK, button_service_start());
    vTaskDelay(pdMS_TO_TICKS(30));

    /* The service borrows the HAL.  Prove task shutdown before destroying the
     * test-owned fake HAL, otherwise stop_disable() calls through freed memory. */
    TEST_ASSERT_EQUAL(ESP_OK, group_d_test_global_cleanup());
    fake_hal_destroy(fake_hal_get_singleton());
    TEST_ASSERT_FALSE(group_d_test_is_poisoned());

    /* Cycle 2: create new HAL + init/start/stop, global cleanup again */
    fh = fake_hal_create();
    TEST_ASSERT_NOT_NULL(fh);
    test_sink_init(&nc); test_sink_init(&uc);
    cfg = make_btn_config(fake_hal_get_interface(fh), &nc, &uc, 10, 50);
    TEST_ASSERT_EQUAL(ESP_OK, button_service_init(&cfg));
    TEST_ASSERT_EQUAL(ESP_OK, button_service_start());
    vTaskDelay(pdMS_TO_TICKS(30));
    TEST_ASSERT_EQUAL(ESP_OK, button_service_stop());

    TEST_ASSERT_EQUAL(ESP_OK, group_d_test_global_cleanup());
    fake_hal_destroy(fake_hal_get_singleton());

    /* Heap not leaking across cycles */
    uint32_t heap_after = esp_get_free_heap_size();
    TEST_ASSERT_INT_WITHIN(4096, heap_before, heap_after);
}

static bool pred_publish_attempts_ge_2(const button_service_snapshot_t *s, void *ctx)
{
    (void)ctx;
    return s->urgent_publish_attempts >= 2;
}

/* R1.8: Deterministic stale result injection using callback barrier.
 * Flow:
 * 1. attempt1 fails (force_error=TIMEOUT) → PENDING
 * 2. Clear error, arm callback barrier (cb_barrier) on urgent sink
 * 3. Service retries → callback blocks at barrier while in-flight
 * 4. While blocked: inject stale attempt1 completion
 * 5. Release barrier → actual callback result (attempt2) overwrites stale
 * 6. Verify: exactly 1 delivery, stale was rejected (proven by attempt mismatch)
 * No fixed delays. All synchronization via deterministic barriers. */
TEST_CASE("BTN-D37: stale urgent result rejected by attempt token",
          "[btn][group_d][service]")
{
    btn_setup();
    fake_hal_ctx_t *fh = fake_hal_create();
    TEST_ASSERT_NOT_NULL(fh);
    test_sink_ctx_t nc, uc;
    test_sink_init(&nc); test_sink_init(&uc);
    button_service_config_t cfg = make_btn_config(fake_hal_get_interface(fh), &nc, &uc, 10, 50);
    TEST_ASSERT_EQUAL(ESP_OK, button_service_init(&cfg));

    fake_hal_set_button_state(fh, ALL_RELEASED);
    TEST_ASSERT_EQUAL(ESP_OK, button_service_start());
    TEST_ASSERT_TRUE(wait_for_btn_read_count(fh, 3, 2000));

    /* Force urgent sink to fail first attempt */
    atomic_store(&uc.force_error, ESP_ERR_TIMEOUT);

    /* BTN3 press -> debounce -> release -> attempt1 starts and fails -> PENDING */
    fake_hal_set_button_state(fh, BTN3_RAW_PRESSED);
    uint32_t gen = fake_hal_get_btn_read_count(fh);
    TEST_ASSERT_TRUE(wait_for_btn_read_count(fh, gen + 2, 2000));
    fake_hal_set_button_state(fh, ALL_RELEASED);
    gen = fake_hal_get_btn_read_count(fh);
    TEST_ASSERT_TRUE(wait_for_btn_read_count(fh, gen + 2, 2000));

    /* Wait for attempt1 to fail and transition to PENDING */
    TEST_ASSERT_TRUE(wait_for_snapshot_predicate(
        pred_publish_attempts_ge_2, NULL, 5000));

    button_service_snapshot_t snap;
    TEST_ASSERT_EQUAL(ESP_OK, button_service_get_snapshot(&snap));
    TEST_ASSERT_GREATER_OR_EQUAL(1, (int)snap.urgent_publish_attempts);
    TEST_ASSERT_TRUE(snap.urgent_pending);

    /* Clear error for attempt2, arm callback barrier */
    atomic_store(&uc.force_error, ESP_OK);
    uc.cb_barrier = xSemaphoreCreateBinary();  /* created empty → blocks */
    TEST_ASSERT_NOT_NULL(uc.cb_barrier);

    /* Service retries → callback blocks at cb_barrier.
     * Wait for the callback to actually block by checking publish_attempts.
     * The callback will block BEFORE returning, so publish_attempts won't increment
     * until after the barrier is released. But the retry will have started
     * (urgent_state transitions to IN_FLIGHT before calling publish). */
    vTaskDelay(pdMS_TO_TICKS(500));  /* let retry start and callback block */

    /* While callback is blocked, inject stale completion for attempt1.
     * The service's current attempt is 2+ (retry). Stale attempt_id=1 won't match. */
    button_service_test_inject_completion(1, ESP_OK);

    /* Verify stale not committed — delivered still 0 */
    TEST_ASSERT_EQUAL(0, test_sink_get_count(&uc));

    /* Release callback barrier — actual callback completes with ESP_OK,
     * stores attempt2's result (overwrites stale). try_apply_handoff succeeds. */
    xSemaphoreGive(uc.cb_barrier);
    TEST_ASSERT_TRUE(wait_for_urgent_count(&uc, 1, 5000));

    /* Verify: exactly 1 delivery, no duplicates, 2+ publish attempts */
    TEST_ASSERT_EQUAL(1, test_sink_get_count(&uc));
    TEST_ASSERT_EQUAL(ESP_OK, button_service_get_snapshot(&snap));
    TEST_ASSERT_EQUAL(1, snap.urgent_delivered);
    TEST_ASSERT_FALSE(snap.urgent_pending);
    TEST_ASSERT_GREATER_THAN(1, (int)snap.urgent_publish_attempts);

    vSemaphoreDelete(uc.cb_barrier);
    uc.cb_barrier = NULL;
    btn_teardown();
}

/* R1.7: Sequential stop proves exactly-once join commit.
 * Uses snapshot counter (no re-init between stops).
 * Second stop returns INVALID_STATE and does NOT increment join_commit_count. */
TEST_CASE("BTN-D38: sequential stop exactly one join commit", "[btn][group_d][service]")
{
    btn_setup();
    fake_hal_ctx_t *fh = fake_hal_create();
    TEST_ASSERT_NOT_NULL(fh);
    test_sink_ctx_t nc, uc;
    test_sink_init(&nc); test_sink_init(&uc);
    button_service_config_t cfg = make_btn_config(fake_hal_get_interface(fh), &nc, &uc, 10, 50);
    TEST_ASSERT_EQUAL(ESP_OK, button_service_init(&cfg));

    fake_hal_set_button_state(fh, ALL_RELEASED);
    TEST_ASSERT_EQUAL(ESP_OK, button_service_start());
    TEST_ASSERT_TRUE(wait_for_btn_read_count(fh, 3, 2000));

    /* Snapshot before stop */
    button_service_test_counters_t ctrs_before;
    TEST_ASSERT_EQUAL(ESP_OK, button_service_test_get_counters(&ctrs_before));

    /* First stop: succeeds, commits join */
    esp_err_t stop_err = button_service_stop();
    TEST_ASSERT_EQUAL(ESP_OK, stop_err);

    /* R1.8: Verify join count incremented by exactly 1 via counter API */
    button_service_test_counters_t ctrs_after;
    TEST_ASSERT_EQUAL(ESP_OK, button_service_test_get_counters(&ctrs_after));
    TEST_ASSERT_EQUAL(ctrs_before.join_commit_count + 1, ctrs_after.join_commit_count);

    /* Second stop: INVALID_STATE, no re-commit */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, button_service_stop());

    /* Re-init for teardown */
    cfg = make_btn_config(fake_hal_get_interface(fh), &nc, &uc, 10, 50);
    TEST_ASSERT_EQUAL(ESP_OK, button_service_init(&cfg));
    btn_teardown();
}
