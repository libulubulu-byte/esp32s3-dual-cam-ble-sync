/**
 * @file pins_config.h
 * @brief XIAO ESP32-S3 + OV3660 + SDMMC 引脚表（从 verified 工程 straight copy）
 */

#pragma once

/* ============================ 摄像头 FPC ============================ */
#define CAM_PIN_D0     11
#define CAM_PIN_D1     9
#define CAM_PIN_D2     8
#define CAM_PIN_D3     10
#define CAM_PIN_D4     12
#define CAM_PIN_D5     18
#define CAM_PIN_D6     17
#define CAM_PIN_D7     16
#define CAM_PIN_XCLK   15
#define CAM_PIN_PCLK   13
#define CAM_PIN_VSYNC  6
#define CAM_PIN_HREF   7
#define CAM_PIN_SDA    4
#define CAM_PIN_SCL    5
#define CAM_PIN_RESET  (-1)
#define CAM_PIN_PWDN   (-1)

/* ============================ SD 卡 (SDMMC 1-bit) ============================ */
#define SD_PIN_CLK     39
#define SD_PIN_CMD     38
#define SD_PIN_DAT0    40
#define SD_PIN_CD      (-1)
#define SD_CD_ACTIVE_LOW 1

/* ============================ 用户外设 ============================ */
#define PIN_PIR        (-1)
#define PIN_BUTTON     0
#define PIN_FLASH      21
#define PIN_BUZZER     (-1)
#define PIN_LED_WS2812 (-1)
#define PIN_LED_GREEN  2

/* ============================ 保留引脚 ============================ */
#define PIN_RESERVED_USB_DM     19
#define PIN_RESERVED_USB_DP     20
#define PIN_RESERVED_PSRAM_0    33
#define PIN_RESERVED_PSRAM_1    37
#define PIN_RESERVED_UART0_TX   43
#define PIN_RESERVED_UART0_RX   44
#define PIN_RESERVED_STRAP_BOOT 45
#define PIN_RESERVED_STRAP_LOG  46
