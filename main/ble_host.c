/**
 * @file ble_host.c
 * @brief 主机：NimBLE GATT Client（扫描 -> 连接 -> 服务发现 -> 写命令）+ 本地定时拍照
 *
 * CSV 日志经 UART0（115200）输出，行首固定为 "HOST,"，便于 PC 脚本过滤：
 *   HOST,BOOT,<t_us>
 *   HOST,BLE_CONNECTED,<t_us>
 *   HOST,BLE_DISCONNECTED,<t_us>,<reason>
 *   HOST,RX_CMD,<t_rx_us>,<lead_ms>
 *   HOST,FIRE_SCHEDULED,<t_rx_us>,<target_us>
 *   HOST,FIRE_ACTUAL,<t_fire_us>,<jitter_us>
 *   HOST,DONE,<t_done_us>
 */

#include "ble_host.h"
#include "app_conf.h"
#include "logger.h"
#include "camera_hal.h"
#include "sd_hal.h"
#include "upload_telegram.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <stdlib.h>
#include <time.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "driver/gpio.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "host/ble_gatt.h"

static const char *TAG = LOG_T_BLE;

#define PHOTO_PREFIX "host"

static const ble_uuid16_t s_svc_uuid  = BLE_UUID16_INIT(BLE_SYNC_SVC_UUID);
static const ble_uuid16_t s_cmd_uuid  = BLE_UUID16_INIT(BLE_SYNC_CMD_UUID);
static const ble_uuid16_t s_resp_uuid = BLE_UUID16_INIT(BLE_SYNC_RESP_UUID);

/* ============================== 状态 ============================== */
static volatile bool            s_inited;
static volatile bool            s_connected;
static uint16_t                 s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint8_t                  s_own_addr_type;
static uint16_t                 s_cmd_handle;
static uint16_t                 s_resp_handle;
static uint16_t                 s_cccd_handle;

/* --- 钟差标定：t_slave = t_host + s_offset --- */
static volatile bool            s_offset_valid;
static volatile int64_t         s_offset;        /* µs */
static volatile int64_t         s_best_rtt;      /* 最小 RTT 样本 */
static volatile int64_t         s_ping_tx;       /* PING 发出时刻（主机时钟） */
static volatile int            s_calib_cnt;     /* 已收集样本数 */
#define CALIB_SAMPLES   12
static esp_timer_handle_t       s_fire_timer;
static esp_timer_handle_t       s_reconnect_timer;
static bool                     s_reconnect_pending;
static bool                     s_connecting;
static bool                     s_scanning;
static volatile bool            s_scan_paused;   /* 上传期间把射频完全让给 WiFi */
static TaskHandle_t             s_sync_task;
static SemaphoreHandle_t        s_csv_lock;

static volatile int64_t         s_target_us;
static volatile int64_t         s_fire_ts;
static volatile int64_t         s_fire_jitter;

/* ============================== 工具 ============================== */
static int64_t now_us(void)
{
    return esp_timer_get_time();
}

void ble_host_csv(const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (s_csv_lock) {
        xSemaphoreTake(s_csv_lock, portMAX_DELAY);
    }
    printf("HOST,%s\n", buf);
    fflush(stdout);
    if (s_csv_lock) {
        xSemaphoreGive(s_csv_lock);
    }
}

#define csv_line ble_host_csv

/**
 * 把**从机经 BLE 回传**的事件打印到本机串口，行首用 "SLAVE,"。
 *
 * 用途：PC 只连主机一个串口（网页验收 / Web Serial）时，仍能拿到两板数据。
 * 从机发的是通知 `E,<event>,<t>,<extra>`，这里去掉 "E," 并补 "SLAVE," 前缀，
 * 输出格式与从机本地串口**完全一致**，测试脚本/网页无需区分来源。
 */
static void csv_line_slave(const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (s_csv_lock) {
        xSemaphoreTake(s_csv_lock, portMAX_DELAY);
    }
    printf("SLAVE,%s\n", buf);
    fflush(stdout);
    if (s_csv_lock) {
        xSemaphoreGive(s_csv_lock);
    }
}

static void marker_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BLE_SYNC_MARKER_GPIO,
        .mode         = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(BLE_SYNC_MARKER_GPIO, 0);
    LOGI(TAG, "marker GPIO%d (high = fire)", BLE_SYNC_MARKER_GPIO);
}

static inline void marker_set(int level)
{
    gpio_set_level(BLE_SYNC_MARKER_GPIO, level);
}

/* ============================== 心跳（独立任务） ==============================
 * 为什么用**独立任务**而不是 esp_timer：心跳要 printf，而 printf 走 UART0
 * 可能阻塞几毫秒。如果和到点触发共用 esp_timer 任务，正好撞上就会把 jitter
 * 从 µs 拉到 ms 级。放独立任务彻底隔离。 */
static void hb_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        csv_line("HB,%lld,conn=%d,bound=%d,calib=%d",
                 (long long)now_us(), (int)s_connected,
                 (int)(s_cmd_handle != 0), (int)s_offset_valid);
    }
}

/* ============================== 拍照 ============================== */
/** 抓一帧后：① 有 SD 就落盘；② 投递 Telegram 上传（**不依赖 SD**） */
static bool capture_once(int64_t t_fire)
{
    camera_fb_t *fb = camera_hal_grab(2000);
    if (!fb) {
        LOGE(TAG, "grab fail");
        return false;
    }

    bool ok = false;

    if (sd_hal_is_mounted()) {
        char path[128];
        snprintf(path, sizeof(path), "%s/%s_%lld.jpg",
                 SD_PHOTO_DIR, PHOTO_PREFIX, (long long)t_fire);
        if (sd_hal_write_file(path, fb->buf, fb->len) == ESP_OK) {
            LOGI(TAG, "photo saved %s len=%u", path, (unsigned)fb->len);
            ok = true;
        } else {
            LOGE(TAG, "photo write fail %s", path);
        }
    } else {
        LOGW(TAG, "SD not mounted -> 只上传不落盘");
    }

    esp_err_t e = upload_telegram_enqueue(fb->buf, fb->len, t_fire);
    if (e == ESP_OK) {
        LOGI(TAG, "photo queued for telegram len=%u", (unsigned)fb->len);
        ok = true;
    } else {
        LOGW(TAG, "telegram enqueue failed 0x%x", e);
    }

    camera_hal_return(fb);
    return ok;
}

/* ============================== 拍照任务 ============================== */
static void sync_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        int64_t t_fire = s_fire_ts;
        int64_t jitter = s_fire_jitter;

        csv_line("FIRE_ACTUAL,%lld,%lld", (long long)t_fire, (long long)jitter);

        int64_t t0 = now_us();
        bool ok = capture_once(t_fire);
        int64_t took = (now_us() - t0) / 1000;

        csv_line("DONE,%lld", (long long)now_us());
        LOGI(TAG, "capture done ok=%d took=%lldms jitter=%+lldus",
             (int)ok, (long long)took, (long long)jitter);

        marker_set(0);
    }
}

static void fire_timer_cb(void *arg)
{
    (void)arg;
    s_fire_ts     = now_us();
    s_fire_jitter = s_fire_ts - s_target_us;
    marker_set(1);
    if (s_sync_task) {
        xTaskNotifyGive(s_sync_task);
    }
}

/* ============================== 扫描 / 重连 ============================== */
static void start_scan(void);
static int  gap_event(struct ble_gap_event *event, void *arg);

static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    if (!s_inited) {
        return;
    }
    s_reconnect_pending = false;
    if (s_connected) {
        return;
    }
    LOGI(TAG, "reconnect timer -> start scan");
    start_scan();
}

static void start_scan(void)
{
    if (s_connected || s_scanning) {
        return;
    }
    /* ★ 上传期间把射频整个让给 WiFi（上传不需要找从机）。
     *   实测：不停扫描时主机 TLS 建连要 15 s 超时 + retry，共 26~43 s。 */
    if (s_scan_paused) {
        LOGI(TAG, "scan skipped (paused for upload)");
        return;
    }
    struct ble_gap_disc_params params;
    memset(&params, 0, sizeof(params));
    /* ★★ 扫描占空比必须给 Wi-Fi 留窗口。
     *    原来 itvl == window == 0x0010 是 **100% duty**：扫描期间射频连续占用，
     *    ESP32-S3 只有一套 2.4G 射频，共存仲裁下 Wi-Fi RX 会被完全挤掉。
     *    实测症状（从机不在、主机正在找从机时）：
     *      - ping 主机 100% 丢包（ICMP 请求落在扫描窗口内收不到）
     *      - 上电后 reason=1 / reason=205 连掉两次，DHCP 第三次才拿到 IP
     *      - TLS 建连 15 s 超时（TG: open 失败 0x7002），靠 retry 磨到 27~43 s
     *    从机是 Peripheral、**不扫描**，所以它从来没有这个问题 ——
     *    这就是"从机 3 s 传得上去、主机传不上去"的直接原因。
     *    单位 0.625 ms：itvl=0x0050(50 ms) / window=0x0010(10 ms) -> 20%。 */
    params.itvl              = 0x0050;   /* 50ms */
    params.window            = 0x0010;   /* 10ms -> 20% duty（原来 100%） */
    params.filter_policy     = BLE_HCI_SCAN_FILT_NO_WL;
    params.passive           = 0;        /* 主动扫描（可拿 scan response） */
    params.filter_duplicates = 0;

    int rc = ble_gap_disc(s_own_addr_type, BLE_SCAN_DURATION_MS, &params,
                          gap_event, NULL);
    if (rc == 0) {
        s_scanning = true;
        LOGI(TAG, "scan started");
    } else {
        LOGW(TAG, "scan start rc=%d", rc);
    }
}

/* ============================== 服务/特征发现回调 ============================== */
static void calib_send_ping(void);

static int dsc_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)chr_val_handle;
    (void)arg;
    if (error->status == 0 && dsc) {
        if (dsc->uuid.u.type == BLE_UUID_TYPE_16 && dsc->uuid.u16.value == 0x2902) {
            s_cccd_handle = dsc->handle;
            LOGI(TAG, "found CCCD handle=%u", s_cccd_handle);
        }
    } else if (error->status == BLE_HS_EDONE) {
        /* 订阅 notify：写 CCCD = 0x0001 */
        if (s_cccd_handle) {
            uint8_t val[2] = { 0x01, 0x00 };
            int rc = ble_gattc_write_flat(conn_handle, s_cccd_handle, val, sizeof(val), NULL, NULL);
            LOGI(TAG, "CCCD subscribe rc=%d", rc);
        } else {
            LOGW(TAG, "no CCCD found -> 无法订阅从机时间戳");
        }
        calib_send_ping();
    }
    return 0;
}

static int chr_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;
    if (error->status == 0 && chr) {
        if (chr->uuid.u.type == BLE_UUID_TYPE_16 &&
            chr->uuid.u16.value == s_cmd_uuid.value) {
            s_cmd_handle = chr->val_handle;
            LOGI(TAG, "found cmd chr val_handle=%u", s_cmd_handle);
        } else if (chr->uuid.u.type == BLE_UUID_TYPE_16 &&
                   chr->uuid.u16.value == s_resp_uuid.value) {
            s_resp_handle = chr->val_handle;
            LOGI(TAG, "found resp chr val_handle=%u", s_resp_handle);
        }
    } else if (error->status == BLE_HS_EDONE) {
        LOGI(TAG, "chr discovery complete cmd=%u resp=%u", s_cmd_handle, s_resp_handle);
        if (s_resp_handle) {
            ble_gattc_disc_all_dscs(conn_handle, s_resp_handle, s_resp_handle + 8,
                                    dsc_disc_cb, NULL);
        } else {
            calib_send_ping();
        }
    }
    return 0;
}

static int svc_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_svc *service, void *arg)
{
    (void)arg;
    if (error->status == 0 && service) {
        if (service->uuid.u.type == BLE_UUID_TYPE_16 &&
            service->uuid.u16.value == s_svc_uuid.value) {
            LOGI(TAG, "found svc start=%u end=%u",
                 service->start_handle, service->end_handle);
            ble_gattc_disc_all_chrs(conn_handle, service->start_handle,
                                    service->end_handle, chr_disc_cb, NULL);
        }
    } else if (error->status == BLE_HS_EDONE) {
        LOGI(TAG, "svc discovery complete");
    }
    return 0;
}

/* ============================== 钟差标定 ==============================
 * 主机发 PING（记 t_tx），从机回 NOTIFY "PONG,<t_slave_us>"。
 * 主机收到时记 t_rx，RTT = t_rx - t_tx，
 *   offset_cand = t_slave - (t_tx + RTT/2)
 * **取 RTT 最小的那个样本**（此时 d1≈d2≈0，对称假设误差最小）作为最终 offset。
 * 误差量级：|d1-d2|/2，最小 RTT 样本下通常几十 µs。 */
static void calib_send_ping(void)
{
    if (!s_connected || s_cmd_handle == 0) {
        return;
    }
    if (s_calib_cnt >= CALIB_SAMPLES) {
        return;
    }
    char cmd[16];
    snprintf(cmd, sizeof(cmd), "PING");
    s_ping_tx = now_us();
    struct os_mbuf *om = ble_hs_mbuf_from_flat(cmd, strlen(cmd));
    if (om) {
        ble_gattc_write_no_rsp(s_conn_handle, s_cmd_handle, om);
        s_calib_cnt++;
    }
}

static void calib_on_pong(const char *buf)
{
    if (strncmp(buf, "PONG,", 5) != 0) {
        return;
    }
    int64_t t_slave = strtoll(buf + 5, NULL, 10);
    int64_t t_rx    = now_us();
    int64_t rtt     = t_rx - s_ping_tx;
    int64_t cand    = t_slave - (s_ping_tx + rtt / 2);

    if (!s_offset_valid || rtt < s_best_rtt) {
        s_best_rtt     = rtt;
        s_offset       = cand;
        s_offset_valid = true;
        LOGI(TAG, "calib sample -> offset=%lldus rtt=%lldus (n=%d)",
             (long long)cand, (long long)rtt, s_calib_cnt);
    }

    if (s_calib_cnt < CALIB_SAMPLES) {
        vTaskDelay(pdMS_TO_TICKS(10));
        calib_send_ping();
    } else if (s_calib_cnt == CALIB_SAMPLES) {
        s_calib_cnt++;  /* 只打一次 */
        csv_line("CALIB,%lld,%lld", (long long)s_offset, (long long)s_best_rtt);
        LOGI(TAG, "★ calibration done: offset=%lldus best_rtt=%lldus",
             (long long)s_offset, (long long)s_best_rtt);
    }
}

/* ============================== GAP 事件 ============================== */
static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        if (s_connected || s_connecting) {
            return 0;
        }
        const struct ble_gap_disc_desc *d = &event->disc;

        /* 名字以 "SLAVE-" 开头 或 含 service UUID 0xABF0 */
        const uint8_t *data = d->data;
        int len = d->length_data;
        bool match = false;
        int i = 0;
        while (i + 2 <= len) {
            uint8_t flen = data[i];
            if (flen == 0 || i + 1 + flen > len) {
                break;
            }
            uint8_t type = data[i + 1];
            const uint8_t *val = &data[i + 2];
            int vlen = flen - 1;
            if ((type == 0x09 || type == 0x08) && vlen >= 6 &&
                strncmp((const char *)val, "SLAVE-", 6) == 0) {
                match = true;
            } else if (type == 0x03 && vlen >= 2) {
                uint16_t u = (uint16_t)val[0] | ((uint16_t)val[1] << 8);
                if (u == BLE_SYNC_SVC_UUID) {
                    match = true;
                }
            }
            i += 1 + flen;
        }
        if (!match) {
            return 0;
        }

        LOGI(TAG, "found slave %02x:%02x:%02x:%02x:%02x:%02x",
             d->addr.val[5], d->addr.val[4], d->addr.val[3],
             d->addr.val[2], d->addr.val[1], d->addr.val[0]);

        ble_gap_disc_cancel();
        s_scanning = false;

        int rc = ble_gap_connect(s_own_addr_type, &d->addr,
                                 BLE_SCAN_DURATION_MS, NULL, gap_event, NULL);
        if (rc == 0) {
            s_connecting = true;
            LOGI(TAG, "connecting...");
        } else {
            LOGW(TAG, "connect rc=%d", rc);
        }
        return 0;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_scanning = false;
        if (!s_connecting && !s_connected && s_inited && !s_reconnect_pending) {
            s_reconnect_pending = true;
            esp_timer_start_once(s_reconnect_timer, BLE_RECONNECT_INTERVAL_MS * 1000);
        }
        return 0;

    case BLE_GAP_EVENT_CONNECT: {
        s_connecting = false;
        if (event->connect.status == 0) {
            s_connected = true;
            s_conn_handle = event->connect.conn_handle;
            LOGI(TAG, "connected handle=%d", event->connect.conn_handle);
            csv_line("BLE_CONNECTED,%lld", (long long)now_us());

            struct ble_gap_upd_params upd = {
                .itvl_min = BLE_CONN_ITVL_UNITS,
                .itvl_max = BLE_CONN_ITVL_UNITS,
                .latency  = BLE_CONN_LATENCY,
                .supervision_timeout = BLE_SUPERVISION_TIMEOUT_MS / 10,
            };
            ble_gap_update_params(event->connect.conn_handle, &upd);

            /* 主动发起 MTU 协商（否则默认 23，从机回传的长通知会被截断） */
            ble_gattc_exchange_mtu(event->connect.conn_handle, NULL, NULL);

            s_cmd_handle  = 0;
            s_resp_handle = 0;
            s_cccd_handle = 0;
            s_offset_valid = false;
            s_calib_cnt    = 0;
            s_best_rtt     = INT64_MAX;
            ble_gattc_disc_all_svcs(event->connect.conn_handle, svc_disc_cb, NULL);
        } else {
            LOGW(TAG, "connect failed status=%d", event->connect.status);
            s_connected = false;
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            if (!s_reconnect_pending) {
                s_reconnect_pending = true;
                esp_timer_start_once(s_reconnect_timer, BLE_RECONNECT_INTERVAL_MS * 1000);
            }
        }
        return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT:
        LOGI(TAG, "disconnected reason=%d", event->disconnect.reason);
        s_connected = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_cmd_handle = 0;
        s_resp_handle = 0;
        s_cccd_handle = 0;
        s_offset_valid = false;      /* 断连后钟差失效，重连要重新标定 */
        s_calib_cnt = 0;
        s_connecting = false;
        csv_line("BLE_DISCONNECTED,%lld,%d",
                 (long long)now_us(), (int)event->disconnect.reason);
        if (!s_reconnect_pending) {
            s_reconnect_pending = true;
            esp_timer_start_once(s_reconnect_timer, BLE_RECONNECT_INTERVAL_MS * 1000);
        }
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE: {
        struct ble_gap_conn_desc d;
        if (event->conn_update.status == 0 &&
            ble_gap_conn_find(event->conn_update.conn_handle, &d) == 0) {
            LOGI(TAG, "conn updated itvl=%dms sup=%dms",
                 d.conn_itvl * 125 / 100, d.supervision_timeout * 10);
        }
        return 0;
    }

    case BLE_GAP_EVENT_MTU:
        LOGI(TAG, "mtu conn=%d mtu=%d", event->mtu.conn_handle, event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        char buf[96];
        uint16_t len = 0;
        if (ble_hs_mbuf_to_flat(event->notify_rx.om, buf, sizeof(buf) - 1, &len) == 0) {
            buf[len] = '\0';
            if (strncmp(buf, "PONG,", 5) == 0) {
                calib_on_pong(buf);          /* 钟差标定样本 */
            } else if (strncmp(buf, "E,", 2) == 0) {
                csv_line_slave("%s", buf + 2);  /* 从机事件 -> 转发到本机串口 */
            }
        }
        return 0;
    }

    default:
        return 0;
    }
}

/* ============================== 对外 API ============================== */
bool ble_host_connected(void)
{
    return s_connected;
}

bool ble_host_snap_now(void)
{
    return capture_once(now_us());
}

void ble_host_set_upload_mode(bool on)
{
    /* 1) 暂停扫描（仅在"还没连上从机"时有意义：已连接时本来就不扫） */
    s_scan_paused = on;
    if (on && s_scanning) {
        ble_gap_disc_cancel();
        s_scanning = false;
    }

    /* 2) 放宽连接间隔 15ms -> 100ms，并把新参数上报给对端（Central 发起）。
     *    单位 1.25ms：80 = 100ms。
     *    ★ 注：实测这一步对端并未接受（日志里始终没有 conn updated），
     *      但它无害；主机上传失败的真因是 WiFi 长时间不关联（见 upload_telegram.c
     *      里对"等 WiFi"的处理），与 BLE 连接本身无关。 */
    if (s_connected && s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        struct ble_gap_upd_params p = {
            .itvl_min = on ? 80 : BLE_CONN_ITVL_UNITS,
            .itvl_max = on ? 80 : BLE_CONN_ITVL_UNITS,
            .latency  = BLE_CONN_LATENCY,
            .supervision_timeout = BLE_SUPERVISION_TIMEOUT_MS / 10,
        };
        int rc = ble_gap_update_params(s_conn_handle, &p);
        LOGI(TAG, "upload mode %s: conn itvl -> %dms (rc=%d)",
             on ? "ON" : "OFF", on ? 100 : BLE_CONN_INTERVAL_MS, rc);
    } else {
        LOGI(TAG, "upload mode %s (no connection, scan %s)",
             on ? "ON" : "OFF", on ? "paused" : "resumed");
    }
}

esp_err_t ble_host_arm_fire(int64_t t_host_target, int lead_ms)
{
    if (!s_connected || s_cmd_handle == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    /* ---- 1) 本机定时：用"剩余时间"，保证 jitter 只反映 esp_timer 精度 ---- */
    s_target_us = t_host_target;
    esp_timer_stop(s_fire_timer);
    int64_t delta = t_host_target - now_us();
    if (delta < 1) {
        delta = 1;
    }
    esp_timer_start_once(s_fire_timer, (uint64_t)delta);

    /* ---- 2) 通知从机"在你自己的时钟 T 触发"（绝对时刻，提前量足够） ---- */
    char cmd[48];
    if (s_offset_valid) {
        int64_t t_slave_target = t_host_target + s_offset;
        snprintf(cmd, sizeof(cmd), "ARM,%lld", (long long)t_slave_target);
        csv_line("FIRE_SCHEDULED,%lld,%lld",
                 (long long)t_host_target, (long long)t_slave_target);
    } else {
        /* 还没标定成功 -> 退回相对 lead 模式（精度差，但至少能动） */
        snprintf(cmd, sizeof(cmd), "FIRE,%d", lead_ms);
        csv_line("FIRE_SCHEDULED,%lld,%lld",
                 (long long)(t_host_target - (int64_t)lead_ms * 1000), (long long)t_host_target);
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(cmd, strlen(cmd));
    if (!om) {
        return ESP_ERR_NO_MEM;
    }
    int rc = ble_gattc_write_no_rsp(s_conn_handle, s_cmd_handle, om);
    if (rc != 0) {
        LOGE(TAG, "write cmd \"%s\" rc=%d", cmd, rc);
        return ESP_FAIL;
    }
    LOGI(TAG, "ARM sent \"%s\" (offset_valid=%d)", cmd, (int)s_offset_valid);
    return ESP_OK;
}

esp_err_t ble_host_send_fire(const char *cmd)
{
    if (!s_connected || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_cmd_handle == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat((void *)cmd, strlen(cmd));
    if (!om) {
        return ESP_ERR_NO_MEM;
    }
    int rc = ble_gattc_write_no_rsp(s_conn_handle, s_cmd_handle, om);
    if (rc != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ============================== host 回调 ============================== */
static void host_task(void *param)
{
    (void)param;
    LOGI(TAG, "nimble host task started");
    nimble_port_run();               /* 只在 nimble_port_stop() 后返回 */
    nimble_port_freertos_deinit();
}

static void on_reset(int reason)
{
    LOGE(TAG, "host reset reason=%d", reason);
    s_connected = false;
    s_scanning = false;
    s_connecting = false;
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        LOGE(TAG, "ensure_addr rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        LOGE(TAG, "id_infer_auto rc=%d", rc);
        return;
    }
    uint8_t addr[6] = {0};
    ble_hs_id_copy_addr(s_own_addr_type, addr, NULL);
    LOGI(TAG, "sync ok, own addr %02x:%02x:%02x:%02x:%02x:%02x -> scan",
         addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);

    start_scan();
}

/* ============================== init ============================== */
esp_err_t ble_host_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    s_csv_lock = xSemaphoreCreateMutex();
    if (!s_csv_lock) {
        return ESP_ERR_NO_MEM;
    }

    marker_init();

    {
        const esp_timer_create_args_t args = {
            .callback = fire_timer_cb,
            .arg      = NULL,
            .name     = "ble_host_fire",
            .dispatch_method = ESP_TIMER_TASK,
        };
        esp_err_t e = esp_timer_create(&args, &s_fire_timer);
        if (e != ESP_OK) {
            LOGE(TAG, "esp_timer_create fail err=0x%x", e);
            return e;
        }
    }
    {
        const esp_timer_create_args_t args = {
            .callback = reconnect_timer_cb,
            .arg      = NULL,
            .name     = "ble_host_reconnect",
            .dispatch_method = ESP_TIMER_TASK,
        };
        esp_err_t e = esp_timer_create(&args, &s_reconnect_timer);
        if (e != ESP_OK) {
            LOGE(TAG, "reconnect timer create fail err=0x%x", e);
            return e;
        }
    }

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        LOGE(TAG, "nimble_port_init fail err=0x%x", ret);
        return ret;
    }

    /* ★ 申请 247 字节 MTU：默认 23 时从机回传的
     *   "E,FIRE_ACTUAL,<10位µs>,<jitter>"（25 字节）会被静默截断。 */
    ble_att_set_preferred_mtu(247);

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.gatts_register_cb = NULL;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    {
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        char name[24];
        snprintf(name, sizeof(name), "HOST-%02X%02X", mac[4], mac[5]);
        int nrc = ble_svc_gap_device_name_set(name);
        if (nrc != 0) {
            LOGW(TAG, "device_name_set rc=%d", nrc);
        }
    }

    if (xTaskCreate(sync_task, "ble_host_sync", 4096, NULL, 5, &s_sync_task) != pdPASS) {
        LOGE(TAG, "sync_task create fail");
        s_sync_task = NULL;
    }

    /* ★ 必须启动 NimBLE host task，否则 sync_cb 永远不会被调用（不会开始扫描）。
     *   ❌ 不要传 NULL：该参数直接作为 FreeRTOS 任务入口，NULL 会导致 PC=0x0 崩溃。 */
    nimble_port_freertos_init(host_task);

    /* 心跳任务（给网页/脚本判断"当前是否已连接"用） */
    xTaskCreate(hb_task, "ble_host_hb", 3072, NULL, 3, NULL);

    s_inited = true;
    csv_line("BOOT,%lld", (long long)now_us());
    LOGI(TAG, "host init ok marker=GPIO%d", BLE_SYNC_MARKER_GPIO);

    return ESP_OK;
}
