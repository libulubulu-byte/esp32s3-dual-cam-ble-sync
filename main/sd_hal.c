/**
 * @file sd_hal.c
 * @brief SD 卡极简 HAL（SDMMC 1-bit，mount + 写文件）
 *
 * 从 verified 工程精简：去掉 bring-up 探针 / 自动重挂载 / 空间清理 /
 * 睡眠准备，只保留"启动挂载 + 写文件"两条路径。
 */

#include "sd_hal.h"
#include "pins_config.h"
#include "logger.h"

#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
#include "esp_timer.h"

static const char *TAG = LOG_T_SD;

#define SD_MOUNT_POINT   "/sdcard"
#define SD_MAX_FILES     5
#define SD_ALLOC_UNIT    (16 * 1024)

static sdmmc_card_t *s_card;
static bool          s_mounted;
static SemaphoreHandle_t s_lock;

static void abs_path(const char *rel, char *out, size_t out_sz)
{
    if (rel[0] == '/') {
        strlcpy(out, rel, out_sz);
    } else {
        snprintf(out, out_sz, "%s/%s", SD_MOUNT_POINT, rel);
    }
}

static void mkdirs_for(const char *rel)
{
    char p[256];
    abs_path(rel, p, sizeof(p));
    char *slash = p + strlen(SD_MOUNT_POINT);
    for (char *q = slash; *q; q++) {
        if (*q == '/') {
            *q = '\0';
            mkdir(p, 0777);
            *q = '/';
        }
    }
}

static esp_err_t do_mount(void)
{
    if (s_mounted) {
        return ESP_OK;
    }

    LOGI(TAG, "mount start clk=%d cmd=%d d0=%d mode=1bit",
         SD_PIN_CLK, SD_PIN_CMD, SD_PIN_DAT0);

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags &= ~(SDMMC_HOST_FLAG_4BIT | SDMMC_HOST_FLAG_8BIT);
    host.flags |= SDMMC_HOST_FLAG_1BIT;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = SD_PIN_CLK;
    slot.cmd = SD_PIN_CMD;
    slot.d0  = SD_PIN_DAT0;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,
        .max_files = SD_MAX_FILES,
        .allocation_unit_size = SD_ALLOC_UNIT,
        .disk_status_check_enable = false,
    };

    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &mcfg, &s_card);
    if (err != ESP_OK) {
        LOGE(TAG, "mount fail err=0x%x", err);
        return err;
    }

    s_mounted = true;
    uint64_t total = 0, fre = 0;
    esp_vfs_fat_info(SD_MOUNT_POINT, &total, &fre);
    LOGI(TAG, "mount ok total=%lluMB free=%lluMB",
         (unsigned long long)(total / (1024ULL * 1024ULL)),
         (unsigned long long)(fre / (1024ULL * 1024ULL)));
    return ESP_OK;
}

esp_err_t sd_hal_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = do_mount();
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t sd_hal_write_file(const char *rel_path, const void *data, size_t len)
{
    if (!s_mounted || !s_card) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(8000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    char tmp_rel[256];
    snprintf(tmp_rel, sizeof(tmp_rel), "%s.tmp", rel_path);

    char tmp_abs[280];
    abs_path(tmp_rel, tmp_abs, sizeof(tmp_abs));

    mkdirs_for(rel_path);

    FILE *f = fopen(tmp_abs, "wb");
    esp_err_t ret = ESP_OK;
    if (!f) {
        ret = ESP_FAIL;
    } else {
        size_t w = fwrite(data, 1, len, f);
        fclose(f);
        if (w != len) {
            ret = ESP_FAIL;
        }
    }

    if (ret == ESP_OK) {
        char abs[280];
        abs_path(rel_path, abs, sizeof(abs));
        remove(abs);
        if (rename(tmp_abs, abs) != 0) {
            ret = ESP_FAIL;
        }
    }

    if (ret != ESP_OK) {
        LOGE(TAG, "write fail path=%s len=%u", rel_path, (unsigned)len);
        remove(tmp_abs);
    } else {
        LOGI(TAG, "write ok path=%s len=%u", rel_path, (unsigned)len);
    }

    xSemaphoreGive(s_lock);
    return ret;
}

bool sd_hal_is_mounted(void)
{
    return s_mounted;
}

const char *sd_hal_mount_point(void)
{
    return SD_MOUNT_POINT;
}
