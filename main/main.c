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
#include "esp_log.h"
#include "nvs_flash.h"
#include "mainapp.h"
#include "network.h"
#include "logger.h"
#include "gpio.h"
#include "audio.h"
#include "files.h"
#include "bootstrap.h"
#include "mdns_service.h"

static const char *TAG = "main";

void app_main(void)
{
    logger_init();                                      // Initialize logger
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);    // Suppress expected handoff socket warnings
    ESP_LOGI(TAG, "Starting Coinbox");
    ESP_ERROR_CHECK(nvs_flash_init());                  // Initialize NVS
    ESP_ERROR_CHECK(esp_netif_init());                  // Initialize TCP/IP stack
    ESP_ERROR_CHECK(esp_event_loop_create_default());   // Create default event loop
    ESP_ERROR_CHECK(files_init());                      // Initialize file storage
    ESP_ERROR_CHECK(gpio_init());                       // Initialize GPIOs (inputs + ISRs)
    ESP_ERROR_CHECK(audio_init());                      // Initialize audio (I2S DAC + board-level mute)
    ESP_ERROR_CHECK(init_wifi());                       // Initialize WiFi
    ESP_ERROR_CHECK(mdns_start_service());              // Start mDNS service (coinbox.local)
    ESP_ERROR_CHECK(bootstrap());                       // Start bootstrap process
}
