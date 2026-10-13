/**
 * @file upload_telegram.c
 * @brief Telegram Bot sendPhoto（multipart/form-data）+ 异步上传队列
 *
 * 移植自 two_demo_SD_who_esp32S3_CAM/main/upload_telegram.c，两条**关键经验**
 * 原样保留（都被现场踩过，别再改回去）：
 *
 * ★★ 1) `esp_http_client_perform()` **会把响应体整个读掉并丢弃**
 *     （IDF esp_http_client.c 里 perform() 末尾两个 get_data 循环），
 *     之后 read_response 只能拿到 0 字节 ⇒ `strstr(resp,"\"ok\":true")` 永远不成立。
 *     ⇒ 必须手工三段式：open(len) → write(body) → fetch_headers → read_response → close。
 *
 * ★★ 2) Telegram **出错时也返回 HTTP 200**，真正的成败在 body 的 `"ok":true` 里
 *     ⇒ 只判 status==200 会把失败当成功。
 */

#include "upload_telegram.h"
#include "app_conf.h"
#include "logger.h"
#include "wifi_hal.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_system.h"        /* esp_get_free_heap_size() */

#include <netdb.h>             /* getaddrinfo()，DNS 诊断用 */
#include <arpa/inet.h>         /* inet_ntoa() */
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

static const char *TAG = "TG";

/* ★ 上传模式：主机侧暂停扫描 + 把连接间隔从 15ms 放宽到 100ms，把射频让给
 *   WiFi。从机是 Peripheral、没有这两项负担，编译期直接空操作。
 *
 *   ★★ 这里**不能用弱符号**：实测主机侧 upload_telegram.o 解析到了那个空
 *   实现，整个优化静默失效（日志里一行 "upload mode" 都没有，上传照旧
 *   15s 超时）。同一个组件库里同时存在强/弱定义时，不要指望链接器一定
 *   选中强符号 —— 用 #if 在编译期定死。 */
#if BOARD_ROLE_HOST
#include "ble_host.h"
#define UPLOAD_SET_MODE(on)   ble_host_set_upload_mode(on)
#else
#define UPLOAD_SET_MODE(on)   ((void)(on))
#endif

#if BOARD_ROLE_HOST
#define BOARD_ID_STR "HOST"
#else
#define BOARD_ID_STR "SLAVE"
#endif

typedef struct {
    uint8_t *buf;
    size_t   len;
    int64_t  t_fire;
    uint32_t seq;          /* ★ 本板拍照序号（从 1 起），写进 caption 便于对账 */
} up_item_t;

static QueueHandle_t s_q;
static uint32_t      s_ok, s_fail;
static int64_t       s_last_send_ms;

/* ★★ 上传被降级成**低优先级后台任务**：
 *    拍照只入队；只有静默 TG_IDLE_BEFORE_UPLOAD_MS 没有新拍照才真正开传；
 *    一有新拍照就打断在传的那张（分块写，每块前检查 s_abort）。
 *    好处：连续拍 N 张的整个过程中，射频完全留给 BLE + 摄像头，
 *    同步精度不会被上传的 WiFi/TLS 流量干扰。 */
static volatile int64_t  s_last_shot_us;   /* 最后一次投递照片的时刻 */
static volatile uint32_t s_shot_gen;       /* 每次投递递增（诊断用） */
static volatile bool     s_abort;          /* true = 有新拍照，打断当前上传 */
static volatile uint32_t s_shot_seq;       /* ★ 拍照序号，写进 caption（HOST #3 / SLAVE #3） */

/* ============================== multipart sendPhoto ============================== */
static esp_err_t tg_send_photo_once(const char *chat_id, const char *caption,
                                    const uint8_t *jpeg, size_t jpeg_len)
{
    /* ★ 每块板打印一次实际解析结果。现场踩坑：宿主机上跑着 Clash/v2ray 的
     *   TUN 模式时，Windows 移动热点会把 fake-ip（198.18.x.x）透传给客户端，
     *   连显式指定 223.5.5.5 也会被 TUN 重定向。对比主机/从机解析到哪个 IP，
     *   就能判断是"解析被劫持"还是"流量路径本身不同"。 */
    static bool s_dns_logged;
    if (!s_dns_logged) {
        s_dns_logged = true;
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        int gai = getaddrinfo("api.telegram.org", "443", &hints, &res);
        if (gai == 0 && res) {
            LOGI(TAG, "DNS(首次) api.telegram.org -> %s",
                 inet_ntoa(((struct sockaddr_in *)res->ai_addr)->sin_addr));
            freeaddrinfo(res);
        } else {
            LOGE(TAG, "DNS(首次) 解析失败 gai=%d", gai);
        }
    }

    char url[256];
    snprintf(url, sizeof(url), "%s/bot%s/sendPhoto", TG_API, APP_TELEGRAM_TOKEN);

    const char *BOUND = "----ESP32S3DualCamBoundary";

    /* body = 头部若干段 + JPEG 若干 + 结束边界。
     * caption 会被 URL 编码膨胀（最多 3 倍），所以按 3 倍留量。 */
    size_t cap = jpeg_len + strlen(caption) * 3 + 1024;
    char *body = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!body) {
        body = malloc(cap);
    }
    if (!body) {
        LOGE(TAG, "body 分配失败 (%u 字节)", (unsigned)cap);
        return ESP_ERR_NO_MEM;
    }

    size_t o = 0;
    o += snprintf(body + o, cap - o,
                  "--%s\r\nContent-Disposition: form-data; name=\"chat_id\"\r\n\r\n%s\r\n",
                  BOUND, chat_id);
    o += snprintf(body + o, cap - o,
                  "--%s\r\nContent-Disposition: form-data; name=\"caption\"\r\n\r\n"
                  "%.400s\r\n", BOUND, caption);
    o += snprintf(body + o, cap - o,
                  "--%s\r\nContent-Disposition: form-data; name=\"photo\"; "
                  "filename=\"sync.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n", BOUND);
    if (jpeg_len && o + jpeg_len + 64 < cap) {
        memcpy(body + o, jpeg, jpeg_len);
        o += jpeg_len;
    }
    o += snprintf(body + o, cap - o, "\r\n--%s--\r\n", BOUND);

    char ctype[96];
    snprintf(ctype, sizeof(ctype), "multipart/form-data; boundary=%s", BOUND);

    esp_http_client_config_t cc = {
        .url               = url,
        .method            = HTTP_METHOD_POST,
        .timeout_ms        = TG_TIMEOUT_MS,
        .buffer_size       = 4096,
        .buffer_size_tx    = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cc);
    if (!cli) {
        free(body);
        return ESP_FAIL;
    }
    esp_http_client_set_header(cli, "Content-Type", ctype);

    esp_err_t ret = ESP_FAIL;
    esp_err_t e = esp_http_client_open(cli, (int)o);
    if (e == ESP_OK) {
        /* ★★ 分块写而不是一次写完：每块之前检查 s_abort，这样"来了新的拍照任务"
         *    能在几十毫秒内打断上传。一次性写整个 body 时无法中断，一张慢传
         *    会把下一次触发拖住几十秒（现场就是这个症状）。 */
        size_t sent = 0;
        bool   aborted = false;
        int    wr = 0;
        while (sent < o) {
            if (s_abort) { aborted = true; break; }
            size_t chunk = (o - sent > 2048) ? 2048 : (o - sent);
            wr = esp_http_client_write(cli, (char *)body + sent, (int)chunk);
            if (wr < 0) break;
            sent += (size_t)wr;
        }
        if (aborted) {
            LOGW(TAG, "有新拍照 -> 打断本次上传（已发 %u/%u 字节）",
                 (unsigned)sent, (unsigned)o);
            esp_http_client_close(cli);
            esp_http_client_cleanup(cli);
            free(body);
            return ESP_ERR_INVALID_STATE;   /* 调用方负责把这张放回队列 */
        }
        if (wr < 0) {
            LOGE(TAG, "write 失败");
        } else {
            esp_http_client_fetch_headers(cli);
            int status = esp_http_client_get_status_code(cli);
            char resp[256] = {0};
            int rd = esp_http_client_read_response(cli, resp, sizeof(resp) - 1);
            resp[rd > 0 ? rd : 0] = '\0';
            if (status == 200 && strstr(resp, "\"ok\":true") != NULL) {
                ret = ESP_OK;
            } else {
                LOGW(TAG, "sendPhoto 失败 status=%d resp=%.140s", status, resp);
            }
        }
        esp_http_client_close(cli);
    } else {
        /* ★ 现场诊断：主机（Central）反复 open 失败 0x7002，但从机（Peripheral）
         *   同样的代码却能传。已经排除的：内存（6.6MB heap / 6.5MB PSRAM 空闲）、
         *   射频（扫描已暂停 + 连接间隔已放宽到 100ms）。剩下最可能的是 DNS：
         *   open 内部先解析域名，失败就报 0x7002，且耗时会卡在解析超时上
         *   （实测每次都是约 20.5 s，而不是配置的 45 s）。 */
        LOGE(TAG, "open 失败 0x%x  heap=%u PSRAM=%u body=%uKB",
             e, (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)(o / 1024));

        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo *res = NULL;
        int gai = getaddrinfo("api.telegram.org", "443", &hints, &res);
        if (gai == 0 && res) {
            struct sockaddr_in *sin = (struct sockaddr_in *)res->ai_addr;
            LOGI(TAG, "  DNS ok: api.telegram.org -> %s", inet_ntoa(sin->sin_addr));
            freeaddrinfo(res);
        } else {
            LOGE(TAG, "  DNS FAIL: api.telegram.org gai=%d (%s)", gai,
                 gai == 0 ? "empty" : "resolve error");
        }
    }
    esp_http_client_cleanup(cli);
    free(body);
    return ret;
}

static esp_err_t tg_send_photo(const char *caption, const uint8_t *jpeg, size_t len)
{
    static const uint32_t backoff[TG_RETRY_MAX] = { 1000, 2000, 3000, 4000, 5000 };
    esp_err_t ret = ESP_FAIL;
    for (int i = 0; i <= TG_RETRY_MAX; i++) {
        if (i > 0) {
            vTaskDelay(pdMS_TO_TICKS(backoff[i - 1]));
            LOGW(TAG, "retry %d/%d", i, TG_RETRY_MAX);
        }
        ret = tg_send_photo_once(APP_TELEGRAM_CHAT_ID, caption, jpeg, len);
        if (ret == ESP_OK) {
            return ESP_OK;
        }
    }
    return ret;
}

/* ============================== 上传任务 ============================== */
static void up_task(void *arg)
{
    (void)arg;
    bool session = false;      /* 是否处于一个"上传会话"（会话内不等静默） */
    for (;;) {
        up_item_t it;
        if (xQueueReceive(s_q, &it, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* ★★ 只在**开始一个上传会话**时等静默窗口，会话内连续传。
         *    如果每张都重新等，20 张会白等 100 s（实测第一版就是这样）。 */
        if (!session) {
            for (;;) {
                int64_t idle_us = esp_timer_get_time() - s_last_shot_us;
                if (idle_us >= (int64_t)TG_IDLE_BEFORE_UPLOAD_MS * 1000) break;
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            session = true;
            LOGI(TAG, "静默 %d ms -> 开始上传会话（队列 %u 张，gen=%u）",
                 TG_IDLE_BEFORE_UPLOAD_MS, (unsigned)uxQueueMessagesWaiting(s_q) + 1,
                 (unsigned)s_shot_gen);

            /* ★ 错开只在整个**会话开始**时做一次：两板同时静默结束、同时开传，
             *   会撞同一个 chat。会话内则连续传，不再逐张让位。 */
#if TG_UPLOAD_STAGGER_MS > 0
            LOGI(TAG, "stagger %d ms（让开主机，避免并发打同一 chat）",
                 TG_UPLOAD_STAGGER_MS);
            vTaskDelay(pdMS_TO_TICKS(TG_UPLOAD_STAGGER_MS));
#endif
        }
        /* 会话内：队列已经空了就收工（下次重新等静默）。 */
        if (uxQueueMessagesWaiting(s_q) == 0) {
            session = false;
        }

        /* ★★ 等 WiFi 就绪。实测两板同挂一个 Windows 移动热点时，主机的 WiFi
         *   关联极不稳定：可能 40+ 秒才 got_ip（中间反复 reason=2/201/205）。
         *   原来只等 15 s 就 free 掉，照片白拍 —— 而且成败完全取决于那一枪
         *   恰好落在掉线窗口的哪一侧。
         *   现在：等 TG_WIFI_WAIT_S 秒；仍不就绪就**放回队列**（不丢弃），
         *   并退出本次会话，等下一次静默再试。 */
        int waited = 0;
        while (!wifi_hal_connected() && waited < TG_WIFI_WAIT_S * 10) {
            vTaskDelay(pdMS_TO_TICKS(100));
            waited++;
        }
        if (!wifi_hal_connected()) {
            LOGW(TAG, "WiFi 仍未就绪（等了 %d s）-> 这张放回队列，稍后再试",
                 TG_WIFI_WAIT_S);
            session = false;
            if (xQueueSend(s_q, &it, 0) != pdTRUE) {
                up_item_t old;
                if (xQueueReceive(s_q, &old, 0) == pdTRUE) free(old.buf);
                if (xQueueSend(s_q, &it, 0) != pdTRUE) free(it.buf);
            }
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        /* 同 chat 节流 */
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (s_last_send_ms && (now_ms - s_last_send_ms) < TG_THROTTLE_MS) {
            vTaskDelay(pdMS_TO_TICKS(TG_THROTTLE_MS - (now_ms - s_last_send_ms)));
        }

        char cap[128];
        /* ★ caption 带序号：两板各自的第 N 张对得上（HOST #3 / SLAVE #3），
         *   配合 t_fire 可以直接人工核对 Telegram 里收到的张数与顺序。 */
        snprintf(cap, sizeof(cap), "%s #%u  sync t_fire=%lldus len=%uB",
                 BOARD_ID_STR, (unsigned)it.seq, (long long)it.t_fire,
                 (unsigned)it.len);

        LOGI(TAG, "上传中 %s (%u 字节)...", cap, (unsigned)it.len);
        /* ★ 整段 TLS 会话期间把射频让给 WiFi：暂停扫描 + 连接间隔 15->100ms。
         *   这里安全：上传只发生在"静默窗口"，此前的同步拍照已经完成。 */
        s_abort = false;
        UPLOAD_SET_MODE(true);
        int64_t t0 = esp_timer_get_time();
        esp_err_t e = tg_send_photo(cap, it.buf, it.len);
        UPLOAD_SET_MODE(false);
        int64_t took = (esp_timer_get_time() - t0) / 1000;

        /* ★ 在传的中途被新拍照打断：这张放回队尾（**不能 free**，buf 仍归它），
         *   等下一次静默再传。 */
        if (e == ESP_ERR_INVALID_STATE) {
            LOGW(TAG, "有新拍照 -> 本次上传会话结束，这张放回队列尾部");
            session = false;                  /* 有新拍照：退出会话，等下次静默 */
            if (xQueueSend(s_q, &it, 0) != pdTRUE) {
                up_item_t old;
                if (xQueueReceive(s_q, &old, 0) == pdTRUE) free(old.buf);
                if (xQueueSend(s_q, &it, 0) != pdTRUE) free(it.buf);
            }
            continue;
        }

        if (e == ESP_OK) {
            s_ok++;
            s_last_send_ms = esp_timer_get_time() / 1000;
            LOGI(TAG, "sendPhoto ok (%.1fs) 累计 ok=%u fail=%u",
                 took / 1000.0, (unsigned)s_ok, (unsigned)s_fail);
        } else {
            s_fail++;
            LOGE(TAG, "sendPhoto FAILED (%.1fs) 累计 ok=%u fail=%u",
                 took / 1000.0, (unsigned)s_ok, (unsigned)s_fail);
        }
        free(it.buf);
    }
}

/* ============================== 对外 API ============================== */
esp_err_t upload_telegram_init(void)
{
    if (APP_TELEGRAM_TOKEN[0] == '\0' || APP_TELEGRAM_CHAT_ID[0] == '\0') {
        LOGW(TAG, "未配置 Telegram token/chat_id -> 跳过上传");
        return ESP_ERR_INVALID_STATE;
    }

    s_q = xQueueCreate(UPLOAD_QUEUE_LEN, sizeof(up_item_t));
    if (!s_q) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(up_task, "tg_upload", 8192, NULL, 4, NULL) != pdPASS) {
        LOGE(TAG, "上传任务创建失败");
        return ESP_ERR_NO_MEM;
    }
    LOGI(TAG, "上传队列就绪（len=%d, chat=%s, board=%s）",
         UPLOAD_QUEUE_LEN, APP_TELEGRAM_CHAT_ID, BOARD_ID_STR);
    return ESP_OK;
}

esp_err_t upload_telegram_enqueue(const uint8_t *jpeg, size_t len, int64_t t_fire)
{
    if (!s_q || !jpeg || !len) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t *cp = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!cp) {
        cp = malloc(len);
    }
    if (!cp) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(cp, jpeg, len);

    up_item_t it = { cp, len, t_fire, ++s_shot_seq };

    /* ★★ 先登记"刚拍了一张"：
     *   - s_last_shot_us 让 up_task 判断是否已进入静默（没静默就不许上传）
     *   - s_abort 让正在传的那张立刻让路（下一块 write 前就会退出） */
    s_last_shot_us = esp_timer_get_time();
    s_shot_gen++;
    s_abort = true;

    /* 队列满时丢**最旧**的一张，保证本次一定入队（同步拍照不该被上传拖累） */
    if (xQueueSend(s_q, &it, 0) != pdTRUE) {
        up_item_t old;
        if (xQueueReceive(s_q, &old, 0) == pdTRUE) {
            LOGW(TAG, "上传队列满 -> 丢弃最旧的一张 (%u 字节)", (unsigned)old.len);
            free(old.buf);
        }
        if (xQueueSend(s_q, &it, 0) != pdTRUE) {
            free(cp);
            return ESP_ERR_INVALID_STATE;
        }
    }
    return ESP_OK;
}

void upload_telegram_stats(uint32_t *ok, uint32_t *fail)
{
    if (ok)   *ok = s_ok;
    if (fail) *fail = s_fail;
}
