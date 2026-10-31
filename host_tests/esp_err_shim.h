/* Host-side shim for esp_err_t — used by host_tests only */
#pragma once
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK          0
#define ESP_ERR_NO_MEM  0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_INVALID_VERSION 0x105
#define ESP_ERR_INVALID_CRC 0x106
#define ESP_ERR_NOT_SUPPORTED 0x107
