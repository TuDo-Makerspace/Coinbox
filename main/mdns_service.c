#include "mdns_service.h"

#include <stdbool.h>

#include "esp_err.h"
#include "esp_log.h"
#include "mdns.h"
#include "sdkconfig.h"

#define COINBOX_HTTP_PORT 80

static const char *TAG = "mdns";
static bool s_http_service_added;

esp_err_t mdns_stop_service(void)
{
    if (!s_http_service_added) {
        return ESP_OK;
    }

    esp_err_t err = mdns_service_remove("_http", "_tcp");
    if (err == ESP_OK || err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_STATE) {
        s_http_service_added = false;
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Failed to remove HTTP service: %s", esp_err_to_name(err));
    return err;
}

esp_err_t mdns_start_service(void)
{
    const char *hostname = CONFIG_MDNS_HOSTNAME[0] ? CONFIG_MDNS_HOSTNAME : "coinbox";
    const char *instance = CONFIG_MDNS_INSTANCE[0] ? CONFIG_MDNS_INSTANCE : "Coinbox";
    bool can_cleanup = false;

    esp_err_t err = mdns_init();
    if (err == ESP_OK) {
        can_cleanup = true;
    } else if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "mDNS already initialized; refreshing records");
    } else {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = mdns_hostname_set(hostname);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set hostname: %s", esp_err_to_name(err));
        goto fail;
    }

    err = mdns_instance_name_set(instance);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set instance name: %s", esp_err_to_name(err));
        goto fail;
    }

    if (s_http_service_added) {
        err = mdns_stop_service();
        if (err != ESP_OK) {
            goto fail;
        }
    }

    err = mdns_service_add(NULL, "_http", "_tcp", COINBOX_HTTP_PORT, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to advertise HTTP service: %s", esp_err_to_name(err));
        goto fail;
    }
    s_http_service_added = true;

    ESP_LOGI(TAG, "mDNS responder started: http://%s.local (%s)", hostname, instance);
    return ESP_OK;

fail:
    s_http_service_added = false;
    if (can_cleanup) {
        mdns_free();
    }
    return err;
}
