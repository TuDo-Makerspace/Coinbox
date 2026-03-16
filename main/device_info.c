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

#include "device_info.h"

#include <stdio.h>

#include "esp_mac.h"
#include "recovery_code.h"

#ifndef COINBOX_FW_VERSION
#define COINBOX_FW_VERSION "0.0.0"
#endif

#ifndef COINBOX_HW_VERSION
#define COINBOX_HW_VERSION "0.0.0"
#endif

///////////////////////////////////////////////////////////////////////////////////////////////////
// Constants
///////////////////////////////////////////////////////////////////////////////////////////////////

static const char *s_firmware_version = COINBOX_FW_VERSION;
static const char *s_hardware_version = COINBOX_HW_VERSION;

///////////////////////////////////////////////////////////////////////////////////////////////////
// Public Interface
///////////////////////////////////////////////////////////////////////////////////////////////////

const char *device_info_firmware_version(void)
{
    return s_firmware_version;
}

const char *device_info_hardware_version(void)
{
    return s_hardware_version;
}

const char *device_info_vendor(void)
{
    return DEVICE_INFO_VENDOR;
}

const char *device_info_source_code_url(void)
{
    return DEVICE_INFO_SOURCE_CODE_URL;
}

const char *device_info_license_name(void)
{
    return DEVICE_INFO_LICENSE_NAME;
}

esp_err_t device_info_get_mac_string(char *out, size_t out_size)
{
    if (!out || out_size < 18) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t mac[6] = {0};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        out[0] = '\0';
        return err;
    }

    int len = snprintf(out,
                       out_size,
                       "%02x:%02x:%02x:%02x:%02x:%02x",
                       mac[0],
                       mac[1],
                       mac[2],
                       mac[3],
                       mac[4],
                       mac[5]);
    if (len <= 0 || len >= (int)out_size) {
        out[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

esp_err_t device_info_get_recovery_code_string(char *out, size_t out_size)
{
    if (!out || out_size < 5) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t mac[RECOVERY_CODE_MAC_LEN] = {0};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        out[0] = '\0';
        return err;
    }

    int recovery_code = 0;
    err = recovery_code_generate_for_mac(mac, &recovery_code);
    if (err != ESP_OK) {
        out[0] = '\0';
        return err;
    }

    int len = snprintf(out, out_size, "%04d", recovery_code);
    if (len <= 0 || len >= (int)out_size) {
        out[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}
