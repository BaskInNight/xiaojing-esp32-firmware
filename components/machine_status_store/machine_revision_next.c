/*
 * machine_revision_next.c — 纯 C，无 FreeRTOS/ESP-IDF 依赖
 * 固件和 host test 共同编译此文件
 */
#include "machine_revision_next.h"

uint32_t machine_revision_next(uint32_t current)
{
    uint32_t next = current + 1;
    return (next == 0) ? 1 : next;
}
