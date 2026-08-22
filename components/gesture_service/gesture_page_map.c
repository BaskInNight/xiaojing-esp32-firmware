/*
 * gesture_page_map.c — pure gesture → page-navigation mapping (no FreeRTOS/HAL deps)
 */

#include "gesture_page_map.h"

int8_t gesture_page_delta(hal_gesture_t gesture)
{
    switch (gesture) {
    case HAL_GESTURE_RIGHT:
    case HAL_GESTURE_FORWARD:
        return 1;
    case HAL_GESTURE_LEFT:
    case HAL_GESTURE_BACKWARD:
        return -1;
    default:
        return 0;
    }
}
