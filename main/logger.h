#pragma once

#include <stdint.h>
#include "esp_log.h"

typedef enum {
    LOG_T_APP = 0,
    LOG_T_CAM,
    LOG_T_SD,
    LOG_T_BLE,
    LOG_T_SYNC,
    LOG_T_UART,
    LOG_T_MAX
} log_tag_t;

#define LOG_T_APP   "APP"
#define LOG_T_CAM   "CAM"
#define LOG_T_SD    "SD"
#define LOG_T_BLE   "BLE"
#define LOG_T_SYNC  "SYNC"
#define LOG_T_UART  "UART"

#define LOGI(tag, fmt, ...)  ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#define LOGW(tag, fmt, ...)  ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#define LOGE(tag, fmt, ...)  ESP_LOGE(tag, fmt, ##__VA_ARGS__)
#define LOGV(tag, fmt, ...)  ESP_LOGV(tag, fmt, ##__VA_ARGS__)
