/**
 * @file sd_hal.h
 * @brief SD 卡极简 HAL（mount / write / exists）
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t sd_hal_init(void);
esp_err_t sd_hal_write_file(const char *rel_path, const void *data, size_t len);
bool sd_hal_is_mounted(void);
const char *sd_hal_mount_point(void);

#ifdef __cplusplus
}
#endif
