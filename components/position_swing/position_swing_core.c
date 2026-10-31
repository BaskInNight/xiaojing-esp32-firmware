/*
 * position_swing_core.c — 连续换向展示台纯 C 逻辑（host-testable）
 * 仅实现 clamp 纯函数，无 FreeRTOS/HAL/静态可变状态。
 * 运行时状态机在 position_swing_service.c。
 */

#include "position_swing_core.h"

uint32_t position_swing_clamp_rounds(uint32_t rounds)
{
    if (rounds == 0) return POS_SWING_ROUNDS_DEFAULT;
    return rounds > POS_SWING_ROUNDS_MAX ? POS_SWING_ROUNDS_MAX : rounds;
}

uint32_t position_swing_clamp_dwell(uint32_t dwell_ms)
{
    if (dwell_ms == 0) return POS_SWING_DWELL_DEFAULT_MS;
    if (dwell_ms < POS_SWING_DWELL_MIN_MS) return POS_SWING_DWELL_MIN_MS;
    if (dwell_ms > POS_SWING_DWELL_MAX_MS) return POS_SWING_DWELL_MAX_MS;
    return dwell_ms;
}