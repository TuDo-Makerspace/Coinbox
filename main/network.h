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

#ifndef NETWORK_H
#define NETWORK_H

#include "esp_err.h"
#include <stddef.h>
#include <stdbool.h>

///////////////////////////////////////////////////////////////////////////////////////////////////
// Constants
///////////////////////////////////////////////////////////////////////////////////////////////////

#define NETWORK_WIFI_SSID_MAX_LEN 32
#define NETWORK_WIFI_PSK_MAX_LEN 64

///////////////////////////////////////////////////////////////////////////////////////////////////
// Types
///////////////////////////////////////////////////////////////////////////////////////////////////

typedef struct {
    char ap_ssid[NETWORK_WIFI_SSID_MAX_LEN + 1];
    bool ap_password_set;
    bool ap_reboot_required;
    bool disable_ap_when_sta_connected;
    bool disable_ap_when_sta_connected_reboot_required;
    char sta_ssid[NETWORK_WIFI_SSID_MAX_LEN + 1];
    bool sta_password_set;
    bool sta_reboot_required;
} network_public_config_t;

///////////////////////////////////////////////////////////////////////////////////////////////////
// Interface
///////////////////////////////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------
// Lifecycle
//-------------------------------------------------------------------------

esp_err_t init_wifi(void);

//-------------------------------------------------------------------------
// Status Queries
//-------------------------------------------------------------------------

esp_err_t network_get_ipv4_strings(char *ap_out, size_t ap_out_size,
                                   char *sta_out, size_t sta_out_size);
esp_err_t network_get_connected_sta_ssid(char *out, size_t out_size);
esp_err_t network_get_ap_runtime_disabled_for_sta(bool *out);
esp_err_t network_get_public_config(network_public_config_t *out);

//-------------------------------------------------------------------------
// Configuration
//-------------------------------------------------------------------------

esp_err_t network_update_config(const char *ap_ssid,
                                const char *ap_password,
                                bool ap_password_provided,
                                bool disable_ap_when_sta_connected,
                                bool disable_ap_when_sta_connected_provided,
                                const char *sta_ssid,
                                const char *sta_password,
                                bool sta_password_provided);
esp_err_t network_reset_config_to_defaults(bool apply_runtime_now);
esp_err_t network_enable_recovery_ap(void);
esp_err_t network_restore_configured_ap(void);
esp_err_t network_restore_configured_sta(void);

#endif // NETWORK_H
