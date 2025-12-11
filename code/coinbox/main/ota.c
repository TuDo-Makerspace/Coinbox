#include "ota.h"
#include <esp_http_server.h>
#include <esp_event.h>
#include <esp_system.h>
#include "ws_server.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "sys/param.h"
#include <esp_app_format.h>
#include <esp_littlefs.h>
#include "logger.h"

static const char *TAG = "ota";

#define BUF_SIZE 1024
#define PART_ARG_LEN 16

static esp_err_t ota_os(httpd_req_t *req)
{
    size_t total_len = req->content_len;
    uint8_t pct_received = 0;
    esp_err_t err = ESP_OK;

    /* update handle : set by esp_ota_begin(), must be freed via esp_ota_end() */
    esp_ota_handle_t update_handle = 0 ;
    const esp_partition_t *update_partition = NULL;

    logger_logi(TAG, "Starting OTA OS");

    const esp_partition_t *configured = esp_ota_get_boot_partition();
    const esp_partition_t *running = esp_ota_get_running_partition();

    if (configured != running) {
        logger_logw(TAG, "Configured OTA boot partition at offset 0x%08"PRIx32", but running from offset 0x%08"PRIx32,
                 configured->address, running->address);
        logger_logw(TAG, "(This can happen if either the OTA boot data or preferred boot image become corrupted somehow.)");
    }
    logger_logi(TAG, "Running partition type %d subtype %d (offset 0x%08"PRIx32")",
             running->type, running->subtype, running->address);

    update_partition = esp_ota_get_next_update_partition(NULL);
    assert(update_partition != NULL);
    logger_logi(TAG, "Writing to partition subtype %d at offset 0x%"PRIx32,
             update_partition->subtype, update_partition->address);

    int binary_file_length = 0;

    char *buf = malloc(BUF_SIZE);

    /* Content length of the request gives
     * the size of the file being uploaded */
    int remaining = total_len;

    /*deal with all receive packet*/
    bool image_header_was_checked = false;
    while (remaining > 0) {
        int data_read = httpd_req_recv(req, buf, BUF_SIZE);
        //logger_logi(TAG, "Remaining: %d", remaining);
        uint8_t new_pct_received = (uint8_t)(((total_len - remaining) * 100) / total_len);
        if (pct_received != new_pct_received)
        {
            pct_received = new_pct_received;
            logger_logw(TAG, "Flashing: OS; Packages received: %d; Status: downloading...", pct_received);
        }
        if (data_read <= 0) {
            if (data_read == HTTPD_SOCK_ERR_TIMEOUT) {
                /* Retry if timeout occurred */
                continue;
            }

            logger_loge(TAG, "OTA reception failed!");
            /* Respond with 500 Internal Server Error */
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive ota");
            return ESP_FAIL;
        } else {
            if (image_header_was_checked == false) {
                esp_app_desc_t new_app_info;
                if (data_read > sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)) {
                    // check current version with downloading
                    memcpy(&new_app_info, &buf[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t)], sizeof(esp_app_desc_t));
                    logger_logi(TAG, "New firmware version: %s", new_app_info.version);

                    esp_app_desc_t running_app_info;
                    if (esp_ota_get_partition_description(running, &running_app_info) == ESP_OK) {
                        logger_logi(TAG, "Running firmware version: %s", running_app_info.version);
                    }

                    const esp_partition_t* last_invalid_app = esp_ota_get_last_invalid_partition();
                    esp_app_desc_t invalid_app_info;
                    if (esp_ota_get_partition_description(last_invalid_app, &invalid_app_info) == ESP_OK) {
                        logger_logi(TAG, "Last invalid firmware version: %s", invalid_app_info.version);
                    }

                    // check current version with last invalid partition
                    if (last_invalid_app != NULL) {
                        if (memcmp(invalid_app_info.version, new_app_info.version, sizeof(new_app_info.version)) == 0) {
                            logger_logw(TAG, "New version is the same as invalid version.");
                            logger_logw(TAG, "Previously, there was an attempt to launch the firmware with %s version, but it failed.", invalid_app_info.version);
                            logger_logw(TAG, "The firmware has been rolled back to the previous version.");
                            return ESP_FAIL;
                        }
                    }

                    image_header_was_checked = true;

                    err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle);
                    if (err != ESP_OK) {
                        logger_loge(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
                        esp_ota_abort(update_handle);
                        return ESP_FAIL;
                    }
                    logger_logi(TAG, "esp_ota_begin succeeded");
                } else {
                    logger_loge(TAG, "received package is not fit len");
                    esp_ota_abort(update_handle);
                    return ESP_FAIL;
                }
            }
            err = esp_ota_write( update_handle, (const void *)buf, data_read);
            if (err != ESP_OK) {
                esp_ota_abort(update_handle);
                return ESP_FAIL;
            }
            binary_file_length += data_read;
            logger_logd(TAG, "Written image length %d", binary_file_length);
        }
        remaining -= data_read;
    }

    logger_logi(TAG, "Total Write binary data length: %d", binary_file_length);

    err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            logger_loge(TAG, "Image validation failed, image is corrupted");
        } else {
            logger_loge(TAG, "esp_ota_end failed (%s)!", esp_err_to_name(err));
        }
        return err;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        logger_loge(TAG, "esp_ota_set_boot_partition failed (%s)!", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

static esp_err_t ota_littlefs(httpd_req_t *req){
      size_t total_len = req->content_len;
    size_t remaining = total_len;
    uint8_t pct_received = 0;
    esp_err_t err = ESP_OK;
    size_t dst_offset = 0;
    bool header_checked = false;

    // 1. Find the "storage" partition (LittleFS usually shares the same label/subtype semantics as SPIFFS). 
    const esp_partition_t *storage_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_ANY,  // accept littlefs or spiffs style labels
        "storage");
    if (storage_part == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Partition not found");
        logger_logw(TAG, "Flashing: LittleFS; Packages received: %d; Status: Partition not found", 0);
        return ESP_FAIL;
    }

    if (total_len > storage_part->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Upload too large for partition");
        logger_logw(TAG, "Flashing: LittleFS; Packages received: %d; Status: Size exceeds partition", 0);
        return ESP_FAIL;
    }

    // 2. Erase full partition before writing
    err = esp_partition_erase_range(storage_part, 0, storage_part->size);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Erase failed");
        logger_logw(TAG, "Flashing: LittleFS; Packages received: %d; Status: Erase failed", 0);
        return err;
    }

    int binary_file_length = 0;
    char *buf = malloc(BUF_SIZE);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        logger_logw(TAG, "Flashing: LittleFS; Packages received: %d; Status: OOM", 0);
        return ESP_ERR_NO_MEM;
    }

    // 3. Stream in and write directly to partition
    while (remaining > 0) {
        int to_read = (remaining > BUF_SIZE) ? BUF_SIZE : remaining;
        int data_read = httpd_req_recv(req, buf, to_read);
        uint8_t new_pct = (uint8_t)(((total_len - remaining) * 100) / total_len);
        if (pct_received != new_pct) {
            pct_received = new_pct;
            logger_logw(TAG, "Flashing: LittleFS; Packages received: %d; Status: downloading...", pct_received);
        }

        if (data_read <= 0) {
            if (data_read == HTTPD_SOCK_ERR_TIMEOUT) {
                continue; // retry on timeout
            }
            logger_loge(TAG, "OTA reception failed");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive ota");
            free(buf);
            return ESP_FAIL;
        }

        // Optional: if the uploaded image is actually a firmware image, keep this header/version check.
        // If you're replacing a LittleFS image, you can remove this block entirely.
        if (!header_checked) {
            if (data_read > sizeof(esp_image_header_t)
                  + sizeof(esp_image_segment_header_t)
                  + sizeof(esp_app_desc_t)) {
                esp_app_desc_t new_app_info;
                memcpy(&new_app_info,
                       buf + sizeof(esp_image_header_t)
                           + sizeof(esp_image_segment_header_t),
                       sizeof(esp_app_desc_t));
                logger_logi(TAG, "Incoming image version: %s", new_app_info.version);

                const esp_partition_t *last_invalid = esp_ota_get_last_invalid_partition();
                esp_app_desc_t invalid_info;
                if (last_invalid != NULL &&
                    esp_ota_get_partition_description(last_invalid, &invalid_info) == ESP_OK &&
                    memcmp(invalid_info.version, new_app_info.version, sizeof(new_app_info.version)) == 0) {
                    logger_logw(TAG, "Version matches last invalid; aborting.");
                    free(buf);
                    return ESP_FAIL;
                }
                header_checked = true;
            } else {
                logger_loge(TAG, "Chunk too small for header check");
                free(buf);
                return ESP_FAIL;
            }
        }

        err = esp_partition_write(storage_part, dst_offset, (const void *)buf, data_read);
        if (err != ESP_OK) {
            logger_loge(TAG, "Partition write failed: %s", esp_err_to_name(err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
            free(buf);
            return err;
        }

        dst_offset += data_read;
        binary_file_length += data_read;
        logger_logd(TAG, "Written %d bytes, total %d", data_read, binary_file_length);
        remaining -= data_read;
    }

    free(buf);
    logger_logi(TAG, "Total written binary data length: %d", binary_file_length);
    logger_logw(TAG, "Flashing: LittleFS; Packages received: %d; Status: complete", 100);
    return ESP_OK;
}

static void reboot_timer_cb(TimerHandle_t xTimer)
{
    logger_loge(TAG, "Restarting...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart(); 
}

esp_err_t ota_update_handler(httpd_req_t *req)
{

    size_t total_len = req->content_len;
    size_t query_len = httpd_req_get_url_query_len(req);
    logger_loge(TAG, "URL query length: %d", query_len);
    char *buf_url_q = malloc((query_len + 1) * sizeof(char));
    esp_err_t err = httpd_req_get_url_query_str(req, buf_url_q, query_len+1);
    if(err != ESP_OK) {
        logger_loge(TAG, "Failed to get URL query string");
        logger_loge(TAG, "Error: %s", esp_err_to_name(err));
        free(buf_url_q);
        return ESP_FAIL;
    }
    logger_loge(TAG, "URL query: %s", buf_url_q);

    // Determine which partition to update
    char part_name[PART_ARG_LEN] = {0};
    httpd_query_key_value(buf_url_q, "partition", part_name, PART_ARG_LEN);
    logger_loge(TAG, "Partition: %s", part_name);
    bool is_fw = (strcmp(part_name, "firmware") == 0);

    logger_logw(TAG, "Flashing: %s; Packages received: %d; Status: ready", is_fw ? "OS" : "SPIFFS", 0);

    
    if (is_fw)
    {
        err = ota_os(req);
    }
    else
    {
        err = ota_littlefs(req);
    }

    if (err != ESP_OK)
    {
        logger_loge(TAG, "OTA update failed: %s", esp_err_to_name(err));
        logger_logw(TAG, "Flashing: %s; Packages received: %d; Status: OTA update failed", is_fw ? "OS" : "SPIFFS", 0);
        free(buf_url_q);
        return err;
    }
    
    logger_logi(TAG, "Prepare to restart system!");
   
    TimerHandle_t reboot_timer = xTimerCreate(
        "reboot_timer",              // name (for debugging)
        pdMS_TO_TICKS(2000),        // period in ticks (10 000 ms)
        pdFALSE,                     // pdFALSE = one‐shot, pdTRUE = auto‐reload
        NULL,                        // timer “ID” (not needed here)
        reboot_timer_cb             // callback
    );
    logger_loge(TAG, "OTA complete");

    if (xTimerStart(reboot_timer, /*ticks to wait*/ 100) != pdPASS) {
        logger_loge(TAG, "Failed to start reboot timer");
    }
    
    logger_logw(TAG, "Flashing: %s; Packages received: %d; Status: Rebooting now ...", is_fw ? "OS" : "SPIFFS", 100);
    
    // Success
    httpd_resp_set_status(req, "200");
    httpd_resp_set_type(req, "text/plain");                // optional
    httpd_resp_set_hdr(req, "Content-Length", "0");        // explicit zero-length
    httpd_resp_set_hdr(req, "Connection", "close");        // signal connection close
    // send an empty string (zero bytes) – curl sees Content-Length=0 and closes
    httpd_resp_send(req, "", HTTPD_RESP_USE_STRLEN);

    return ESP_OK;
}
