/**
 * @file camera_hal.h
 * @brief OV3660 摄像头极简 HAL（init / grab / return）
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_camera.h"
#include "pins_config.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t camera_hal_init(void);
camera_fb_t *camera_hal_grab(uint32_t timeout_ms);
esp_err_t camera_hal_return(camera_fb_t *fb);
const char *camera_hal_sensor_name(void);
int camera_hal_current_framesize(void);

#ifdef __cplusplus
}
#endif
