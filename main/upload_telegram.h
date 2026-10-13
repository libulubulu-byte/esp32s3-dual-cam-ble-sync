/**
 * @file upload_telegram.h
 * @brief 照片上传到 Telegram（异步队列，不阻塞同步触发）
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 创建上传队列与上传任务（不依赖 WiFi 是否已连上，任务自己等） */
esp_err_t upload_telegram_init(void);

/**
 * @brief 投递一张照片去上传（内部会把 JPEG 拷进 PSRAM 后立刻返回）
 *
 * @param jpeg    JPEG 数据（调用方保留所有权，本函数会拷贝）
 * @param len     字节数
 * @param t_fire  触发时刻（µs，写进 caption 便于与串口日志对照）
 * @return ESP_OK 已入队；ESP_ERR_NO_MEM 内存不足；ESP_ERR_INVALID_STATE 队列满（已丢最旧）
 */
esp_err_t upload_telegram_enqueue(const uint8_t *jpeg, size_t len, int64_t t_fire);

/** 累计成功/失败计数（给网页/日志用） */
void upload_telegram_stats(uint32_t *ok, uint32_t *fail);

#ifdef __cplusplus
}
#endif
