/**
 * @file uart_cmd.c
 * @brief 主机串口命令解析（FIRE,<lead_ms>）
 *
 * 仅主机使用；从机不编译本文件。
 */

#include "uart_cmd.h"
#include "app_conf.h"
#include "logger.h"
#include "ble_host.h"
#include "ble_slave.h"

#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_timer.h"

static const char *TAG = LOG_T_UART;

#define UART_BUF_SIZE  256

static void handle_line(char *line)
{
    /* 去掉尾部 \r \n 空格 */
    size_t n = strlen(line);
    while (n > 0 &&
           (line[n - 1] == '\r' || line[n - 1] == '\n' ||
            line[n - 1] == ' '  || line[n - 1] == '\t')) {
        line[--n] = '\0';
    }
    char *p = line;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '\0') {
        return;
    }

    LOGI(TAG, "cmd \"%s\"", p);

    if (strncasecmp(p, "FIRE,", 5) == 0) {
        int lead_ms = atoi(p + 5);
        if (lead_ms < MIN_LEAD_MS)  lead_ms = MIN_LEAD_MS;
        if (lead_ms > MAX_LEAD_MS)  lead_ms = MAX_LEAD_MS;

        int64_t t_rx = esp_timer_get_time();
        ble_host_csv("RX_CMD,%lld,%d", (long long)t_rx, lead_ms);

        /* arm_fire 内部：本地定时 + 给从机发 ARM(绝对时刻) */
        int64_t target = t_rx + (int64_t)lead_ms * 1000;
        esp_err_t e = ble_host_arm_fire(target, lead_ms);
        if (e != ESP_OK) {
            LOGE(TAG, "arm_fire failed err=0x%x (ble not ready?)", e);
        }
    } else if (strcasecmp(p, "PING") == 0) {
        LOGI(TAG, "PONG");
    } else if (strcasecmp(p, "SNAP") == 0) {
        /* 立即抓一帧并投递 Telegram 上传（不经过 BLE 同步触发）。
         * 用途：单板独立验证"拍照→WiFi→Telegram"这条链路。 */
#if BOARD_ROLE_HOST
        bool ok = ble_host_snap_now();
#else
        bool ok = ble_slave_snap_now();
#endif
        LOGI(TAG, "SNAP -> %s", ok ? "ok" : "fail");
    } else {
        LOGW(TAG, "unknown cmd \"%s\"", p);
    }
}

static void cmd_task(void *arg)
{
    (void)arg;
    static uint8_t buf[UART_BUF_SIZE];
    size_t pos = 0;

    for (;;) {
        /* ★★ 关键：必须在每次迭代时把"已消费的字节"从缓冲里移走。
         *   旧实现写成 `memmove(buf, &buf[0], pos)`（空操作）+ pos 从不重置，
         *   于是 pos 只增不减，约 30 条命令后 `UART_BUF_SIZE - pos - 1` 变成 0，
         *   uart_read_bytes 永远读 0 字节 -> 任务静默卡死（串口还在发，设备不响应）。 */
        int space = (int)sizeof(buf) - (int)pos - 1;
        if (space <= 0) {
            /* 行太长（没有换行）：整个丢弃，避免死锁 */
            LOGW(TAG, "line too long -> drop %u bytes", (unsigned)pos);
            pos = 0;
            space = (int)sizeof(buf) - 1;
        }

        int len = uart_read_bytes(UART_CMD_NUM, &buf[pos], space, pdMS_TO_TICKS(100));
        if (len > 0) {
            pos += (size_t)len;
        }

        /* 逐行消费 */
        size_t consumed = 0;
        for (;;) {
            uint8_t *nl = memchr(buf + consumed, '\n', pos - consumed);
            if (!nl) {
                break;
            }
            size_t linelen = (size_t)(nl - (buf + consumed));
            if (linelen > 0 && linelen < UART_BUF_SIZE) {
                char line[UART_BUF_SIZE];
                memcpy(line, buf + consumed, linelen);
                line[linelen] = '\0';
                handle_line(line);
            }
            consumed = linelen + 1;
        }

        if (consumed > 0) {
            memmove(buf, buf + consumed, pos - consumed);
            pos -= consumed;
        }
    }
}

esp_err_t uart_cmd_init(void)
{
    const uart_config_t cfg = {
        .baud_rate  = UART_CMD_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t e = uart_param_config(UART_CMD_NUM, &cfg);
    if (e != ESP_OK) {
        return e;
    }

    e = uart_set_pin(UART_CMD_NUM, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (e != ESP_OK && e != ESP_ERR_NOT_SUPPORTED) {
        return e;
    }

    e = uart_driver_install(UART_CMD_NUM, UART_BUF_SIZE * 2, 0, 0, NULL, 0);
    if (e != ESP_OK) {
        return e;
    }

    if (xTaskCreate(cmd_task, "uart_cmd", 4096, NULL, 6, NULL) != pdPASS) {
        LOGE(TAG, "cmd_task create fail");
        return ESP_ERR_NO_MEM;
    }

    LOGI(TAG, "init ok uart=%d baud=%d", UART_CMD_NUM, UART_CMD_BAUD);
    return ESP_OK;
}
