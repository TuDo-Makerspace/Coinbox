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
