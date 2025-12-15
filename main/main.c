/*
 * SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
/* HTTP File Server Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "ws_server.h"
#include "network.h"
#include "mount.h"
#include "logger.h"
#include "gpio.h"
#include "audio.h"
#include "files.h"
#include "bootstrap.h"
#include "mdns_service.h"

static const char *TAG = "main";

void app_main(void)
{
    logger_logi(TAG, "Starting Coinbox");
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Initialize file storage */
    const char* base_path = "/data";
    ESP_ERROR_CHECK(mount_storage(base_path));
    ESP_ERROR_CHECK(files_set_base_path(base_path));

    init_wifi();
    ESP_ERROR_CHECK(mdns_start_service());

    ESP_ERROR_CHECK(bootstrap(base_path));

    // /* Start the web server */
    // ESP_ERROR_CHECK(start_ws_server(base_path));
    // logger_logi(TAG, "Web server started");

    // configure_gpio();
    // audio_init();
}
