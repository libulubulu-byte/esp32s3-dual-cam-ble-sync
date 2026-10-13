/**
 * @file app_conf.h
 * @brief 集中配置：角色、GATT UUID、文件名前缀、默认 lead_ms
 */

#pragma once

/* ★ 必须显式包含 sdkconfig.h：ESP-IDF 不会把它全局 -include 进源文件，
 *   而 main.c 里 `#include "sdkconfig.h"` 排在本头文件**之后**，
 *   若不在这里包含，下面的 CONFIG_BOARD_ROLE_HOST 判断永远为假 -> 角色永远是 SLAVE。 */
#include "sdkconfig.h"

#include <stdint.h>
#include "pins_config.h"

/* ============================ 角色（由 Kconfig 宏决定） ============================ */
#if defined(CONFIG_BOARD_ROLE_HOST)
#define BOARD_ROLE_HOST  1
#define BOARD_ROLE_SLAVE 0
#elif defined(CONFIG_BOARD_ROLE_SLAVE)
#define BOARD_ROLE_HOST  0
#define BOARD_ROLE_SLAVE 1
#else
#define BOARD_ROLE_HOST  0
#define BOARD_ROLE_SLAVE 1
#endif

/* ============================ BLE GATT UUID（沿用现有工程验证过的 0xABF0 体系） ============================ */
#define BLE_SYNC_SVC_UUID   0xABF0
#define BLE_SYNC_CMD_UUID   0xABF1
#define BLE_SYNC_RESP_UUID  0xABF2

/* ============================ 标记 GPIO（给示波器/逻辑分析仪） ============================ */
#ifndef BLE_SYNC_MARKER_GPIO
#define BLE_SYNC_MARKER_GPIO  PIN_FLASH   /* 板载补光 LED，高电平 = 到点 */
#endif

/* ============================ 拍照与存储 ============================ */
#define CAM_DEFAULT_FRAMESIZE  FRAMESIZE_SXGA   /* 1600x1200 */
#define CAM_DEFAULT_QUALITY    12
#define CAM_XCLK_MHZ          20

#define SD_MOUNT_POINT        "/sdcard"
#define SD_PHOTO_DIR          "sync_photos"
#define FILE_PREFIX_HOST      "host"
#define FILE_PREFIX_SLAVE     "slave"

/* ============================ 串口（仅主机使用） ============================ */
#define UART_CMD_NUM          UART_NUM_0
#define UART_CMD_BAUD         115200
#define UART_CMD_BUF_SIZE     256

/* ============================ 默认 lead_ms ============================ */
#define DEFAULT_LEAD_MS       500
#define MIN_LEAD_MS           100
#define MAX_LEAD_MS           1000

/* ============================ 重连 ============================ */
#define BLE_RECONNECT_INTERVAL_MS  5000
#define BLE_SCAN_DURATION_MS       3000

/* ★ 连接间隔：BLE 的单位是 1.25ms，不是 ms！
 *   目标 15ms -> 15 / 1.25 = 12 个单位。（曾经误写成 15*8=120 -> 150ms） */
#define BLE_CONN_INTERVAL_MS       15
#define BLE_CONN_ITVL_UNITS        (BLE_CONN_INTERVAL_MS * 4 / 5)   /* 12 = 15ms */

#define BLE_CONN_LATENCY          0
#define BLE_SUPERVISION_TIMEOUT_MS 3000

/* ============================ 摄像头引脚（供 HAL 使用） ============================ */
#define PIN_CAM_SDA  CAM_PIN_SDA
#define PIN_CAM_SCL  CAM_PIN_SCL

/* ============================ WiFi（照片上传用） ============================
 * ⚠️ 凭据**不要写在本文件里**（本文件是公开仓库的一部分）。
 *    把真实 SSID/密码/token 写进同目录的 app_conf.local.h，它已在 .gitignore 中：
 *
 *        // main/app_conf.local.h   （不入库）
 *        #define APP_WIFI_SSID         "your-ssid"
 *        #define APP_WIFI_PASS         "your-pass"
 *        #define APP_TELEGRAM_TOKEN    "123456:AA..."
 *        #define APP_TELEGRAM_CHAT_ID  "123456789"
 *
 *    留空 = 不启动 WiFi/上传，只做本地同步拍照（不影响同步精度验收）。 */
#if defined(__has_include)
#  if __has_include("app_conf.local.h")
#    include "app_conf.local.h"
#  endif
#endif

#ifndef APP_WIFI_SSID
#define APP_WIFI_SSID      ""
#endif
#ifndef APP_WIFI_PASS
#define APP_WIFI_PASS      ""
#endif
/** WiFi 最大发射功率（dBm，2..20）。★ 默认 11 而不是 20：
 *  S3 起射频那一瞬电流冲高，现场出现过 BROWNOUT 复位，降功率可避免。 */
#define APP_WIFI_MAX_TX_DBM   11

/** ★★ 强制指定的 DNS（留空 = 沿用 DHCP 下发的）。
 *  必须覆盖的原因：Windows 移动热点(ICS) 会把**宿主机**的 DNS 原样透传。
 *  若宿主机上跑着 Clash/v2ray 等代理的 TUN 模式，DNS 会返回 fake-ip
 *  （实测 api.telegram.org -> 198.18.0.41，RFC2544 保留段），那个地址只有
 *  宿主机的代理认识，ESP32 直连必然失败：`TG: open 失败 0x7002`，且每次
 *  都卡满 TCP 超时（~20 s）。换成公共 DNS 即可绕开。 */
#define APP_WIFI_DNS          "223.5.5.5"

/* ============================ Telegram 上传 ============================
 * token / chat_id 同样放 app_conf.local.h。留空 = 跳过上传
 * （照片仍会落 SD 卡，同步拍照功能完全不受影响）。 */
#ifndef APP_TELEGRAM_TOKEN
#define APP_TELEGRAM_TOKEN    ""
#endif
#ifndef APP_TELEGRAM_CHAT_ID
#define APP_TELEGRAM_CHAT_ID  ""
#endif
#define TG_API                "https://api.telegram.org"
/** ★ 建连超时放宽到 45 s：主机侧实测单次 TLS 建连要 13~27 s（Central 角色的
 *  射频开销 + 热点上行拥塞），15 s 会让第一次 open 直接超时，只能靠 retry 磨，
 *  整体拖到 40 s 以上且经常失败。放宽后第一次就有机会成功。 */
#define TG_TIMEOUT_MS         45000
#define TG_RETRY_MAX          5
/** 上传前等 WiFi 就绪的最长时间（秒）。★ 不能取小：两板同挂一个 Windows
 *  移动热点时主机的关联极不稳，实测 40+ 秒才 got_ip；等不够会把照片白丢。
 *  超时后照片会**放回队列**而不是丢弃，等下次静默再试。 */
#define TG_WIFI_WAIT_S        60
/** 同一 chat 的最小上传间隔（节流，防刷屏） */
#define TG_THROTTLE_MS        1000

/** ★★ 两板上传错开时间（ms）：两板往**同一个 chat** 发 sendPhoto，实测几乎同时
 *   发起时，从机 3.3 s 成功、主机 18.5 s 建连失败（0x7002），单独测主机却能
 *   ~13 s 成功 —— 并发才是主因。让从机让开主机一段时间即可。
 *   （主机 0：它是被指令触发的那一方，先发。）*/
#if BOARD_ROLE_HOST
#define TG_UPLOAD_STAGGER_MS  0
#else
#define TG_UPLOAD_STAGGER_MS  8000
#endif
/** 上传队列长度（照片在 PSRAM 里排队，超出丢弃最旧）。
 *  ★ 一次批次可能要攒 20~40 张再传，所以队列要留够：40 张 × ~32KB ≈ 1.3MB PSRAM。 */
#define UPLOAD_QUEUE_LEN      40

/** ★★ 上传是**低优先级后台任务**：拍照只入队，等"静默"这么久没有新拍照才真正
 *  开始传；一旦有新拍照就**打断**在传的那张（分块写，每块前检查）。
 *  这样连续拍 20 张期间射频完全留给 BLE + 摄像头（同步精度不受上传干扰），
 *  20 张拍完才开始传。
 *  ⚠️ 该值必须**大于两枪之间的间隔**，否则每枪之间都会被判定为空闲。
 *     （网页验收默认 Interval = 3000 ms，这里取 5000 ms。）*/
#define TG_IDLE_BEFORE_UPLOAD_MS  5000
