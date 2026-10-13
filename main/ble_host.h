/**
 * @file ble_host.h
 * @brief 主机 BLE（NimBLE GATT Client）
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ble_host_init(void);
bool ble_host_connected(void);
esp_err_t ble_host_send_fire(const char *cmd);
esp_err_t ble_host_arm_fire(int64_t target_us, int lead_ms);

/** 立即抓一帧并投递上传（串口 SNAP 命令用；不经过同步触发） */
bool ble_host_snap_now(void);
/** 输出一行 CSV（行首自动加 "HOST,"），内部有锁，可从任意任务调用 */
void ble_host_csv(const char *fmt, ...);

/** ★ 上传模式：整个 TLS 会话期间把射频让给 WiFi。
 *  - 暂停 BLE 扫描（找不到从机时扫描会持续抢射频）
 *  - 把连接间隔从 15ms 放宽到 100ms 并上报给对端（Central 每个连接事件
 *    都要发包，15ms 间隔下 WiFi 的收发窗口被切得很碎）
 *  上传发生在 FIRE 到点**之后**，同步已经完成，所以放宽间隔不影响同步精度；
 *  lead 默认 500ms，足以吸收 100ms 的连接延迟。
 *  （从机侧是弱符号空实现。） */
void ble_host_set_upload_mode(bool on);

#ifdef __cplusplus
}
#endif
