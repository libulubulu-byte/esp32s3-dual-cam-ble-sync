/**
 * @file camera_hal.c
 * @brief OV3660 摄像头极简 HAL
 *
 * 从 verified 工程 two_demo_SD_who_esp32S3_CAM 精简而来：
 *   - 保留 OV3660 驱动 + 双缓冲 + 防残留帧
 *   - 去掉深睡唤醒恢复 / 裸 SCCB 自救 / 参数运行时调节
 */

#include "camera_hal.h"
#include "pins_config.h"
#include "logger.h"
#include "app_conf.h"

#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_camera.h"
#include "esp_timer.h"

static const char *TAG = LOG_T_CAM;

static bool     s_inited;
static int      s_framesize;
static int      s_quality;

static const char *framesize_name(int fs)
{
    switch (fs) {
    case FRAMESIZE_96X96:    return "96x96";
    case FRAMESIZE_QQVGA:    return "QQVGA(160x120)";
    case FRAMESIZE_QCIF:     return "QCIF(176x144)";
    case FRAMESIZE_HQVGA:    return "HQVGA(240x176)";
    case FRAMESIZE_QVGA:     return "QVGA(320x240)";
    case FRAMESIZE_CIF:      return "CIF(400x296)";
    case FRAMESIZE_HVGA:     return "HVGA(480x320)";
    case FRAMESIZE_VGA:      return "VGA(640x480)";
    case FRAMESIZE_SVGA:     return "SVGA(800x600)";
    case FRAMESIZE_XGA:      return "XGA(1024x768)";
    case FRAMESIZE_HD:       return "HD(1280x720)";
    case FRAMESIZE_SXGA:     return "SXGA(1280x1024)";
    case FRAMESIZE_UXGA:     return "UXGA(1600x1200)";
    case FRAMESIZE_FHD:      return "FHD(1920x1080)";
    case FRAMESIZE_P_HD:     return "P_HD(720x1280)";
    case FRAMESIZE_P_3MP:    return "P_3MP(864x1536)";
    case FRAMESIZE_QXGA:     return "QXGA(2048x1536)";
    case FRAMESIZE_QHD:      return "QHD(2560x1440)";
    case FRAMESIZE_WQXGA:    return "WQXGA(2560x1600)";
    case FRAMESIZE_P_FHD:    return "P_FHD(1080x1920)";
    case FRAMESIZE_QSXGA:    return "QSXGA(2560x1920)";
    case FRAMESIZE_5MP:      return "5MP(2592x1944)";
    default:                 return "?";
    }
}

static int default_quality_for(int fs)
{
    if (fs >= FRAMESIZE_QSXGA) return 10;
    if (fs >= FRAMESIZE_UXGA)  return 12;
    if (fs >= FRAMESIZE_XGA)   return 12;
    if (fs >= FRAMESIZE_SVGA)  return 12;
    return 14;
}

esp_err_t camera_hal_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    int fs = CAM_DEFAULT_FRAMESIZE;
    if (fs > FRAMESIZE_QXGA) {
        fs = FRAMESIZE_QXGA;
    }
    if (fs < FRAMESIZE_QVGA) {
        fs = FRAMESIZE_QVGA;
    }
    s_framesize = fs;
    s_quality   = CAM_DEFAULT_QUALITY;

    camera_config_t cfg = {
        .pin_pwdn     = CAM_PIN_PWDN,
        .pin_reset    = CAM_PIN_RESET,
        .pin_xclk     = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SDA,
        .pin_sccb_scl = CAM_PIN_SCL,
        .pin_d7       = CAM_PIN_D7,
        .pin_d6       = CAM_PIN_D6,
        .pin_d5       = CAM_PIN_D5,
        .pin_d4       = CAM_PIN_D4,
        .pin_d3       = CAM_PIN_D3,
        .pin_d2       = CAM_PIN_D2,
        .pin_d1       = CAM_PIN_D1,
        .pin_d0       = CAM_PIN_D0,
        .pin_vsync    = CAM_PIN_VSYNC,
        .pin_href     = CAM_PIN_HREF,
        .pin_pclk     = CAM_PIN_PCLK,

        .xclk_freq_hz = CAM_XCLK_MHZ * 1000000,
        .ledc_timer   = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = PIXFORMAT_JPEG,
        .frame_size   = (framesize_t)fs,
        .jpeg_quality = s_quality,
        .fb_count     = 2,
        .fb_location  = CAMERA_FB_IN_PSRAM,
        .grab_mode    = CAMERA_GRAB_WHEN_EMPTY,
    };

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        LOGE(TAG, "init failed err=0x%x", err);
        return err;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s == NULL) {
        LOGE(TAG, "sensor NULL after init");
        esp_camera_deinit();
        return ESP_FAIL;
    }

    LOGI(TAG, "init ok sensor=OV3660 pid=0x%04X frame=%s q=%d xclk=%dMHz fb=2xPSRAM",
         (unsigned)s->id.PID, framesize_name(fs), s_quality, CAM_XCLK_MHZ);

    s_inited = true;
    return ESP_OK;
}

#define CAM_GRAB_MAX_DRAIN  4
#define CAM_TS_FUTURE_MS    500

camera_fb_t *camera_hal_grab(uint32_t timeout_ms)
{
    if (!s_inited) {
        return NULL;
    }

    const int64_t t_req = esp_timer_get_time();
    camera_fb_t *fb = NULL;
    int drained = 0;

    while (drained <= CAM_GRAB_MAX_DRAIN) {
        fb = esp_camera_fb_get();
        if (fb == NULL) {
            LOGW(TAG, "grab fail (fb NULL)");
            return NULL;
        }

        if (fb->len == 0 || fb->buf == NULL) {
            LOGW(TAG, "drop empty frame len=%u", (unsigned)fb->len);
            esp_camera_fb_return(fb);
            fb = NULL;
            drained++;
            continue;
        }

        const int64_t ts = (int64_t)fb->timestamp.tv_sec * 1000000 + (int64_t)fb->timestamp.tv_usec;
        const int64_t now = esp_timer_get_time();

        if (ts > now + (int64_t)CAM_TS_FUTURE_MS * 1000) {
            LOGW(TAG, "drop future-ts frame (ts=%lldms > now=%lldms) len=%u",
                 (long long)(ts / 1000), (long long)(now / 1000), (unsigned)fb->len);
            esp_camera_fb_return(fb);
            fb = NULL;
            drained++;
            continue;
        }

        if (ts >= t_req) {
            break;
        }

        LOGW(TAG, "drop stale frame len=%u age=%lldms",
             (unsigned)fb->len, (long long)((esp_timer_get_time() - ts) / 1000));
        esp_camera_fb_return(fb);
        fb = NULL;
        drained++;

        if (timeout_ms && (esp_timer_get_time() - t_req) / 1000 > (int64_t)timeout_ms) {
            LOGW(TAG, "drain > %ums, accept current", (unsigned)timeout_ms);
            break;
        }
    }

    if (fb == NULL) {
        fb = esp_camera_fb_get();
        if (fb == NULL) {
            LOGW(TAG, "grab fail (drained=%d)", drained);
            return NULL;
        }
        LOGW(TAG, "grab accepted (may be stale) len=%u drained=%d",
             (unsigned)fb->len, drained);
        return fb;
    }

    const int64_t ts = (int64_t)fb->timestamp.tv_sec * 1000000 + (int64_t)fb->timestamp.tv_usec;
    LOGI(TAG, "grab ok len=%u age=%lldms drained=%d",
         (unsigned)fb->len, (long long)((esp_timer_get_time() - ts) / 1000), drained);
    return fb;
}

esp_err_t camera_hal_return(camera_fb_t *fb)
{
    if (fb == NULL) {
        return ESP_OK;
    }
    if (!s_inited) {
        LOGW(TAG, "camera deinit -> drop fb");
        return ESP_OK;
    }
    esp_camera_fb_return(fb);
    return ESP_OK;
}

const char *camera_hal_sensor_name(void)
{
    return "OV3660";
}

int camera_hal_current_framesize(void)
{
    return s_framesize;
}
