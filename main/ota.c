#include "ota.h"

#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_format.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "mainapp.h"
#include "recovery_code.h"
#include "sdkconfig.h"
#if CONFIG_NETWORK_ETH_OPENETH
#include "esp_private/system_internal.h"
#endif

///////////////////////////////////////////////////////////////////////////////////////////////////
// Constants
///////////////////////////////////////////////////////////////////////////////////////////////////

#define BUF_SIZE 1024
#define OTA_AUTH_PASSWORD_HEADER "X-Coinbox-OTA-Password"
#define OTA_AUTH_RECOVERY_CODE_HEADER "X-Coinbox-OTA-Recovery-Code"
#define OTA_AUTH_PASSWORD_MAX_LEN 64
#define OTA_AUTH_RECOVERY_CODE_MAX_LEN 8

static const char *TAG = "ota";

///////////////////////////////////////////////////////////////////////////////////////////////////
// Private Helpers
///////////////////////////////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------
// Auth
//-------------------------------------------------------------------------

static esp_err_t ota_send_unauthorized(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, "Unauthorized");
    return ESP_FAIL;
}

static esp_err_t ota_send_bad_request(httpd_req_t *req, const char *message)
{
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, message ? message : "Bad Request");
    return ESP_FAIL;
}

static bool ota_header_to_string(httpd_req_t *req, const char *name, char *out, size_t out_size)
{
    if (!req || !name || !out || out_size == 0) {
        return false;
    }
    out[0] = '\0';

    size_t value_len = httpd_req_get_hdr_value_len(req, name);
    if (value_len == 0 || value_len >= out_size) {
        return false;
    }

    return httpd_req_get_hdr_value_str(req, name, out, out_size) == ESP_OK;
}

static bool ota_parse_recovery_code_header(const char *raw, int *out_code)
{
    if (!raw || !out_code) {
        return false;
    }

    size_t len = strlen(raw);
    if (len != 4) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        if (!isdigit((unsigned char)raw[i])) {
            return false;
        }
    }

    *out_code = atoi(raw);
    return true;
}

static esp_err_t ota_require_auth(httpd_req_t *req)
{
    if (!mainapp_security_is_password_set()) {
        return ESP_OK;
    }

    size_t password_len = httpd_req_get_hdr_value_len(req, OTA_AUTH_PASSWORD_HEADER);
    size_t recovery_len = httpd_req_get_hdr_value_len(req, OTA_AUTH_RECOVERY_CODE_HEADER);
    bool has_password = (password_len > 0);
    bool has_recovery_code = (recovery_len > 0);

    if (has_password == has_recovery_code) {
        if (has_password) {
            ESP_LOGW(TAG, "Rejecting OTA request with multiple auth headers");
            return ota_send_bad_request(req, "Provide exactly one OTA auth header");
        }
        ESP_LOGW(TAG, "Rejecting OTA request without auth");
        return ota_send_unauthorized(req);
    }

    if (has_password) {
        char password[OTA_AUTH_PASSWORD_MAX_LEN + 1];
        if (!ota_header_to_string(req, OTA_AUTH_PASSWORD_HEADER, password, sizeof(password))) {
            ESP_LOGW(TAG, "Rejecting OTA request with invalid password header");
            return ota_send_unauthorized(req);
        }
        if (!mainapp_security_password_matches(password)) {
            ESP_LOGW(TAG, "Rejecting OTA request due to password auth failure");
            return ota_send_unauthorized(req);
        }
        return ESP_OK;
    }

    char recovery_code_raw[OTA_AUTH_RECOVERY_CODE_MAX_LEN + 1];
    int recovery_code = 0;
    if (!ota_header_to_string(req, OTA_AUTH_RECOVERY_CODE_HEADER, recovery_code_raw, sizeof(recovery_code_raw)) ||
        !ota_parse_recovery_code_header(recovery_code_raw, &recovery_code) ||
        !recovery_code_check(recovery_code)) {
        ESP_LOGW(TAG, "Rejecting OTA request due to recovery code auth failure");
        return ota_send_unauthorized(req);
    }

    return ESP_OK;
}

//-------------------------------------------------------------------------
// Firmware Update
//-------------------------------------------------------------------------

static esp_err_t ota_firmware(httpd_req_t *req)
{
    size_t total_len = req->content_len;
    uint8_t pct_received = 0;
    esp_err_t err = ESP_OK;

    esp_ota_handle_t update_handle = 0;
    const esp_partition_t *update_partition = NULL;

    ESP_LOGI(TAG, "Starting OTA firmware update");

    const esp_partition_t *configured = esp_ota_get_boot_partition();
    const esp_partition_t *running = esp_ota_get_running_partition();

    if (configured != running) {
        ESP_LOGW(TAG, "Configured OTA boot partition at offset 0x%08" PRIx32 ", but running from offset 0x%08" PRIx32,
                 configured->address, running->address);
        ESP_LOGW(TAG, "(This can happen if either the OTA boot data or preferred boot image become corrupted somehow.)");
    }
    ESP_LOGI(TAG, "Running partition type %d subtype %d (offset 0x%08" PRIx32 ")",
             running->type, running->subtype, running->address);

    update_partition = esp_ota_get_next_update_partition(NULL);
    assert(update_partition != NULL);
    ESP_LOGI(TAG, "Writing to partition subtype %d at offset 0x%" PRIx32,
             update_partition->subtype, update_partition->address);

    char *buf = malloc(BUF_SIZE);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    int binary_file_length = 0;
    int remaining = req->content_len;
    bool image_header_was_checked = false;

    while (remaining > 0) {
        int data_read = httpd_req_recv(req, buf, BUF_SIZE);
        uint8_t new_pct_received = (uint8_t)(((total_len - remaining) * 100) / total_len);
        if (pct_received != new_pct_received) {
            pct_received = new_pct_received;
            ESP_LOGW(TAG, "Flashing: firmware; Packages received: %d; Status: downloading...", pct_received);
        }

        if (data_read <= 0) {
            if (data_read == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }

            ESP_LOGE(TAG, "OTA reception failed!");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive ota");
            free(buf);
            return ESP_FAIL;
        }

        if (!image_header_was_checked) {
            esp_app_desc_t new_app_info;
            if (data_read <= (int)(sizeof(esp_image_header_t) +
                                    sizeof(esp_image_segment_header_t) +
                                    sizeof(esp_app_desc_t))) {
                ESP_LOGE(TAG, "received package is not fit len");
                esp_ota_abort(update_handle);
                free(buf);
                return ESP_FAIL;
            }

            memcpy(&new_app_info,
                   &buf[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t)],
                   sizeof(esp_app_desc_t));
            ESP_LOGI(TAG, "New firmware version: %s", new_app_info.version);

            esp_app_desc_t running_app_info;
            if (esp_ota_get_partition_description(running, &running_app_info) == ESP_OK) {
                ESP_LOGI(TAG, "Running firmware version: %s", running_app_info.version);
            }

            const esp_partition_t *last_invalid_app = esp_ota_get_last_invalid_partition();
            esp_app_desc_t invalid_app_info;
            if (last_invalid_app != NULL &&
                esp_ota_get_partition_description(last_invalid_app, &invalid_app_info) == ESP_OK) {
                ESP_LOGI(TAG, "Last invalid firmware version: %s", invalid_app_info.version);
                if (memcmp(invalid_app_info.version, new_app_info.version, sizeof(new_app_info.version)) == 0) {
                    ESP_LOGW(TAG, "New version is the same as invalid version.");
                    ESP_LOGW(TAG, "Previously, there was an attempt to launch the firmware with %s version, but it failed.",
                             invalid_app_info.version);
                    ESP_LOGW(TAG, "The firmware has been rolled back to the previous version.");
                    free(buf);
                    return ESP_FAIL;
                }
            }

            image_header_was_checked = true;

            err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
                esp_ota_abort(update_handle);
                free(buf);
                return ESP_FAIL;
            }
            ESP_LOGI(TAG, "esp_ota_begin succeeded");
        }

        err = esp_ota_write(update_handle, (const void *)buf, data_read);
        if (err != ESP_OK) {
            esp_ota_abort(update_handle);
            free(buf);
            return ESP_FAIL;
        }

        binary_file_length += data_read;
        ESP_LOGD(TAG, "Written image length %d", binary_file_length);
        remaining -= data_read;
    }

    free(buf);
    ESP_LOGI(TAG, "Total Write binary data length: %d", binary_file_length);

    err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed, image is corrupted");
        } else {
            ESP_LOGE(TAG, "esp_ota_end failed (%s)!", esp_err_to_name(err));
        }
        return err;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)!", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

//-------------------------------------------------------------------------
// Restart
//-------------------------------------------------------------------------

static void reboot_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    ESP_LOGE(TAG, "Restarting...");
    vTaskDelay(pdMS_TO_TICKS(1000));
#if CONFIG_NETWORK_ETH_OPENETH
    esp_restart_noos_dig();
#else
    esp_restart();
#endif
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Public Interface
///////////////////////////////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------
// Firmware Update Handler
//-------------------------------------------------------------------------

esp_err_t ota_update_handler(httpd_req_t *req)
{
    esp_err_t auth_err = ota_require_auth(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }

    ESP_LOGW(TAG, "Flashing: firmware; Packages received: %d; Status: ready", 0);

    esp_err_t err = ota_firmware(req);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA update failed: %s", esp_err_to_name(err));
        ESP_LOGW(TAG, "Flashing: firmware; Packages received: %d; Status: OTA update failed", 0);
        return err;
    }

    httpd_resp_set_status(req, "200");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Content-Length", "0");
    httpd_resp_set_hdr(req, "Connection", "close");
    esp_err_t resp_err = httpd_resp_send(req, "", HTTPD_RESP_USE_STRLEN);
    if (resp_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send OTA success response: %s", esp_err_to_name(resp_err));
    }

    ESP_LOGI(TAG, "Prepare to restart system!");

    TimerHandle_t reboot_timer = xTimerCreate(
        "reboot_timer",
        pdMS_TO_TICKS(2000),
        pdFALSE,
        NULL,
        reboot_timer_cb
    );
    ESP_LOGI(TAG, "OTA complete");

    if (reboot_timer == NULL) {
        ESP_LOGE(TAG, "Failed to create reboot timer");
        return ESP_OK;
    }
    if (xTimerStart(reboot_timer, 100) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start reboot timer");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Flashing: firmware; Packages received: %d; Status: Rebooting now ...", 100);

    return ESP_OK;
}
