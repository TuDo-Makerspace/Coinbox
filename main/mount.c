#include "mount.h"
#include <stdio.h>
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_littlefs.h"
#include "sdkconfig.h"

static const char *TAG = "mount";

esp_err_t mount_storage(const char* base_path)
{
    ESP_LOGI(TAG, "Initializing LittleFS");

    esp_vfs_littlefs_conf_t conf = {
        .base_path             = base_path,
        .partition_label       = "storage",       // must match a partition in your CSV table
        .format_if_mount_failed = true,
        .dont_mount            = false
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format LittleFS");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "LittleFS partition not found");
        } else {
            ESP_LOGE(TAG, "Failed to initialize LittleFS (%s)", esp_err_to_name(ret));
        }
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_littlefs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get LittleFS partition info (%s)", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "LittleFS partition mounted at '%s' (total: %d, used: %d)",
             base_path, total, used);
    return ESP_OK;
}

esp_err_t format_storage(const char* base_path)
{
    ESP_LOGW(TAG, "Formatting LittleFS partition");
    esp_err_t err = esp_vfs_littlefs_unregister("storage");
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGE(TAG, "Failed to unmount before format: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_littlefs_format("storage");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Format failed: %s", esp_err_to_name(err));
        return err;
    }

    return mount_storage(base_path);
}
