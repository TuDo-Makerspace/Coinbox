#include "mount.h"
#include <stdio.h>
#include <string.h>
#include "logger.h"
#include "esp_err.h"
#include "esp_littlefs.h"
#include "sdkconfig.h"

static const char *TAG = "mount";

esp_err_t mount_storage(const char* base_path)
{
    logger_logi(TAG, "Initializing LittleFS");

    esp_vfs_littlefs_conf_t conf = {
        .base_path             = base_path,
        .partition_label       = "storage",       // must match a partition in your CSV table
        .format_if_mount_failed = true,
        .dont_mount            = false
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            logger_loge(TAG, "Failed to mount or format LittleFS");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            logger_loge(TAG, "LittleFS partition not found");
        } else {
            logger_loge(TAG, "Failed to initialize LittleFS (%s)", esp_err_to_name(ret));
        }
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_littlefs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK) {
        logger_loge(TAG, "Failed to get LittleFS partition info (%s)", esp_err_to_name(ret));
        return ret;
    }

    logger_logi(TAG, "LittleFS partition mounted at '%s' (total: %d, used: %d)",
             base_path, total, used);
    return ESP_OK;
}
