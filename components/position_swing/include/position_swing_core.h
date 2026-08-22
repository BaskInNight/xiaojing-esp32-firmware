/*
 * position_swing_core.h — 连续换向展示台 纯 C 逻辑（host-testable）
 *
 * 只包含无依赖的静态常量与 clamp 函数。运行时状态机在
 * position_swing_service.c（依赖 FreeRTOS/HAL），本文件不依赖任何
 * 运行时环境，供服务内部与 host 测试共用同一份权威 clamp。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 单段时长/段数硬上限（比 position move 130s 更严，限制能耗与观众暴露） */
#define POS_SWING_DWELL_MIN_MS        500U
#define POS_SWING_DWELL_DEFAULT_MS    1500U
#define POS_SWING_DWELL_MAX_MS        8000U
#define POS_SWING_ROUNDS_DEFAULT      4U
#define POS_SWING_ROUNDS_MAX          4U
#define POS_SWING_PERIOD_MAX_MS       (POS_SWING_DWELL_MAX_MS + 500U)

/* 参数 clamp（0 → 默认；超上下限 → 封顶/下限） */
uint32_t position_swing_clamp_rounds(uint32_t rounds);
uint32_t position_swing_clamp_dwell(uint32_t dwell_ms);

#ifdef __cplusplus
}
#endif