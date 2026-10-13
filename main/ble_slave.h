/**
 * @file ble_slave.h
 * @brief 从机 BLE（NimBLE GATT Server）
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ble_slave_init(void);
bool ble_slave_connected(void);

/** 立即抓一帧并投递上传（串口 SNAP 命令用；不经过 BLE 触发） */
bool ble_slave_snap_now(void);

#ifdef __cplusplus
}
#endif
