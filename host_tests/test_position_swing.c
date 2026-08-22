/*
 * test_position_swing.c — position_swing clamp 纯 C 边界 host 测试
 *
 * 编译真实生产 clamp 函数（components/position_swing/include/position_swing.h
 * 声明的 position_swing_clamp_rounds / position_swing_clamp_dwell），
 * 验证边界与安全上限。position_swing 服务本身需要 FreeRTOS/HAL，不在
 * host 测试范围；此处仅锁定 clamp 与状态枚举的稳定性（供小程序协议契约）。
 */
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "position_swing_core.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { \
    tests_run++; \
    printf("  TEST %02d: %-58s ", tests_run, name); \
} while (0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while (0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while (0)

static void test_clamp_rounds(void)
{
    TEST("rounds=0 -> default");
    if (position_swing_clamp_rounds(0) == POS_SWING_ROUNDS_DEFAULT) PASS();
    else FAIL("0 should default");

    TEST("rounds=1 -> 1");
    if (position_swing_clamp_rounds(1) == 1) PASS(); else FAIL("1");

    TEST("rounds=4 -> 4 (max)");
    if (position_swing_clamp_rounds(4) == 4) PASS(); else FAIL("4");

    TEST("rounds=99 -> capped 4");
    if (position_swing_clamp_rounds(99) == POS_SWING_ROUNDS_MAX) PASS();
    else FAIL("cap");

    TEST("rounds=2 valid");
    if (position_swing_clamp_rounds(2) == 2) PASS(); else FAIL("2");
}

static void test_clamp_dwell(void)
{
    TEST("dwell=0 -> default");
    if (position_swing_clamp_dwell(0) == POS_SWING_DWELL_DEFAULT_MS) PASS();
    else FAIL("0 default");

    TEST("dwell=499 -> min 500");
    if (position_swing_clamp_dwell(499) == POS_SWING_DWELL_MIN_MS) PASS();
    else FAIL("min");

    TEST("dwell=1500 ok");
    if (position_swing_clamp_dwell(1500) == 1500) PASS(); else FAIL("1500");

    TEST("dwell=8000 ok max");
    if (position_swing_clamp_dwell(8000) == POS_SWING_DWELL_MAX_MS) PASS();
    else FAIL("max");

    TEST("dwell=9000 -> capped 8000");
    if (position_swing_clamp_dwell(9000) == POS_SWING_DWELL_MAX_MS) PASS();
    else FAIL("cap");
}

int main(void)
{
    test_clamp_rounds();
    test_clamp_dwell();

    printf("\n%d/%d passed\n", tests_passed, tests_run);
    return tests_run == tests_passed ? 0 : 1;
}