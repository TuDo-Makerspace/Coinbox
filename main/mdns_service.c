#include "mdns_service.h"

#include <stdbool.h>

#include "esp_err.h"
#include "esp_log.h"
#include "mdns.h"
#include "sdkconfig.h"

#define COINBOX_HTTP_PORT 80

static const char *TAG = "mdns";

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

    mdns_service_remove("_http", "_tcp");
    err = mdns_service_add(NULL, "_http", "_tcp", COINBOX_HTTP_PORT, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to advertise HTTP service: %s", esp_err_to_name(err));
        goto fail;
    }

    ESP_LOGI(TAG, "mDNS responder started: http://%s.local (%s)", hostname, instance);
    return ESP_OK;

fail:
    if (can_cleanup) {
        mdns_free();
    }
    return err;
}
