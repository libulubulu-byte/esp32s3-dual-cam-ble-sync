/**
 * @file uart_cmd.h
 * @brief 主机串口命令解析（FIRE,<lead_ms>）
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t uart_cmd_init(void);

#ifdef __cplusplus
}
#endif
