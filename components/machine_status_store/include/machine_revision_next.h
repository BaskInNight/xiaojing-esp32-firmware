#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pure function: next revision, UINT32_MAX → 1 (skip 0 = "never updated") */
uint32_t machine_revision_next(uint32_t current);

#ifdef __cplusplus
}
#endif
