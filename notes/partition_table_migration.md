# Partition Table Migration

This note documents the OTA-based partition table migration that was used to move legacy 16 MB devices from the old layout:

```text
ota_0 @ 0x20000 size 0x180000
ota_1 @ 0x1A0000 size 0x180000
storage @ 0x320000
```

to the new layout:

```text
ota_0 @ 0x20000 size 0x190000
ota_1 @ 0x1B0000 size 0x190000
storage @ 0x340000
```

## What the migration firmware does

The migration image is a separate minimal firmware that still fits into the old `0x180000` OTA slots. On boot it:

1. Detects whether the device is still on the legacy partition table.
2. If the migration image is running from old `ota_1`, it copies itself into old `ota_0`, switches the boot partition to `ota_0`, and reboots.
3. If it is running from old `ota_0`, it rewrites the partition table at `0x8000` with the new 16 MB partition table and reboots.
4. After the reboot under the new layout, it serves a small migration web UI and reuses the existing `POST /update` OTA endpoint so the normal firmware can be uploaded.

Important limitations:

- This only makes sense for the 16 MB hardware.
- Dangerous flash writes must be enabled in the migration build.
- `storage` moves, so LittleFS contents should be treated as disposable. In practice the new firmware reformats or recreates that partition.
- Power loss while rewriting `0x8000` can brick the device.

## Files added

### `partitions_migration.csv`

```csv
# Name,      Type, SubType,  Offset,     Size,      Flags
nvs,         data, nvs,      0x9000,    0x6000,
phy_init,    data, phy,      0xf000,    0x1000,
otadata,     data, ota,      0x10000,   0x2000,
ota_0,       app,  ota_0,    0x20000,   0x180000,
ota_1,       app,  ota_1,    0x1A0000,  0x180000,
storage,     data, littlefs, 0x320000,  0xCE0000,
```

### `sdkconfig.defaults.migration`

```ini
CONFIG_PARTITION_MIGRATION_BUILD=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions_migration.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions_migration.csv"
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
CONFIG_NETWORK_WIFI_AP=y
CONFIG_NETWORK_WIFI_AP_SSID="CoinboxMigration"
CONFIG_NETWORK_WIFI_AP_PASSWORD=""
# CONFIG_NETWORK_WIFI_STA is not set
# CONFIG_SPI_FLASH_DANGEROUS_WRITE_ABORTS is not set
CONFIG_SPI_FLASH_DANGEROUS_WRITE_ALLOWED=y
```

### `main/migration.h`

```c
/*
 *	The MIT License (MIT)
 *
 *	Copyright (c) 2026 TuDo Makerspace
 *
 *	Permission is hereby granted, free of charge, to any person obtaining a copy
 *	of this software and associated documentation files (the "Software"), to deal
 *	in the Software without restriction, including without limitation the rights
 *	to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *	copies of the Software, and to permit persons to whom the Software is
 *	furnished to do so, subject to the following conditions:
 *
 *	The above copyright notice and this permission notice shall be included in all
 *	copies or substantial portions of the Software.
 *
 *	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *	IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *	FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *	AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *	LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *	OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *	SOFTWARE.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t migration_start(void);

#ifdef __cplusplus
}
#endif
```

### `main/migration_main.c`

```c
/*
 *	The MIT License (MIT)
 *
 *	Copyright (c) 2026 TuDo Makerspace
 *
 *	Permission is hereby granted, free of charge, to any person obtaining a copy
 *	of this software and associated documentation files (the "Software"), to deal
 *	in the Software without restriction, including without limitation the rights
 *	to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *	copies of the Software, and to permit persons to whom the Software is
 *	furnished to do so, subject to the following conditions:
 *
 *	The above copyright notice and this permission notice shall be included in all
 *	copies or substantial portions of the Software.
 *
 *	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *	IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *	FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *	AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *	LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *	OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *	SOFTWARE.
 */

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "logger.h"
#include "migration.h"

static const char *TAG = "migration_main";

void app_main(void)
{
    logger_init();
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    ESP_LOGI(TAG, "Starting Coinbox partition migration helper");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(migration_start());
}
```

### `main/migration.c`

```c
/*
 *	The MIT License (MIT)
 *
 *	Copyright (c) 2026 TuDo Makerspace
 *
 *	Permission is hereby granted, free of charge, to any person obtaining a copy
 *	of this software and associated documentation files (the "Software"), to deal
 *	in the Software without restriction, including without limitation the rights
 *	to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *	copies of the Software, and to permit persons to whom the Software is
 *	furnished to do so, subject to the following conditions:
 *
 *	The above copyright notice and this permission notice shall be included in all
 *	copies or substantial portions of the Software.
 *
 *	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *	IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *	FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *	AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *	LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *	OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *	SOFTWARE.
 */

#include "migration.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_flash.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "network.h"
#include "ota.h"
#include "sdkconfig.h"
#include "security.h"

#define MIGRATION_COPY_BUFFER_SIZE 4096
#define MIGRATION_LEGACY_OTA0_OFFSET 0x20000
#define MIGRATION_LEGACY_OTA1_OFFSET 0x1A0000
#define MIGRATION_TARGET_OTA1_OFFSET 0x1B0000
#define MIGRATION_TARGET_MIN_FLASH_SIZE (16U * 1024U * 1024U)
#define MIGRATION_PARTITION_TABLE_OFFSET 0x8000
#define MIGRATION_PARTITION_TABLE_ERASE_SIZE 0x1000

static const char *TAG = "migration";

extern const uint8_t _binary_migration_target_partition_table_bin_start[] asm("_binary_migration_target_partition_table_bin_start");
extern const uint8_t _binary_migration_target_partition_table_bin_end[] asm("_binary_migration_target_partition_table_bin_end");

typedef enum {
    MIGRATION_LAYOUT_UNKNOWN = 0,
    MIGRATION_LAYOUT_LEGACY,
    MIGRATION_LAYOUT_TARGET,
} migration_layout_t;

static httpd_handle_t s_server = NULL;
static bool s_upload_ready = false;
static migration_layout_t s_layout = MIGRATION_LAYOUT_UNKNOWN;
static uint32_t s_running_partition_address = 0;
static char s_status_message[192] = "Partition migration helper is starting.";

static void set_status_message(const char *message)
{
    if (!message) {
        s_status_message[0] = '\0';
        return;
    }
    strlcpy(s_status_message, message, sizeof(s_status_message));
}

static const char *layout_to_string(migration_layout_t layout)
{
    switch (layout) {
        case MIGRATION_LAYOUT_LEGACY:
            return "legacy";
        case MIGRATION_LAYOUT_TARGET:
            return "target";
        default:
            return "unknown";
    }
}

static migration_layout_t detect_layout(void)
{
    const esp_partition_t *ota1 =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
    if (!ota1) {
        return MIGRATION_LAYOUT_UNKNOWN;
    }
    if (ota1->address == MIGRATION_TARGET_OTA1_OFFSET) {
        return MIGRATION_LAYOUT_TARGET;
    }
    if (ota1->address == MIGRATION_LEGACY_OTA1_OFFSET) {
        return MIGRATION_LAYOUT_LEGACY;
    }
    return MIGRATION_LAYOUT_UNKNOWN;
}

static esp_err_t ensure_supported_flash_size(void)
{
    uint32_t flash_size = 0;
    esp_err_t err = esp_flash_get_size(NULL, &flash_size);
    if (err != ESP_OK) {
        return err;
    }
    if (flash_size < MIGRATION_TARGET_MIN_FLASH_SIZE) {
        ESP_LOGE(TAG, "Detected flash size 0x%" PRIx32 ", but migration requires at least 16 MB", flash_size);
        set_status_message("Flash size is smaller than 16 MB. This migration image will not modify the device.");
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

static void reboot_after_delay(const char *reason)
{
    if (reason && reason[0] != '\0') {
        ESP_LOGW(TAG, "%s", reason);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static esp_err_t copy_partition_image(const esp_partition_t *src, const esp_partition_t *dst)
{
    if (!src || !dst || src->size > dst->size) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t *buffer = malloc(MIGRATION_COPY_BUFFER_SIZE);
    if (!buffer) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_partition_erase_range(dst, 0, dst->size);
    if (err != ESP_OK) {
        free(buffer);
        return err;
    }

    for (size_t offset = 0; offset < src->size; offset += MIGRATION_COPY_BUFFER_SIZE) {
        size_t chunk = src->size - offset;
        if (chunk > MIGRATION_COPY_BUFFER_SIZE) {
            chunk = MIGRATION_COPY_BUFFER_SIZE;
        }

        err = esp_partition_read(src, offset, buffer, chunk);
        if (err != ESP_OK) {
            free(buffer);
            return err;
        }

        err = esp_partition_write(dst, offset, buffer, chunk);
        if (err != ESP_OK) {
            free(buffer);
            return err;
        }
    }

    free(buffer);
    return ESP_OK;
}

static esp_err_t promote_running_image_to_ota0_and_reboot(const esp_partition_t *running)
{
    const esp_partition_t *ota0 =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (!ota0) {
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGW(TAG, "Running from legacy ota_1 at 0x%08" PRIx32 "; copying migration image to ota_0", running->address);
    esp_err_t err = copy_partition_image(running, ota0);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_ota_set_boot_partition(ota0);
    if (err != ESP_OK) {
        return err;
    }

    reboot_after_delay("Migration image copied to ota_0. Rebooting to continue partition migration.");
    return ESP_OK;
}

static esp_err_t rewrite_partition_table_and_reboot(const esp_partition_t *running)
{
    if (!running) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t partition_table_len =
        (size_t)(_binary_migration_target_partition_table_bin_end - _binary_migration_target_partition_table_bin_start);
    if (partition_table_len == 0 || partition_table_len > MIGRATION_PARTITION_TABLE_ERASE_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = esp_ota_set_boot_partition(running);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGW(TAG, "Rewriting partition table at 0x%08x with %u bytes", MIGRATION_PARTITION_TABLE_OFFSET,
             (unsigned)partition_table_len);

    err = esp_flash_erase_region(NULL, MIGRATION_PARTITION_TABLE_OFFSET, MIGRATION_PARTITION_TABLE_ERASE_SIZE);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_flash_write(NULL, _binary_migration_target_partition_table_bin_start,
                          MIGRATION_PARTITION_TABLE_OFFSET, partition_table_len);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t verify[MIGRATION_PARTITION_TABLE_ERASE_SIZE];
    memset(verify, 0x00, sizeof(verify));
    err = esp_flash_read(NULL, verify, MIGRATION_PARTITION_TABLE_OFFSET, sizeof(verify));
    if (err != ESP_OK) {
        return err;
    }
    if (memcmp(verify, _binary_migration_target_partition_table_bin_start, partition_table_len) != 0) {
        return ESP_ERR_INVALID_CRC;
    }

    reboot_after_delay("Partition table updated. Rebooting into the migrated layout.");
    return ESP_OK;
}

static esp_err_t prepare_partition_layout(void)
{
    esp_err_t err = ensure_supported_flash_size();
    if (err != ESP_OK) {
        return err;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        set_status_message("Could not determine the running OTA partition.");
        return ESP_ERR_NOT_FOUND;
    }

    s_running_partition_address = running->address;
    s_layout = detect_layout();

    if (s_layout == MIGRATION_LAYOUT_TARGET) {
        s_upload_ready = true;
        set_status_message("Target partition layout is active. Upload the regular firmware image below.");
        ESP_LOGI(TAG, "Target partition layout already active; waiting for final firmware upload.");
        return ESP_OK;
    }

    if (s_layout != MIGRATION_LAYOUT_LEGACY) {
        set_status_message("Current partition layout is unknown. Migration helper did not change the device.");
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (running->address == MIGRATION_LEGACY_OTA1_OFFSET) {
        return promote_running_image_to_ota0_and_reboot(running);
    }

    if (running->address == MIGRATION_LEGACY_OTA0_OFFSET) {
        return rewrite_partition_table_and_reboot(running);
    }

    set_status_message("Running partition is not a legacy OTA slot. Migration helper did not change the device.");
    return ESP_ERR_INVALID_STATE;
}

static esp_err_t send_root_page(httpd_req_t *req)
{
    char ap_ip[16] = "-";
    char sta_ip[16] = "-";
    (void)network_get_ipv4_strings(ap_ip, sizeof(ap_ip), sta_ip, sizeof(sta_ip));

    bool password_required = security_is_password_set();
    char page[8192];
    int written = snprintf(
        page,
        sizeof(page),
        "<!doctype html>"
        "<html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Coinbox Migration</title>"
        "<style>"
        "body{font-family:system-ui,sans-serif;background:#f5f7fb;color:#10243e;margin:0;padding:2rem;}"
        "main{max-width:44rem;margin:0 auto;background:#fff;border-radius:1rem;padding:2rem;box-shadow:0 18px 40px rgba(16,36,62,.12);}"
        "h1{margin-top:0;font-size:1.8rem;}p,li{line-height:1.5;}code{background:#eef3ff;padding:.15rem .3rem;border-radius:.3rem;}"
        ".status{padding:1rem 1.1rem;border-radius:.8rem;background:#e8f0ff;margin:1rem 0;color:#17408b;}"
        ".meta{display:grid;gap:.4rem;margin:1rem 0 1.5rem;}"
        ".field{display:grid;gap:.35rem;margin:.9rem 0;}"
        "input[type=file],input[type=password],input[type=text],button{font:inherit;}"
        "input[type=password],input[type=text]{padding:.7rem .8rem;border:1px solid #c8d5ef;border-radius:.65rem;}"
        "button{padding:.8rem 1rem;border:0;border-radius:.7rem;background:#1f63f1;color:#fff;font-weight:700;cursor:pointer;}"
        "button[disabled]{opacity:.55;cursor:not-allowed;}small{color:#52627a;}"
        "#result{margin-top:1rem;white-space:pre-wrap;}"
        "</style></head><body><main>"
        "<h1>Coinbox Partition Migration</h1>"
        "<p>This helper is for devices that still run the legacy 16 MB OTA layout. Once the layout is updated, upload the normal <code>coinbox.bin</code> here.</p>"
        "<div class='status'>%s</div>"
        "<div class='meta'>"
        "<div>Layout: <strong>%s</strong></div>"
        "<div>Running partition: <code>0x%05" PRIx32 "</code></div>"
        "<div>Access point IP: <code>%s</code></div>"
        "<div>Station IP: <code>%s</code></div>"
        "</div>"
        "%s"
        "<p id='result'></p>"
        "<script>"
        "const form=document.getElementById('upload-form');"
        "if(form){form.addEventListener('submit',async(e)=>{"
        "e.preventDefault();"
        "const result=document.getElementById('result');"
        "const firmware=document.getElementById('firmware').files[0];"
        "const password=document.getElementById('ota-password').value.trim();"
        "const recovery=document.getElementById('ota-recovery').value.trim();"
        "if(!firmware){result.textContent='Select a firmware file first.';return;}"
        "if(password&&recovery){result.textContent='Provide either a password or a recovery code, not both.';return;}"
        "if(recovery&&!/^\\d{4}$/.test(recovery)){result.textContent='Recovery code must be exactly 4 digits.';return;}"
        "const headers={};"
        "if(password){headers['X-Coinbox-OTA-Password']=password;}"
        "if(recovery){headers['X-Coinbox-OTA-Recovery-Code']=recovery;}"
        "result.textContent='Uploading firmware. Keep this page open until the device reboots.';"
        "const button=document.getElementById('upload-button');"
        "button.disabled=true;"
        "try{const response=await fetch('/update',{method:'POST',headers,body:firmware});"
        "if(!response.ok){const text=await response.text();throw new Error(text||('HTTP '+response.status));}"
        "result.textContent='Upload finished. The device will reboot automatically.';"
        "}catch(error){result.textContent='Upload failed: '+error.message;button.disabled=false;}});}"
        "</script>"
        "</main></body></html>",
        s_status_message,
        layout_to_string(s_layout),
        s_running_partition_address,
        ap_ip,
        sta_ip,
        s_upload_ready
            ? (
                  password_required
                      ? "<form id='upload-form'><div class='field'><label for='firmware'>Firmware image</label><input id='firmware' type='file' accept='.bin,application/octet-stream'></div><div class='field'><label for='ota-password'>UI password</label><input id='ota-password' type='password' autocomplete='current-password'></div><div class='field'><label for='ota-recovery'>Recovery code</label><input id='ota-recovery' type='text' inputmode='numeric' maxlength='4' placeholder='1234'></div><small>If a UI password is configured, provide either the password or the 4-digit recovery code.</small><div class='field'><button id='upload-button' type='submit'>Upload firmware</button></div></form>"
                      : "<form id='upload-form'><div class='field'><label for='firmware'>Firmware image</label><input id='firmware' type='file' accept='.bin,application/octet-stream'></div><small>No OTA password is configured on this device.</small><div class='field'><button id='upload-button' type='submit'>Upload firmware</button></div></form>")
            : "<p>The helper did not reach an upload-ready state. Check the status message above and the serial log before proceeding.</p>");

    if (written < 0 || (size_t)written >= sizeof(page)) {
        return ESP_ERR_NO_MEM;
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t root_handler(httpd_req_t *req)
{
    return send_root_page(req);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    char payload[256];
    int written = snprintf(payload, sizeof(payload),
                           "{\"uploadReady\":%s,\"layout\":\"%s\",\"runningAddress\":\"0x%05" PRIx32
                           "\",\"status\":\"%s\"}",
                           s_upload_ready ? "true" : "false",
                           layout_to_string(s_layout),
                           s_running_partition_address,
                           s_status_message);
    if (written < 0 || (size_t)written >= sizeof(payload)) {
        return ESP_ERR_NO_MEM;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, payload, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t start_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 3;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t status = {
        .uri = "/migration/status",
        .method = HTTP_GET,
        .handler = status_handler,
        .user_ctx = NULL,
    };

    err = httpd_register_uri_handler(s_server, &root);
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(s_server, &status);
    }

    if (err == ESP_OK && s_upload_ready) {
        httpd_uri_t update = {
            .uri = "/update",
            .method = HTTP_POST,
            .handler = ota_update_handler,
            .user_ctx = NULL,
        };
        err = httpd_register_uri_handler(s_server, &update);
    }

    if (err != ESP_OK) {
        httpd_stop(s_server);
        s_server = NULL;
        return err;
    }

    return ESP_OK;
}

esp_err_t migration_start(void)
{
    esp_err_t err = prepare_partition_layout();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Migration preparation ended without applying the target layout: %s", esp_err_to_name(err));
    }

    err = security_init();
    if (err != ESP_OK) {
        return err;
    }

    err = init_wifi();
    if (err != ESP_OK) {
        return err;
    }

    err = network_enable_recovery_ap();
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
        return err;
    }

    err = start_server();
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "Migration helper ready at / (upload ready: %s)", s_upload_ready ? "yes" : "no");
    return ESP_OK;
}
```

## Files patched

### `main/Kconfig.projbuild`

Insert this near the top of the `menu "Coinbox Configuration"` section:

```diff
 menu "Coinbox Configuration"

+    config PARTITION_MIGRATION_BUILD
+        bool "Build partition migration firmware"
+        default n
+        help
+            Build a minimal recovery firmware that migrates legacy 16 MB OTA
+            layouts to the current partition table and then serves the normal
+            OTA upload endpoint for the final application image.
 
     menu "Network Settings"
```

### `main/CMakeLists.txt`

Patch the file like this:

```diff
@@
-set(COINBOX_EMBED_FILES
-    "embedded_files/favicon.ico"
-    "embedded_files/sounds.html"
-    "embedded_files/settings.html"
-    "embedded_files/navbar.js"
-    "embedded_files/connection_monitor.js"
-    "embedded_files/glyphs.js"
-    "embedded_files/glyphs.css"
-    "embedded_files/bootstrap.html"
-    "embedded_files/login.html"
-)
+set(COINBOX_SRCS
+    "ota.c"
+    "network.c"
+    "logger.c"
+    "recovery_code.c"
+    "security.c"
+)
+
+set(COINBOX_EMBED_FILES "")
+
+if(CONFIG_PARTITION_MIGRATION_BUILD)
+    list(APPEND COINBOX_SRCS
+        "migration.c"
+        "migration_main.c"
+    )
+else()
+    set(COINBOX_EMBED_FILES
+        "embedded_files/favicon.ico"
+        "embedded_files/sounds.html"
+        "embedded_files/settings.html"
+        "embedded_files/navbar.js"
+        "embedded_files/connection_monitor.js"
+        "embedded_files/glyphs.js"
+        "embedded_files/glyphs.css"
+        "embedded_files/bootstrap.html"
+        "embedded_files/login.html"
+    )
+
+    list(APPEND COINBOX_SRCS
+        "mainapp.c"
+        "bootstrap.c"
+        "files.c"
+        "main.c"
+        "audio.c"
+        "gpio.c"
+        "mdns_service.c"
+        "device_info.c"
+        "runtime_status.c"
+    )
+endif()
@@
-if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/embedded_files/default.mp3")
-    list(APPEND COINBOX_EMBED_FILES "embedded_files/default.mp3")
-    list(APPEND COINBOX_EMBED_DEFINITIONS COINBOX_HAS_EMBEDDED_DEFAULT_MP3=1)
-elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/embedded_files/fallback.mp3")
-    list(APPEND COINBOX_EMBED_FILES "embedded_files/fallback.mp3")
-    list(APPEND COINBOX_EMBED_DEFINITIONS COINBOX_HAS_EMBEDDED_FALLBACK_MP3=1)
-else()
-    message(FATAL_ERROR "Coinbox requires embedded_files/default.mp3 or embedded_files/fallback.mp3.")
-endif()
+if(NOT CONFIG_PARTITION_MIGRATION_BUILD)
+    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/embedded_files/default.mp3")
+        list(APPEND COINBOX_EMBED_FILES "embedded_files/default.mp3")
+        list(APPEND COINBOX_EMBED_DEFINITIONS COINBOX_HAS_EMBEDDED_DEFAULT_MP3=1)
+    elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/embedded_files/fallback.mp3")
+        list(APPEND COINBOX_EMBED_FILES "embedded_files/fallback.mp3")
+        list(APPEND COINBOX_EMBED_DEFINITIONS COINBOX_HAS_EMBEDDED_FALLBACK_MP3=1)
+    else()
+        message(FATAL_ERROR "Coinbox requires embedded_files/default.mp3 or embedded_files/fallback.mp3.")
+    endif()
+endif()
@@
 idf_component_register(
     SRCS
-        "mainapp.c"
-        "bootstrap.c"
-        "files.c"
-        "ota.c"
-        "main.c"
-        "network.c"
-        "logger.c"
-        "audio.c"
-        "gpio.c"
-        "mdns_service.c"
-        "recovery_code.c"
-        "device_info.c"
-        "runtime_status.c"
-        "security.c"
+        ${COINBOX_SRCS}
     INCLUDE_DIRS
         "."
     EMBED_FILES
         ${COINBOX_EMBED_FILES}
 )
+
+if(CONFIG_PARTITION_MIGRATION_BUILD)
+    idf_build_get_property(build_dir BUILD_DIR)
+    idf_build_get_property(idf_path IDF_PATH)
+    idf_build_get_property(python PYTHON)
+
+    set(COINBOX_TARGET_PARTITION_TABLE_CSV "${CMAKE_CURRENT_LIST_DIR}/../partitions.csv")
+    set(COINBOX_TARGET_PARTITION_TABLE_BIN "${build_dir}/migration_target_partition_table.bin")
+
+    add_custom_command(
+        OUTPUT "${COINBOX_TARGET_PARTITION_TABLE_BIN}"
+        COMMAND "${python}" "${idf_path}/components/partition_table/gen_esp32part.py"
+                --flash-size 16MB
+                -q
+                "${COINBOX_TARGET_PARTITION_TABLE_CSV}"
+                "${COINBOX_TARGET_PARTITION_TABLE_BIN}"
+        MAIN_DEPENDENCY "${COINBOX_TARGET_PARTITION_TABLE_CSV}"
+        DEPENDS "${idf_path}/components/partition_table/gen_esp32part.py"
+        VERBATIM
+    )
+
+    target_add_binary_data(${COMPONENT_LIB} "${COINBOX_TARGET_PARTITION_TABLE_BIN}" BINARY
+        RENAME_TO "migration_target_partition_table_bin"
+        DEPENDS "${COINBOX_TARGET_PARTITION_TABLE_BIN}")
+endif()
```

## Build commands

Build the migration image:

```bash
source /home/patrick/esp/v5.5.2/esp-idf/export.sh
export ADF_PATH=/home/patrick/esp/esp-adf

idf.py -B build_migration \
  -D SDKCONFIG=build_migration/sdkconfig \
  -D SDKCONFIG_DEFAULTS='sdkconfig.defaults;sdkconfig.defaults.migration' \
  reconfigure build
```

Build the normal 16 MB firmware:

```bash
idf.py -B build_16mb \
  -D SDKCONFIG=build_16mb/sdkconfig \
  -D SDKCONFIG_DEFAULTS='sdkconfig.defaults' \
  reconfigure build
```

The migration image should fit comfortably in the old `0x180000` slots. In the successful build that was tested, the migration image size was:

```text
coinbox.bin binary size 0xc8c00 bytes. Smallest app partition is 0x180000 bytes.
```

The normal firmware then fit in the new `0x190000` slots:

```text
coinbox.bin binary size 0x181c80 bytes. Smallest app partition is 0x190000 bytes.
```

## OTA usage

Upload the migration image first:

```bash
python3 tools/ota.py -d coinbox.local build_migration/coinbox.bin
```

After the migration firmware has rebooted through its sequence, upload the regular firmware:

```bash
python3 tools/ota.py -d 4.3.2.1 build_16mb/coinbox.bin
```

If `coinbox.local` is still reachable after the migration reboot, that hostname can also be used for the second step.

## Why this approach worked

- It did not require ESP-IDF partition-table OTA support or a pre-existing `partition_table, ota` staging partition.
- It avoided rewriting the partition table while executing from the old `ota_1` address.
- It reused the existing `/update` application OTA flow once the new partition layout was active.

## Future improvements

If this ever needs to become a long-term supported feature rather than a one-off recovery path, the next step should be designing the partition table up front with:

- a dedicated `partition_table, ota` partition
- a recovery bootloader partition
- an explicit storage migration or reset flow

That would let the partition table be updated in a more standard ESP-IDF way instead of using a custom raw-flash migration app.
