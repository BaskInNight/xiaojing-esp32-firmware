#pragma once

#include <stdint.h>
#include "xiaojing_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Gesture → page navigation delta.  This is the single adaptation boundary
 * between the raw PAJ7620 gesture direction and the display page navigation.
 *
 * User expectation (physical hand motion):
 *   hand sweeping left → right (HAL_GESTURE_RIGHT) : next page   (+1)
 *   hand sweeping right → left (HAL_GESTURE_LEFT)  : previous page (-1)
 *   hand pushing forward (FORWARD)                 : next page   (+1)
 *   hand pulling back   (BACKWARD)                 : previous page (-1)
 *   other gestures                                 : no page action (0)
 *
 * The sensor log keeps reporting the raw direction; only this mapping inverts
 * for UI navigation.  Do NOT swap the PAJ7620 LEFT/RIGHT enum values.
 */
int8_t gesture_page_delta(hal_gesture_t gesture);

#ifdef __cplusplus
}
#endif
