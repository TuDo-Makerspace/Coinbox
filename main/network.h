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
esp_err_t network_get_public_config(network_public_config_t *out);

//-------------------------------------------------------------------------
// Configuration
//-------------------------------------------------------------------------

esp_err_t network_update_config(const char *ap_ssid,
                                const char *ap_password,
                                bool ap_password_provided,
                                const char *sta_ssid,
                                const char *sta_password,
                                bool sta_password_provided);
esp_err_t network_reset_config_to_defaults(bool apply_runtime_now);
esp_err_t network_enable_recovery_ap(void);
esp_err_t network_restore_configured_ap(void);
esp_err_t network_restore_configured_sta(void);

#endif // NETWORK_H
