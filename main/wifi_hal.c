/**
 * @file wifi_hal.c
 * @brief 极简 WiFi STA（只为照片上传提供外网出口）
 *
 * 与参考工程 two_demo_SD_who_esp32S3_CAM 的差别：**去掉了 AP 配网 / 配网页 /
 * config_store**，SSID/密码直接取 app_conf.h 的宏（本工程无运行期配置系统）。
 *
 * 保留的两个重要经验（都来自现场踩坑）：
 *   1) **发射功率默认降到 11 dBm**（不是 20）：ESP32-S3 起射频那一瞬电流冲高，
 *      现场出现过 BROWNOUT 复位；降功率后消失。
 *   2) 事件回调里**不做重活**，只置标志 + 打日志；耗时操作交给驱动内部重连。
 */

#include "wifi_hal.h"
#include "app_conf.h"
#include "logger.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"

static const char *TAG = "WIFI";

static volatile bool s_connected;
static char          s_ssid[33];
static esp_netif_t  *s_sta_netif;

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        s_connected = false;
        LOGW(TAG, "disconnected reason=%d -> 重新连接", d ? d->reason : -1);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        s_connected = true;
        LOGI(TAG, "got ip " IPSTR " ssid=%s", IP2STR(&e->ip_info.ip), s_ssid);

        /* ★ 现场诊断：主机出现过 reason=201 (NO_AP_FOUND) / 205 / 2，链路极差却
         *   查不到原因。把 RSSI 与信道打出来，一眼区分是"接收太弱/被遮挡"
         *   还是"射频共存"。注意 RSSI 是**接收**强度，与 txmax(发射) 无关。 */
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            LOGI(TAG, "AP rssi=%d dBm  ch=%d", ap.rssi, ap.primary);
        }

        /* ★★ 显式指定公共 DNS，绕开热点透传下来的那台。
         *   踩坑实录：Windows 移动热点(ICS)会把**宿主机**的 DNS 原样透传给
         *   客户端。如果宿主机上跑着 Clash/v2ray 之类代理的 TUN 模式，DNS 会
         *   返回 fake-ip（实测 api.telegram.org -> 198.18.0.41，RFC2544 保留段）。
         *   那个地址只有宿主机的代理才认识，ESP32 直连必然失败：表现为
         *   TG: open 失败 0x7002，且每次都卡满 TCP 超时（~20 s）。
         *   ——与 BLE / 射频 / 内存都无关，纯 DNS 被劫持。 */
        if (APP_WIFI_DNS[0] != '\0') {
            esp_netif_dns_info_t d = {0};
            d.ip.type = ESP_IPADDR_TYPE_V4;
            esp_netif_str_to_ip4(APP_WIFI_DNS, &d.ip.u_addr.ip4);
            esp_err_t de = esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &d);
            LOGI(TAG, "DNS 覆盖为 %s (rc=0x%x)", APP_WIFI_DNS, de);
        }
    }
}

esp_err_t wifi_hal_init(void)
{
    if (APP_WIFI_SSID[0] == '\0') {
        LOGW(TAG, "未配置 WiFi（APP_WIFI_SSID 为空）-> 跳过 WiFi，仅本地同步拍照");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (!s_sta_netif) {
        LOGE(TAG, "create_default_wifi_sta 失败");
        return ESP_FAIL;
    }

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t e = esp_wifi_init(&ic);
    if (e != ESP_OK) {
        LOGE(TAG, "esp_wifi_init 失败 0x%x", e);
        return e;
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        on_wifi_event, NULL, NULL));

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, APP_WIFI_SSID, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, APP_WIFI_PASS, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;   /* 允许开放网络 */
    strlcpy(s_ssid, APP_WIFI_SSID, sizeof(s_ssid));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));

    e = esp_wifi_start();
    if (e != ESP_OK) {
        LOGE(TAG, "esp_wifi_start 失败 0x%x", e);
        return e;
    }

    /* ★★ 以下两项**必须在 esp_wifi_start() 之后**调用：
     *   在 start 之前调 esp_wifi_set_max_tx_power() 会返回 ESP_ERR_WIFI_NOT_STARTED，
     *   而 ESP_ERROR_CHECK 会直接 abort() -> 开机复位循环（已踩，见 backtrace 定位）。
     *   另外这里**不用 ESP_ERROR_CHECK**：设功率失败不该让整机起不来。 */
    e = esp_wifi_set_max_tx_power(APP_WIFI_MAX_TX_DBM * 4);
    if (e != ESP_OK) {
        LOGW(TAG, "set_max_tx_power(%ddBm) 失败 0x%x（保持默认功率）",
             APP_WIFI_MAX_TX_DBM, e);
    }

    /* 省电模式关掉：modem sleep 会让 TLS 往返偶发卡顿/超时 */
    e = esp_wifi_set_ps(WIFI_PS_NONE);
    if (e != ESP_OK) {
        LOGW(TAG, "set_ps(NONE) 失败 0x%x", e);
    }

    LOGI(TAG, "STA 启动 ssid=%s txmax=%ddBm（等待获取 IP）", s_ssid, APP_WIFI_MAX_TX_DBM);
    return ESP_OK;
}

bool wifi_hal_connected(void)
{
    return s_connected;
}

const char *wifi_hal_ssid(void)
{
    return s_ssid;
}
