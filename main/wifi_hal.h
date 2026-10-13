/**
 * @file wifi_hal.h
 * @brief 极简 WiFi STA（只为照片上传提供外网出口；不做 AP 配网）
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 启动 STA 并开始连接（非阻塞；失败会自动重连） */
esp_err_t wifi_hal_init(void);

/** 是否已拿到 IP（只有拿到 IP 才能发起 TLS 上传） */
bool wifi_hal_connected(void);

/** 当前连接的 SSID（未连接返回 ""） */
const char *wifi_hal_ssid(void);

#ifdef __cplusplus
}
#endif
