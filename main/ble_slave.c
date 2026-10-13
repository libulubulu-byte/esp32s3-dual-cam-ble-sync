/**
 * @file ble_slave.c
 * @brief 从机：NimBLE GATT Server + 广播 + FIRE 定时拍照
 *
 * CSV 日志经 UART0（115200）输出，行首固定为 "SLAVE,"，便于 PC 脚本过滤：
 *   SLAVE,BOOT,<t_us>
 *   SLAVE,BLE_CONNECTED,<t_us>
 *   SLAVE,BLE_DISCONNECTED,<t_us>,<reason>
 *   SLAVE,RX_BLE,<t_rx_us>,<lead_ms>
 *   SLAVE,FIRE_SCHEDULED,<t_rx_us>,<target_us>
 *   SLAVE,FIRE_ACTUAL,<t_fire_us>,<jitter_us>
 *   SLAVE,DONE,<t_done_us>
 */

#include "ble_slave.h"
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

static const char *TAG = LOG_T_BLE;

#define CMD_MAX   64
#define PHOTO_PREFIX "slave"

static const ble_uuid16_t s_svc_uuid  = BLE_UUID16_INIT(BLE_SYNC_SVC_UUID);
static const ble_uuid16_t s_cmd_uuid  = BLE_UUID16_INIT(BLE_SYNC_CMD_UUID);
static const ble_uuid16_t s_resp_uuid = BLE_UUID16_INIT(BLE_SYNC_RESP_UUID);

/* ============================== 状态 ============================== */
static bool                     s_inited;
static volatile bool            s_connected;
static uint16_t                 s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint8_t                  s_own_addr_type;
static uint16_t                 s_cmd_handle;
static uint16_t                 s_resp_handle;
static char                     s_resp[160];
static SemaphoreHandle_t        s_resp_lock;
static SemaphoreHandle_t        s_csv_lock;

static esp_timer_handle_t       s_timer;
static volatile bool            s_armed;
static volatile int64_t         s_target_us;
static volatile int64_t         s_fire_ts;
static volatile int64_t         s_fire_jitter;
static TaskHandle_t             s_sync_task;

/* ============================== 工具 ============================== */
static int64_t now_us(void)
{
    return esp_timer_get_time();
}

/** CSV 日志：行首固定 "SLAVE,"，带锁避免多任务交错 */
static void csv_line(const char *fmt, ...)
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

static void set_resp(const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (s_resp_lock && xSemaphoreTake(s_resp_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        strlcpy(s_resp, buf, sizeof(s_resp));
        xSemaphoreGive(s_resp_lock);
    } else {
        strlcpy(s_resp, buf, sizeof(s_resp));
    }

    if (s_connected) {
        ble_gatts_chr_updated(s_resp_handle);
    }
}

/* ============================== 拍照 ============================== */
/**
 * 抓一帧后：① 有 SD 就落盘；② 投递 Telegram 上传（**不依赖 SD**）。
 * ★ 注意：本函数会在**没有 SD 卡时也抓帧** —— 上传只需要 PSRAM 里的 JPEG。
 */
static bool capture_once(int64_t t_fire)
{
    camera_fb_t *fb = camera_hal_grab(2000);
    if (!fb) {
        LOGE(TAG, "grab fail");
        return false;
    }

    bool ok = false;

    /* ① 落 SD（插了卡才做） */
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

    /* ② 投递 Telegram 上传（拷贝进 PSRAM 后立即返回，不阻塞） */
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

        int64_t t_fire   = s_fire_ts;
        int64_t jitter   = s_fire_jitter;

        csv_line("FIRE_ACTUAL,%lld,%lld", (long long)t_fire, (long long)jitter);

        /* ★★ 经 BLE Notify 回传给主机（主机串口打印成 "SLAVE,..."）。
         *   这样 PC 只连主机一个串口就能拿到两板的同步数据（网页验收用）。
         *   前缀 "E," 与钟差标定的 "PONG," 区分开。
         *   ★ 时间戳是**从机本地时钟**，与主机时钟差一个常量 —— 测试端用
         *     |Δ - median(Δ)| 抵消掉即可（详见 README §6）。 */
        set_resp("E,FIRE_ACTUAL,%lld,%lld", (long long)t_fire, (long long)jitter);

        int64_t t0 = now_us();
        bool ok = capture_once(t_fire);
        int64_t took = (now_us() - t0) / 1000;

        csv_line("DONE,%lld", (long long)now_us());
        set_resp("E,DONE,%lld", (long long)now_us());
        LOGI(TAG, "capture done ok=%d took=%lldms jitter=%+lldus",
             (int)ok, (long long)took, (long long)jitter);

        marker_set(0);
    }
}

/* ============================== esp_timer 回调（硬实时：只记录） ============================== */
static void timer_cb(void *arg)
{
    (void)arg;
    s_fire_ts     = now_us();
    s_fire_jitter = s_fire_ts - s_target_us;
    s_armed       = false;
    marker_set(1);
    if (s_sync_task) {
        xTaskNotifyGive(s_sync_task);
    }
}

/* ============================== GATT ============================== */
static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)arg;

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        if (attr_handle != s_cmd_handle) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        char buf[CMD_MAX + 1];
        uint16_t len = 0;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf) - 1, &len);
        if (rc != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        buf[len] = '\0';

        if (strncasecmp(buf, "ARM,", 4) == 0) {
            /* ★★ 主机下发的"绝对目标时刻"（**从机自己的时钟**）。
             *   关键收益：从机不再"收到就计时"，所以 BLE 传输延迟的抖动
             *   （连接间隔决定，实测 ~12ms）**完全不进入触发时刻**。
             *   抖动只剩：钟差估计误差 + 两板晶振频率漂移 + 定时器调度精度。 */
            int64_t T = strtoll(buf + 4, NULL, 10);
            int64_t t_rx = now_us();
            int64_t delta = T - t_rx;
            if (delta < 1) {
                delta = 1;
            }

            s_target_us = T;
            s_armed     = true;

            esp_timer_stop(s_timer);
            esp_timer_start_once(s_timer, (uint64_t)delta);

            csv_line("RX_BLE,%lld,ARM", (long long)t_rx);
            csv_line("FIRE_SCHEDULED,%lld,%lld", (long long)t_rx, (long long)T);

            set_resp("ACK ARM");
        } else if (strncasecmp(buf, "FIRE,", 5) == 0) {
            /* 兼容"相对 lead"模式（不做钟差标定时使用） */
            int lead_ms = atoi(buf + 5);
            if (lead_ms < MIN_LEAD_MS)  lead_ms = MIN_LEAD_MS;
            if (lead_ms > MAX_LEAD_MS)  lead_ms = MAX_LEAD_MS;

            int64_t t_rx   = now_us();
            int64_t target = t_rx + (int64_t)lead_ms * 1000;

            s_target_us = target;
            s_armed     = true;

            esp_timer_stop(s_timer);
            int64_t delta = target - now_us();
            if (delta < 1) {
                delta = 1;
            }
            esp_timer_start_once(s_timer, (uint64_t)delta);

            csv_line("RX_BLE,%lld,%d", (long long)t_rx, lead_ms);
            csv_line("FIRE_SCHEDULED,%lld,%lld", (long long)t_rx, (long long)target);

            set_resp("ACK lead=%d", lead_ms);
        } else if (strcasecmp(buf, "PING") == 0) {
            /* ★ 回带从机自己的时间戳，供主机估计钟差（t_slave = t_host + offset） */
            set_resp("PONG,%lld", (long long)now_us());
        } else {
            set_resp("ERR UNKNOWN_CMD");
        }
        return 0;
    }

    case BLE_GATT_ACCESS_OP_READ_CHR: {
        if (attr_handle != s_resp_handle) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        char buf[160];
        if (s_resp_lock && xSemaphoreTake(s_resp_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
            strlcpy(buf, s_resp, sizeof(buf));
            xSemaphoreGive(s_resp_lock);
        } else {
            strlcpy(buf, s_resp, sizeof(buf));
        }
        int rc = os_mbuf_append(ctxt->om, buf, strlen(buf));
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid       = &s_cmd_uuid.u,
                .access_cb  = gatt_access,
                .flags      = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
                .val_handle = &s_cmd_handle,
            },
            {
                .uuid       = &s_resp_uuid.u,
                .access_cb  = gatt_access,
                .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_resp_handle,
            },
            { 0 },
        },
    },
    { 0 },
};

static void gatt_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
    (void)arg;
    char buf[BLE_UUID_STR_LEN];
    switch (ctxt->op) {
    case BLE_GATT_REGISTER_OP_SVC:
        LOGI(TAG, "svc registered %s handle=%d",
             ble_uuid_to_str(ctxt->svc.svc_def->uuid, buf), ctxt->svc.handle);
        break;
    case BLE_GATT_REGISTER_OP_CHR:
        LOGI(TAG, "chr registered %s def=%d val=%d",
             ble_uuid_to_str(ctxt->chr.chr_def->uuid, buf),
             ctxt->chr.def_handle, ctxt->chr.val_handle);
        break;
    default:
        break;
    }
}

/* ============================== 广播 / GAP ============================== */
static int ble_sync_gap_event(struct ble_gap_event *event, void *arg);

static void advertise(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    const char *name;
    int rc;

    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    static const ble_uuid16_t adv_uuid = BLE_UUID16_INIT(BLE_SYNC_SVC_UUID);
    fields.uuids16 = &adv_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        LOGE(TAG, "adv_set_fields rc=%d", rc);
        return;
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_sync_gap_event, NULL);
    if (rc != 0) {
        LOGE(TAG, "adv_start rc=%d", rc);
    } else {
        LOGI(TAG, "advertising started");
    }
}

static int ble_sync_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_connected = true;
            s_conn_handle = event->connect.conn_handle;
            LOGI(TAG, "connected handle=%d", event->connect.conn_handle);
            csv_line("BLE_CONNECTED,%lld", (long long)now_us());
            set_resp("READY");
        } else {
            LOGW(TAG, "connect failed status=%d -> continue adv", event->connect.status);
            advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        LOGI(TAG, "disconnected reason=%d", event->disconnect.reason);
        s_connected = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        csv_line("BLE_DISCONNECTED,%lld,%d",
                 (long long)now_us(), (int)event->disconnect.reason);
        if (s_armed) {
            esp_timer_stop(s_timer);
            s_armed = false;
        }
        advertise();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        LOGI(TAG, "adv complete reason=%d -> re-advertise", event->adv_complete.reason);
        advertise();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        LOGI(TAG, "subscribe attr=%d notify=%d", event->subscribe.attr_handle,
             event->subscribe.cur_notify);
        return 0;

    case BLE_GAP_EVENT_MTU:
        LOGI(TAG, "mtu conn=%d mtu=%d", event->mtu.conn_handle, event->mtu.value);
        return 0;

    default:
        return 0;
    }
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
    LOGI(TAG, "sync ok, own addr %02x:%02x:%02x:%02x:%02x:%02x -> advertise",
         addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);

    advertise();
}

/* ============================== init ============================== */
esp_err_t ble_slave_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    s_resp_lock = xSemaphoreCreateMutex();
    s_csv_lock  = xSemaphoreCreateMutex();
    if (!s_resp_lock || !s_csv_lock) {
        return ESP_ERR_NO_MEM;
    }
    strlcpy(s_resp, "BOOT", sizeof(s_resp));

    marker_init();

    {
        const esp_timer_create_args_t args = {
            .callback = timer_cb,
            .arg      = NULL,
            .name     = "ble_slave",
            .dispatch_method = ESP_TIMER_TASK,
        };
        esp_err_t e = esp_timer_create(&args, &s_timer);
        if (e != ESP_OK) {
            LOGE(TAG, "esp_timer_create fail err=0x%x", e);
            return e;
        }
    }

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        LOGE(TAG, "nimble_port_init fail err=0x%x", ret);
        return ret;
    }

    /* ★ 申请 247 字节 MTU。默认 MTU=23 时一条通知最多带 20 字节，
     *   "E,FIRE_ACTUAL,<10位µs>,<jitter>" 有 25 字节 -> 会被**静默截断**
     *   （实测只收到 "FIRE_ACTUAL,247611"，后半段丢失，且没有任何报错）。 */
    ble_att_set_preferred_mtu(247);

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.gatts_register_cb = gatt_register_cb;

    ble_svc_gap_init();

    {
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        char name[24];
        snprintf(name, sizeof(name), "SLAVE-%02X%02X", mac[4], mac[5]);
        int nrc = ble_svc_gap_device_name_set(name);
        if (nrc != 0) {
            LOGW(TAG, "device_name_set rc=%d", nrc);
        }
    }

    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc != 0) {
        LOGE(TAG, "gatts_count_cfg rc=%d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(s_svcs);
    if (rc != 0) {
        LOGE(TAG, "gatts_add_svcs rc=%d", rc);
        return ESP_FAIL;
    }

    nimble_port_freertos_init(host_task);

    if (xTaskCreate(sync_task, "ble_slave_sync", 4096, NULL, 5, &s_sync_task) != pdPASS) {
        LOGE(TAG, "sync_task create fail");
        s_sync_task = NULL;
    }

    s_inited = true;
    csv_line("BOOT,%lld", (long long)now_us());
    LOGI(TAG, "init ok svc=0x%04X cmd=0x%04X resp=0x%04X marker=GPIO%d",
         BLE_SYNC_SVC_UUID, BLE_SYNC_CMD_UUID, BLE_SYNC_RESP_UUID,
         BLE_SYNC_MARKER_GPIO);

    return ESP_OK;
}

bool ble_slave_connected(void)
{
    return s_connected;
}

bool ble_slave_snap_now(void)
{
    return capture_once(now_us());
}
