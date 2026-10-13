/**
 * @file main.c
 * @brief ESP32-S3 双机 BLE 同步拍照系统
 *
 * 角色：
 *   HOST：接收 PC 串口 FIRE,<lead_ms> → BLE 发给 SLAVE → 本地 esp_timer 拍照
 *   SLAVE：收到 BLE FIRE → 本地 esp_timer 拍照
 *
 * 切换方式：menuconfig -> Board Config -> BOARD_ROLE -> host / slave
 */

#include "app_conf.h"
#include "logger.h"
#include "camera_hal.h"
#include "sd_hal.h"
#include "ble_slave.h"
#include "ble_host.h"
#include "uart_cmd.h"
#include "wifi_hal.h"
#include "upload_telegram.h"

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_timer.h"

static const char *TAG = LOG_T_APP;

void app_main(void)
{
    /* 注意：不要调 esp_timer_init() —— IDF 启动时已完成，
     * 重复调用会打印 "E esp_timer: Task is already initialized" */

    LOGI(TAG, "=== dual_cam_ble_sync boot ===");
    LOGI(TAG, "role=%s", BOARD_ROLE_HOST ? "HOST" : "SLAVE");

    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(e);
    }

    /* 摄像头先初始化（占时 ~1s） */
    e = camera_hal_init();
    if (e != ESP_OK) {
        LOGE(TAG, "camera init failed err=0x%x -> continue without camera", e);
    }

    /* SD 卡 */
    e = sd_hal_init();
    if (e != ESP_OK) {
        LOGW(TAG, "sd init failed err=0x%x -> continue without SD", e);
    }

    /* WiFi（照片上传的外网出口）；没配 SSID 时会直接跳过 */
    e = wifi_hal_init();
    if (e != ESP_OK) {
        LOGW(TAG, "wifi init skipped err=0x%x", e);
    }

    /* Telegram 上传队列（异步任务，不阻塞同步触发） */
    e = upload_telegram_init();
    if (e != ESP_OK) {
        LOGW(TAG, "telegram upload skipped err=0x%x", e);
    }

    /* BLE */
#if BOARD_ROLE_HOST
    e = ble_host_init();
#else
    e = ble_slave_init();
#endif
    if (e != ESP_OK) {
        LOGE(TAG, "ble init failed err=0x%x", e);
    }

    /* 串口命令：主机收 FIRE / PING / SNAP；从机收 SNAP（用于单板验证上传链路） */
    e = uart_cmd_init();
    if (e != ESP_OK) {
        LOGE(TAG, "uart cmd init failed err=0x%x", e);
    }

    LOGI(TAG, "system ready");
}
