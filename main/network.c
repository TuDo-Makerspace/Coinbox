#include "network.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif_net_stack.h"
#include "esp_netif.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#if IP_NAPT
#include "lwip/lwip_napt.h"
#endif
#include "lwip/err.h"
#include "lwip/sys.h"
#include "sdkconfig.h"

///////////////////////////////////////////////////////////////////////////////////////////////////
// Constants
///////////////////////////////////////////////////////////////////////////////////////////////////

#if CONFIG_NETWORK_WIFI_STA
  #define ESP_WIFI_STA_ENABLE      CONFIG_NETWORK_WIFI_STA
  #define DEFAULT_WIFI_STA_SSID    CONFIG_NETWORK_WIFI_STA_SSID
  #define DEFAULT_WIFI_STA_PASSWD  CONFIG_NETWORK_WIFI_STA_PASSWORD
  #define ESP_MAXIMUM_RETRY        CONFIG_NETWORK_WIFI_STA_RETRY
#else
  #define DEFAULT_WIFI_STA_SSID    ""
  #define DEFAULT_WIFI_STA_PASSWD  ""
#endif

#if CONFIG_NETWORK_WIFI_AP
  #define ESP_WIFI_AP_ENABLE      CONFIG_NETWORK_WIFI_AP
  #define DEFAULT_WIFI_AP_SSID    CONFIG_NETWORK_WIFI_AP_SSID
  #define DEFAULT_WIFI_AP_PASSWD  CONFIG_NETWORK_WIFI_AP_PASSWORD
  #define ESP_WIFI_CHANNEL        CONFIG_NETWORK_WIFI_AP_CHANNEL
  #define MAX_STA_CONN            CONFIG_NETWORK_WIFI_AP_MAX_CONNECTIONS
#else
  #define DEFAULT_WIFI_AP_SSID    ""
  #define DEFAULT_WIFI_AP_PASSWD  ""
#endif

#define WIFI_CONNECTED_BIT BIT0
#define DHCPS_OFFER_DNS    0x02

#define NETWORK_NVS_NAMESPACE "network_cfg"
#define NETWORK_NVS_KEY_AP_SSID "ap_ssid"
#define NETWORK_NVS_KEY_AP_PSK  "ap_psk"
#define NETWORK_NVS_KEY_AP_OFF_ON_STA "ap_off_sta"
#define NETWORK_NVS_KEY_STA_SSID "sta_ssid"
#define NETWORK_NVS_KEY_STA_PSK  "sta_psk"
#define RECOVERY_AP_SSID "coinboxrecovery"
#define SOFTAP_IPV4_ADDR "4.3.2.1"
#define SOFTAP_IPV4_NETMASK "255.255.255.0"

///////////////////////////////////////////////////////////////////////////////////////////////////
// Static Vars
///////////////////////////////////////////////////////////////////////////////////////////////////

static const char *TAG_AP  = "WiFi SoftAP";
static const char *TAG_STA = "WiFi Sta";
#if CONFIG_NETWORK_ETH_OPENETH
static const char *TAG_ETH = "Ethernet";
#endif

#if CONFIG_NETWORK_WIFI_STA
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
#endif
#if CONFIG_NETWORK_WIFI_AP
static esp_netif_t *s_esp_netif_ap = NULL;
#endif
#if CONFIG_NETWORK_WIFI_STA
static esp_netif_t *s_esp_netif_sta = NULL;
#endif
#if CONFIG_NETWORK_ETH_OPENETH
static esp_netif_t *s_esp_netif_eth = NULL;
static esp_eth_handle_t s_eth_handle = NULL;
static esp_eth_mac_t *s_eth_mac = NULL;
static esp_eth_phy_t *s_eth_phy = NULL;
static esp_eth_netif_glue_handle_t s_eth_glue = NULL;
#endif

///////////////////////////////////////////////////////////////////////////////////////////////////
// Types
///////////////////////////////////////////////////////////////////////////////////////////////////

typedef struct {
    char ap_ssid[NETWORK_WIFI_SSID_MAX_LEN + 1];
    char ap_password[NETWORK_WIFI_PSK_MAX_LEN + 1];
    bool disable_ap_when_sta_connected;
    char sta_ssid[NETWORK_WIFI_SSID_MAX_LEN + 1];
    char sta_password[NETWORK_WIFI_PSK_MAX_LEN + 1];
} network_runtime_config_t;

static network_runtime_config_t s_runtime_config;
static bool s_runtime_config_loaded = false;
#if CONFIG_NETWORK_WIFI_AP && CONFIG_NETWORK_WIFI_STA
static bool s_ap_shutdown_on_sta_connected_active = false;
static bool s_ap_runtime_disabled_for_sta = false;
static bool s_ap_recovery_mode = false;
#endif

///////////////////////////////////////////////////////////////////////////////////////////////////
// Forward Declarations
///////////////////////////////////////////////////////////////////////////////////////////////////

#if CONFIG_NETWORK_WIFI_AP
static esp_err_t configure_softap_ipv4(esp_netif_t *esp_netif_ap);
#endif
#if CONFIG_NETWORK_WIFI_AP && CONFIG_NETWORK_WIFI_STA
static esp_err_t softap_set_dns_addr(esp_netif_t *esp_netif_ap, esp_netif_t *esp_netif_sta);
static esp_err_t sync_ap_runtime_with_sta_policy(bool force_reapply_ap);
#endif
#if CONFIG_NETWORK_ETH_OPENETH
static esp_err_t ethernet_init_openeth(esp_netif_t **out_esp_netif_eth);
#endif

///////////////////////////////////////////////////////////////////////////////////////////////////
// Helpers
///////////////////////////////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------
// Config Validation
//-------------------------------------------------------------------------

static void copy_or_empty(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }
    strlcpy(dst, src ? src : "", dst_size);
}

static bool is_valid_ssid(const char *ssid)
{
    if (!ssid) {
        return false;
    }
    size_t len = strlen(ssid);
    return len > 0 && len <= NETWORK_WIFI_SSID_MAX_LEN;
}

static bool is_valid_password_len(const char *password)
{
    if (!password) {
        return false;
    }
    size_t len = strlen(password);
    if (len == 0) {
        return true;  // open network
    }
    return len >= 8 && len <= NETWORK_WIFI_PSK_MAX_LEN;
}

static void set_runtime_defaults(network_runtime_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    copy_or_empty(cfg->ap_ssid, sizeof(cfg->ap_ssid), DEFAULT_WIFI_AP_SSID);
    copy_or_empty(cfg->ap_password, sizeof(cfg->ap_password), DEFAULT_WIFI_AP_PASSWD);
    cfg->disable_ap_when_sta_connected = false;
    copy_or_empty(cfg->sta_ssid, sizeof(cfg->sta_ssid), DEFAULT_WIFI_STA_SSID);
    copy_or_empty(cfg->sta_password, sizeof(cfg->sta_password), DEFAULT_WIFI_STA_PASSWD);
}

static void sanitize_loaded_config(network_runtime_config_t *cfg)
{
    if (!cfg) {
        return;
    }

    if (!is_valid_ssid(cfg->ap_ssid)) {
        copy_or_empty(cfg->ap_ssid, sizeof(cfg->ap_ssid), DEFAULT_WIFI_AP_SSID);
    }
    if (!is_valid_password_len(cfg->ap_password)) {
        copy_or_empty(cfg->ap_password, sizeof(cfg->ap_password), DEFAULT_WIFI_AP_PASSWD);
    }

#if !(CONFIG_NETWORK_WIFI_AP && CONFIG_NETWORK_WIFI_STA)
    cfg->disable_ap_when_sta_connected = false;
#endif

#if CONFIG_NETWORK_WIFI_STA
    if (cfg->sta_ssid[0] != '\0' && !is_valid_ssid(cfg->sta_ssid)) {
        copy_or_empty(cfg->sta_ssid, sizeof(cfg->sta_ssid), DEFAULT_WIFI_STA_SSID);
    }
#else
    cfg->sta_ssid[0] = '\0';
#endif
    if (!is_valid_password_len(cfg->sta_password)) {
        copy_or_empty(cfg->sta_password, sizeof(cfg->sta_password), DEFAULT_WIFI_STA_PASSWD);
    }
}

static void load_nvs_string_or_default(nvs_handle_t nvs, const char *key, char *out, size_t out_size, const char *fallback)
{
    if (!out || out_size == 0) {
        return;
    }

    size_t required = out_size;
    esp_err_t err = nvs_get_str(nvs, key, out, &required);
    if (err == ESP_OK) {
        return;
    }
    if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG_STA, "NVS read failed for %s: %s", key, esp_err_to_name(err));
    }
    copy_or_empty(out, out_size, fallback);
}

static void load_nvs_bool_or_default(nvs_handle_t nvs, const char *key, bool *out, bool fallback)
{
    if (!out) {
        return;
    }

    uint8_t value = fallback ? 1 : 0;
    esp_err_t err = nvs_get_u8(nvs, key, &value);
    if (err == ESP_OK) {
        *out = (value != 0);
        return;
    }
    if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG_STA, "NVS read failed for %s: %s", key, esp_err_to_name(err));
    }
    *out = fallback;
}

//-------------------------------------------------------------------------
// Runtime Config Persistence
//-------------------------------------------------------------------------

static esp_err_t ensure_runtime_config_loaded(void)
{
    if (s_runtime_config_loaded) {
        return ESP_OK;
    }

    set_runtime_defaults(&s_runtime_config);

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(NETWORK_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        load_nvs_string_or_default(nvs, NETWORK_NVS_KEY_AP_SSID,
                                   s_runtime_config.ap_ssid, sizeof(s_runtime_config.ap_ssid),
                                   DEFAULT_WIFI_AP_SSID);
        load_nvs_string_or_default(nvs, NETWORK_NVS_KEY_AP_PSK,
                                   s_runtime_config.ap_password, sizeof(s_runtime_config.ap_password),
                                   DEFAULT_WIFI_AP_PASSWD);
        load_nvs_bool_or_default(nvs, NETWORK_NVS_KEY_AP_OFF_ON_STA,
                                 &s_runtime_config.disable_ap_when_sta_connected, false);
        load_nvs_string_or_default(nvs, NETWORK_NVS_KEY_STA_SSID,
                                   s_runtime_config.sta_ssid, sizeof(s_runtime_config.sta_ssid),
                                   DEFAULT_WIFI_STA_SSID);
        load_nvs_string_or_default(nvs, NETWORK_NVS_KEY_STA_PSK,
                                   s_runtime_config.sta_password, sizeof(s_runtime_config.sta_password),
                                   DEFAULT_WIFI_STA_PASSWD);
        nvs_close(nvs);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG_STA, "Failed to open Wi-Fi config namespace: %s", esp_err_to_name(err));
    }

    sanitize_loaded_config(&s_runtime_config);
    s_runtime_config_loaded = true;
    return ESP_OK;
}

static esp_err_t persist_runtime_config(const network_runtime_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(NETWORK_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs, NETWORK_NVS_KEY_AP_SSID, cfg->ap_ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, NETWORK_NVS_KEY_AP_PSK, cfg->ap_password);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, NETWORK_NVS_KEY_AP_OFF_ON_STA,
                         cfg->disable_ap_when_sta_connected ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, NETWORK_NVS_KEY_STA_SSID, cfg->sta_ssid);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, NETWORK_NVS_KEY_STA_PSK, cfg->sta_password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }

    nvs_close(nvs);
    return err;
}

#if CONFIG_NETWORK_WIFI_AP
static esp_err_t apply_ap_config(const char *ssid, const char *password)
{
    if (!is_valid_ssid(ssid) || !is_valid_password_len(password)) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t wifi_ap_config = {
        .ap = {
            .ssid_len = (uint8_t)strlen(ssid),
            .channel = ESP_WIFI_CHANNEL,
            .max_connection = MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {.required = false},
        },
    };

    copy_or_empty((char *)wifi_ap_config.ap.ssid, sizeof(wifi_ap_config.ap.ssid), ssid);
    copy_or_empty((char *)wifi_ap_config.ap.password, sizeof(wifi_ap_config.ap.password), password);
    if (password[0] == '\0') {
        wifi_ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    return esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config);
}
#endif

#if CONFIG_NETWORK_WIFI_STA
static esp_err_t apply_sta_config(const char *ssid, const char *password)
{
    if (!ssid || !password || strlen(ssid) > NETWORK_WIFI_SSID_MAX_LEN || !is_valid_password_len(password)) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t wifi_sta_config = {
        .sta = {
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .failure_retry_cnt = ESP_MAXIMUM_RETRY,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };

    copy_or_empty((char *)wifi_sta_config.sta.ssid, sizeof(wifi_sta_config.sta.ssid), ssid);
    copy_or_empty((char *)wifi_sta_config.sta.password, sizeof(wifi_sta_config.sta.password), password);

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config);
    if (err != ESP_OK) {
        return err;
    }

    if (ssid[0] == '\0') {
        err = esp_wifi_disconnect();
        if (err == ESP_ERR_WIFI_NOT_CONNECT || err == ESP_ERR_WIFI_NOT_STARTED || err == ESP_ERR_WIFI_NOT_INIT) {
            return ESP_OK;
        }
        return err;
    }

    err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT && err != ESP_ERR_WIFI_NOT_STARTED && err != ESP_ERR_WIFI_NOT_INIT) {
        return err;
    }

    err = esp_wifi_connect();
    if (err == ESP_ERR_WIFI_NOT_STARTED || err == ESP_ERR_WIFI_NOT_INIT) {
        return ESP_OK;
    }
    return err;
}
#endif

#if CONFIG_NETWORK_WIFI_AP && CONFIG_NETWORK_WIFI_STA
static bool is_sta_connected_for_policy(void)
{
    if (!s_wifi_event_group) {
        return false;
    }
    return (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
}

static const char *current_ap_ssid(void)
{
    return s_ap_recovery_mode ? RECOVERY_AP_SSID : s_runtime_config.ap_ssid;
}

static const char *current_ap_password(void)
{
    return s_ap_recovery_mode ? "" : s_runtime_config.ap_password;
}

static esp_err_t set_wifi_mode_if_needed(wifi_mode_t desired_mode)
{
    wifi_mode_t current_mode = WIFI_MODE_NULL;
    esp_err_t err = esp_wifi_get_mode(&current_mode);
    if (err != ESP_OK) {
        return err;
    }
    if (current_mode == desired_mode) {
        return ESP_OK;
    }
    return esp_wifi_set_mode(desired_mode);
}

static esp_err_t ensure_ap_runtime_enabled(bool force_reapply_ap)
{
    esp_err_t err = set_wifi_mode_if_needed(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        return err;
    }

    if (force_reapply_ap || s_ap_runtime_disabled_for_sta) {
        err = configure_softap_ipv4(s_esp_netif_ap);
        if (err != ESP_OK) {
            return err;
        }

        err = apply_ap_config(current_ap_ssid(), current_ap_password());
        if (err != ESP_OK) {
            return err;
        }
    }

    s_ap_runtime_disabled_for_sta = false;
    return ESP_OK;
}

static esp_err_t ensure_ap_runtime_disabled(void)
{
    esp_err_t err = set_wifi_mode_if_needed(WIFI_MODE_STA);
    if (err != ESP_OK) {
        return err;
    }

    s_ap_runtime_disabled_for_sta = true;
    return ESP_OK;
}

static esp_err_t sync_ap_runtime_with_sta_policy(bool force_reapply_ap)
{
    if (!s_esp_netif_ap) {
        return ESP_OK;
    }

    if (s_ap_shutdown_on_sta_connected_active &&
        !s_ap_recovery_mode &&
        is_sta_connected_for_policy()) {
        return ensure_ap_runtime_disabled();
    }

    return ensure_ap_runtime_enabled(force_reapply_ap);
}
#endif

#if CONFIG_NETWORK_WIFI_AP || CONFIG_NETWORK_WIFI_STA || CONFIG_NETWORK_ETH_OPENETH
//-------------------------------------------------------------------------
// IP Queries
//-------------------------------------------------------------------------

static bool fetch_ipv4_for_ifkey(const char *if_key, char *out, size_t out_size)
{
    if (!if_key || !out || out_size == 0) {
        return false;
    }
    out[0] = '\0';

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey(if_key);
    if (!netif) {
        return false;
    }

    esp_netif_ip_info_t ip_info = {0};
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        return false;
    }
    if (ip_info.ip.addr == 0) {
        return false;
    }

    int n = snprintf(out, out_size, IPSTR, IP2STR(&ip_info.ip));
    return (n > 0 && (size_t)n < out_size);
}
#endif

///////////////////////////////////////////////////////////////////////////////////////////////////
// Public
///////////////////////////////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------
// Status Queries
//-------------------------------------------------------------------------

esp_err_t network_get_ipv4_strings(char *ap_out, size_t ap_out_size,
                                   char *sta_out, size_t sta_out_size)
{
    bool found_any = false;

    if (ap_out && ap_out_size > 0) {
        ap_out[0] = '\0';
#if CONFIG_NETWORK_WIFI_AP
#if CONFIG_NETWORK_WIFI_STA
        if (!s_ap_runtime_disabled_for_sta) {
            found_any |= fetch_ipv4_for_ifkey("WIFI_AP_DEF", ap_out, ap_out_size);
        }
#else
        found_any |= fetch_ipv4_for_ifkey("WIFI_AP_DEF", ap_out, ap_out_size);
#endif
#endif
#if CONFIG_NETWORK_ETH_OPENETH
        if (ap_out[0] == '\0') {
            found_any |= fetch_ipv4_for_ifkey("ETH_DEF", ap_out, ap_out_size);
        }
#endif
    }

    if (sta_out && sta_out_size > 0) {
        sta_out[0] = '\0';
#if CONFIG_NETWORK_WIFI_STA
        found_any |= fetch_ipv4_for_ifkey("WIFI_STA_DEF", sta_out, sta_out_size);
#endif
#if CONFIG_NETWORK_ETH_OPENETH
        if (sta_out[0] == '\0') {
            found_any |= fetch_ipv4_for_ifkey("ETH_DEF", sta_out, sta_out_size);
        }
#endif
    }

    return found_any ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t network_get_connected_sta_ssid(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

#if CONFIG_NETWORK_WIFI_STA
    wifi_ap_record_t ap_info = {0};
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
    if (err == ESP_OK) {
        strlcpy(out, (const char *)ap_info.ssid, out_size);
        return out[0] ? ESP_OK : ESP_ERR_NOT_FOUND;
    }
    if (err == ESP_ERR_WIFI_NOT_CONNECT) {
        return ESP_ERR_NOT_FOUND;
    }
    return err;
#elif CONFIG_NETWORK_ETH_OPENETH
    char eth_ip[16] = {0};
    if (fetch_ipv4_for_ifkey("ETH_DEF", eth_ip, sizeof(eth_ip))) {
        strlcpy(out, "open_eth", out_size);
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t network_get_ap_runtime_disabled_for_sta(bool *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }

#if CONFIG_NETWORK_WIFI_AP && CONFIG_NETWORK_WIFI_STA
    *out = s_ap_runtime_disabled_for_sta;
    return ESP_OK;
#else
    *out = false;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

#if CONFIG_NETWORK_WIFI_AP
static bool has_pending_ap_reboot_requirement(void)
{
    wifi_config_t applied = {0};
    esp_err_t err = esp_wifi_get_config(WIFI_IF_AP, &applied);
    if (err != ESP_OK) {
        return false;
    }

    char applied_ssid[NETWORK_WIFI_SSID_MAX_LEN + 1] = {0};
    size_t ssid_len = (size_t)applied.ap.ssid_len;
    if (ssid_len > NETWORK_WIFI_SSID_MAX_LEN) {
        ssid_len = NETWORK_WIFI_SSID_MAX_LEN;
    }
    memcpy(applied_ssid, applied.ap.ssid, ssid_len);
    applied_ssid[ssid_len] = '\0';

    char applied_password[NETWORK_WIFI_PSK_MAX_LEN + 1] = {0};
    size_t password_len = strnlen((const char *)applied.ap.password, sizeof(applied.ap.password));
    if (password_len > NETWORK_WIFI_PSK_MAX_LEN) {
        password_len = NETWORK_WIFI_PSK_MAX_LEN;
    }
    memcpy(applied_password, applied.ap.password, password_len);
    applied_password[password_len] = '\0';

    return strcmp(s_runtime_config.ap_ssid, applied_ssid) != 0 ||
           strcmp(s_runtime_config.ap_password, applied_password) != 0;
}
#endif

#if CONFIG_NETWORK_WIFI_AP && CONFIG_NETWORK_WIFI_STA
static bool has_pending_ap_shutdown_policy_reboot_requirement(void)
{
    return s_runtime_config.disable_ap_when_sta_connected != s_ap_shutdown_on_sta_connected_active;
}
#endif

#if CONFIG_NETWORK_WIFI_STA
static bool has_pending_sta_reboot_requirement(void)
{
    wifi_config_t applied = {0};
    esp_err_t err = esp_wifi_get_config(WIFI_IF_STA, &applied);
    if (err != ESP_OK) {
        return false;
    }

    char applied_ssid[NETWORK_WIFI_SSID_MAX_LEN + 1] = {0};
    size_t ssid_len = strnlen((const char *)applied.sta.ssid, sizeof(applied.sta.ssid));
    if (ssid_len > NETWORK_WIFI_SSID_MAX_LEN) {
        ssid_len = NETWORK_WIFI_SSID_MAX_LEN;
    }
    memcpy(applied_ssid, applied.sta.ssid, ssid_len);
    applied_ssid[ssid_len] = '\0';

    char applied_password[NETWORK_WIFI_PSK_MAX_LEN + 1] = {0};
    size_t password_len = strnlen((const char *)applied.sta.password, sizeof(applied.sta.password));
    if (password_len > NETWORK_WIFI_PSK_MAX_LEN) {
        password_len = NETWORK_WIFI_PSK_MAX_LEN;
    }
    memcpy(applied_password, applied.sta.password, password_len);
    applied_password[password_len] = '\0';

    return strcmp(s_runtime_config.sta_ssid, applied_ssid) != 0 ||
           strcmp(s_runtime_config.sta_password, applied_password) != 0;
}
#endif

//-------------------------------------------------------------------------
// Config State
//-------------------------------------------------------------------------

esp_err_t network_get_public_config(network_public_config_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ensure_runtime_config_loaded();
    if (err != ESP_OK) {
        return err;
    }

    memset(out, 0, sizeof(*out));
    copy_or_empty(out->ap_ssid, sizeof(out->ap_ssid), s_runtime_config.ap_ssid);
    copy_or_empty(out->sta_ssid, sizeof(out->sta_ssid), s_runtime_config.sta_ssid);
    out->ap_password_set = (s_runtime_config.ap_password[0] != '\0');
    out->disable_ap_when_sta_connected = s_runtime_config.disable_ap_when_sta_connected;
#if CONFIG_NETWORK_WIFI_AP && CONFIG_NETWORK_WIFI_STA
    out->disable_ap_when_sta_connected_reboot_required =
        has_pending_ap_shutdown_policy_reboot_requirement();
#endif
    out->sta_password_set = (s_runtime_config.sta_password[0] != '\0');
#if CONFIG_NETWORK_WIFI_AP
    out->ap_reboot_required = has_pending_ap_reboot_requirement();
#endif
#if CONFIG_NETWORK_WIFI_STA
    out->sta_reboot_required = has_pending_sta_reboot_requirement();
#endif
    return ESP_OK;
}

//-------------------------------------------------------------------------
// Config Mutations
//-------------------------------------------------------------------------

esp_err_t network_update_config(const char *ap_ssid,
                                const char *ap_password,
                                bool ap_password_provided,
                                bool disable_ap_when_sta_connected,
                                bool disable_ap_when_sta_connected_provided,
                                const char *sta_ssid,
                                const char *sta_password,
                                bool sta_password_provided)
{
    esp_err_t err = ensure_runtime_config_loaded();
    if (err != ESP_OK) {
        return err;
    }

    network_runtime_config_t next = s_runtime_config;

#if CONFIG_NETWORK_WIFI_AP
    if (!is_valid_ssid(ap_ssid)) {
        return ESP_ERR_INVALID_ARG;
    }
    copy_or_empty(next.ap_ssid, sizeof(next.ap_ssid), ap_ssid);
    if (ap_password_provided) {
        if (!is_valid_password_len(ap_password)) {
            return ESP_ERR_INVALID_ARG;
        }
        copy_or_empty(next.ap_password, sizeof(next.ap_password), ap_password);
    }
#else
    if ((ap_ssid && ap_ssid[0] != '\0') || ap_password_provided) {
        return ESP_ERR_NOT_SUPPORTED;
    }
#endif

#if CONFIG_NETWORK_WIFI_AP && CONFIG_NETWORK_WIFI_STA
    if (disable_ap_when_sta_connected_provided) {
        next.disable_ap_when_sta_connected = disable_ap_when_sta_connected;
    }
#else
    if (disable_ap_when_sta_connected_provided) {
        return ESP_ERR_NOT_SUPPORTED;
    }
#endif

#if CONFIG_NETWORK_WIFI_STA
    if (!sta_ssid || strlen(sta_ssid) > NETWORK_WIFI_SSID_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    copy_or_empty(next.sta_ssid, sizeof(next.sta_ssid), sta_ssid);
    if (sta_password_provided) {
        if (!is_valid_password_len(sta_password)) {
            return ESP_ERR_INVALID_ARG;
        }
        copy_or_empty(next.sta_password, sizeof(next.sta_password), sta_password);
    }
#else
    if ((sta_ssid && sta_ssid[0] != '\0') || sta_password_provided) {
        return ESP_ERR_NOT_SUPPORTED;
    }
#endif

    sanitize_loaded_config(&next);

    bool ap_changed = false;
    bool ap_policy_changed = false;
    bool sta_changed = false;

#if CONFIG_NETWORK_WIFI_AP
    ap_changed = (strcmp(next.ap_ssid, s_runtime_config.ap_ssid) != 0) ||
                 (strcmp(next.ap_password, s_runtime_config.ap_password) != 0);
#endif
#if CONFIG_NETWORK_WIFI_AP && CONFIG_NETWORK_WIFI_STA
    ap_policy_changed = (next.disable_ap_when_sta_connected !=
                         s_runtime_config.disable_ap_when_sta_connected);
#endif
#if CONFIG_NETWORK_WIFI_STA
    sta_changed = (strcmp(next.sta_ssid, s_runtime_config.sta_ssid) != 0) ||
                  (strcmp(next.sta_password, s_runtime_config.sta_password) != 0);
#endif

    if (!ap_changed && !ap_policy_changed && !sta_changed) {
        return ESP_OK;
    }

    err = persist_runtime_config(&next);
    if (err != ESP_OK) {
        return err;
    }

    s_runtime_config = next;

    return ESP_OK;
}

esp_err_t network_reset_config_to_defaults(bool apply_runtime_now)
{
    network_runtime_config_t defaults = {0};
    set_runtime_defaults(&defaults);
    sanitize_loaded_config(&defaults);

    esp_err_t err = persist_runtime_config(&defaults);
    if (err != ESP_OK) {
        return err;
    }

    s_runtime_config = defaults;
    s_runtime_config_loaded = true;
#if CONFIG_NETWORK_WIFI_AP && CONFIG_NETWORK_WIFI_STA
    s_ap_recovery_mode = false;
#endif

    if (!apply_runtime_now) {
        return ESP_OK;
    }

#if CONFIG_NETWORK_WIFI_AP && CONFIG_NETWORK_WIFI_STA
    s_ap_shutdown_on_sta_connected_active = s_runtime_config.disable_ap_when_sta_connected;
#endif

#if CONFIG_NETWORK_WIFI_AP
    err = apply_ap_config(s_runtime_config.ap_ssid, s_runtime_config.ap_password);
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED && err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(TAG_AP, "Failed to apply default AP config immediately: %s", esp_err_to_name(err));
    }
#endif

#if CONFIG_NETWORK_WIFI_STA
    err = apply_sta_config(s_runtime_config.sta_ssid, s_runtime_config.sta_password);
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED && err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(TAG_STA, "Failed to apply default STA config immediately: %s", esp_err_to_name(err));
    }
#endif

#if CONFIG_NETWORK_WIFI_AP && CONFIG_NETWORK_WIFI_STA
    err = sync_ap_runtime_with_sta_policy(true);
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED && err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(TAG_AP, "Failed to sync AP/STA policy after reset: %s", esp_err_to_name(err));
    }
#endif

    return ESP_OK;
}

esp_err_t network_enable_recovery_ap(void)
{
    ESP_LOGI(TAG_AP, "Creating AP: %s", RECOVERY_AP_SSID);

#if CONFIG_NETWORK_WIFI_AP
    esp_err_t err = ESP_OK;

#if CONFIG_NETWORK_WIFI_STA
    s_ap_recovery_mode = true;
    err = sync_ap_runtime_with_sta_policy(true);
#else
    err = apply_ap_config(RECOVERY_AP_SSID, "");
#endif
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED && err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGE(TAG_AP, "Failed to switch AP into recovery mode: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGW(TAG_AP, "Recovery AP enabled (open SSID: %s)", RECOVERY_AP_SSID);
    return ESP_OK;
#else
    ESP_LOGW(TAG_AP, "Recovery AP could not be created: CONFIG_NETWORK_WIFI_AP is disabled");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t network_restore_configured_ap(void)
{
#if CONFIG_NETWORK_WIFI_AP
    esp_err_t err = ensure_runtime_config_loaded();
    if (err != ESP_OK) {
        return err;
    }

#if CONFIG_NETWORK_WIFI_STA
    s_ap_recovery_mode = false;
    err = sync_ap_runtime_with_sta_policy(true);
#else
    err = apply_ap_config(s_runtime_config.ap_ssid, s_runtime_config.ap_password);
#endif
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED && err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGE(TAG_AP, "Failed to restore configured AP: %s", esp_err_to_name(err));
        return err;
    }

  #if CONFIG_NETWORK_WIFI_STA
    if (s_ap_runtime_disabled_for_sta) {
        ESP_LOGI(TAG_AP, "Configured AP kept disabled because STA is connected");
    } else
  #endif
    {
        ESP_LOGI(TAG_AP, "Configured AP restored. SSID:%s auth:%s",
                 s_runtime_config.ap_ssid,
                 s_runtime_config.ap_password[0] ? "wpa2-psk" : "open");
    }
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t network_restore_configured_sta(void)
{
#if CONFIG_NETWORK_WIFI_STA
    esp_err_t err = ensure_runtime_config_loaded();
    if (err != ESP_OK) {
        return err;
    }

    err = apply_sta_config(s_runtime_config.sta_ssid, s_runtime_config.sta_password);
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED && err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGE(TAG_STA, "Failed to restore configured STA: %s", esp_err_to_name(err));
        return err;
    }

    if (s_runtime_config.sta_ssid[0]) {
        ESP_LOGI(TAG_STA, "Configured STA restored. SSID:%s", s_runtime_config.sta_ssid);
    } else {
        ESP_LOGI(TAG_STA, "Configured STA restored with empty SSID (disconnected)");
    }
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Event Handling
///////////////////////////////////////////////////////////////////////////////////////////////////

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == ETH_EVENT && event_id == ETHERNET_EVENT_CONNECTED) {
#if CONFIG_NETWORK_ETH_OPENETH
        ESP_LOGI(TAG_ETH, "OpenCores Ethernet link up");
#endif

    } else if (event_base == ETH_EVENT && event_id == ETHERNET_EVENT_DISCONNECTED) {
#if CONFIG_NETWORK_ETH_OPENETH
        ESP_LOGW(TAG_ETH, "OpenCores Ethernet link down");
#endif

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_ETH_GOT_IP) {
#if CONFIG_NETWORK_ETH_OPENETH
        ip_event_got_ip_t *event = event_data;
        ESP_LOGI(TAG_ETH, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
#endif

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
  #if CONFIG_NETWORK_WIFI_AP
        wifi_event_ap_staconnected_t *event = event_data;
        ESP_LOGI(TAG_AP, "Station "MACSTR" joined, AID=%d",
                 MAC2STR(event->mac), event->aid);
  #endif

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
  #if CONFIG_NETWORK_WIFI_AP
        wifi_event_ap_stadisconnected_t *event = event_data;
        ESP_LOGI(TAG_AP, "Station "MACSTR" left, AID=%d, reason:%d",
                 MAC2STR(event->mac), event->aid, event->reason);
  #endif

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
  #if CONFIG_NETWORK_WIFI_STA
        if (s_runtime_config.sta_ssid[0] != '\0') {
            esp_wifi_connect();
            ESP_LOGI(TAG_STA, "Station started");
        } else {
            ESP_LOGI(TAG_STA, "Station started with empty SSID; waiting for runtime config");
        }
  #endif

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
  #if CONFIG_NETWORK_WIFI_STA
        ip_event_got_ip_t *event = event_data;
        ESP_LOGI(TAG_STA, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
      #if CONFIG_NETWORK_WIFI_AP
        if (s_esp_netif_ap && s_esp_netif_sta) {
            esp_err_t err = softap_set_dns_addr(s_esp_netif_ap, s_esp_netif_sta);
            if (err != ESP_OK) {
                ESP_LOGW(TAG_AP, "Failed to update SoftAP DNS from STA uplink: %s", esp_err_to_name(err));
            }
        }
        esp_err_t err = sync_ap_runtime_with_sta_policy(false);
        if (err != ESP_OK) {
            ESP_LOGW(TAG_AP, "Failed to sync SoftAP state after STA connect: %s", esp_err_to_name(err));
        }
      #endif
  #endif

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
  #if CONFIG_NETWORK_WIFI_STA
        wifi_event_sta_disconnected_t *event = event_data;
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

      #if CONFIG_NETWORK_WIFI_AP
        esp_err_t policy_err = sync_ap_runtime_with_sta_policy(false);
        if (policy_err != ESP_OK) {
            ESP_LOGW(TAG_AP, "Failed to sync SoftAP state after STA disconnect: %s", esp_err_to_name(policy_err));
        }
      #endif

        if (s_runtime_config.sta_ssid[0] == '\0') {
            ESP_LOGI(TAG_STA, "STA disconnected and SSID is empty; not retrying");
            return;
        }

        if (s_retry_num < ESP_MAXIMUM_RETRY) {
            ESP_LOGW(TAG_STA, "Disconnected (reason %d). Retrying %d/%d",
                     event->reason, s_retry_num + 1, ESP_MAXIMUM_RETRY);
        } else if (s_retry_num == ESP_MAXIMUM_RETRY) {
            ESP_LOGE(TAG_STA, "Failed to connect after %d retries; continuing background retries", s_retry_num);
        } else if ((s_retry_num % 20) == 0) {
            ESP_LOGW(TAG_STA, "Still retrying STA in background (%d attempts)", s_retry_num);
        }
        s_retry_num++;

        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG_STA, "esp_wifi_connect retry failed: %s", esp_err_to_name(err));
        }
  #endif
    }
}

#if CONFIG_NETWORK_WIFI_AP
///////////////////////////////////////////////////////////////////////////////////////////////////
// Interface Bring-Up Helpers
///////////////////////////////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------
// SoftAP
//-------------------------------------------------------------------------

static esp_err_t configure_softap_ipv4(esp_netif_t *esp_netif_ap)
{
    if (!esp_netif_ap) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_netif_ip_info_t ip_info = {0};
    if (esp_netif_str_to_ip4(SOFTAP_IPV4_ADDR, &ip_info.ip) != ESP_OK ||
        esp_netif_str_to_ip4(SOFTAP_IPV4_ADDR, &ip_info.gw) != ESP_OK ||
        esp_netif_str_to_ip4(SOFTAP_IPV4_NETMASK, &ip_info.netmask) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = esp_netif_dhcps_stop(esp_netif_ap);
    if (err != ESP_OK) {
        ESP_LOGW(TAG_AP, "SoftAP DHCP stop before IP update failed: %s", esp_err_to_name(err));
    }

    err = esp_netif_set_ip_info(esp_netif_ap, &ip_info);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_netif_dhcps_start(esp_netif_ap);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG_AP, "SoftAP IPv4 configured to %s", SOFTAP_IPV4_ADDR);
    return ESP_OK;
}

static esp_err_t wifi_init_softap(esp_netif_t **out_esp_netif_ap)
{
    if (!out_esp_netif_ap) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_netif_t *esp_netif_ap = esp_netif_create_default_wifi_ap();
    if (!esp_netif_ap) {
        return ESP_ERR_NO_MEM;
    }

    if (!is_valid_ssid(s_runtime_config.ap_ssid) || !is_valid_password_len(s_runtime_config.ap_password)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = configure_softap_ipv4(esp_netif_ap);
    if (err != ESP_OK) {
        return err;
    }

    err = apply_ap_config(s_runtime_config.ap_ssid, s_runtime_config.ap_password);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG_AP, "SoftAP ready. SSID:%s auth:%s CH:%d",
             s_runtime_config.ap_ssid,
             s_runtime_config.ap_password[0] ? "wpa2-psk" : "open",
             ESP_WIFI_CHANNEL);

    *out_esp_netif_ap = esp_netif_ap;
    return ESP_OK;
}
#endif  // CONFIG_NETWORK_WIFI_AP

#if CONFIG_NETWORK_WIFI_STA
//-------------------------------------------------------------------------
// Station
//-------------------------------------------------------------------------

static esp_err_t wifi_init_sta(esp_netif_t **out_esp_netif_sta)
{
    if (!out_esp_netif_sta) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_netif_t *esp_netif_sta = esp_netif_create_default_wifi_sta();
    if (!esp_netif_sta) {
        return ESP_ERR_NO_MEM;
    }

    if (strlen(s_runtime_config.sta_ssid) > NETWORK_WIFI_SSID_MAX_LEN ||
        !is_valid_password_len(s_runtime_config.sta_password)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = apply_sta_config(s_runtime_config.sta_ssid, s_runtime_config.sta_password);
    if (err != ESP_OK) {
        return err;
    }

    if (s_runtime_config.sta_ssid[0]) {
        ESP_LOGI(TAG_STA, "Station config done. SSID:%s", s_runtime_config.sta_ssid);
    } else {
        ESP_LOGI(TAG_STA, "Station config done without SSID; auto-connect disabled");
    }

    *out_esp_netif_sta = esp_netif_sta;
    return ESP_OK;
}
#endif  // CONFIG_NETWORK_WIFI_STA

#if CONFIG_NETWORK_ETH_OPENETH
//-------------------------------------------------------------------------
// Ethernet (Used for QEMU)
//-------------------------------------------------------------------------

static void ethernet_release_resources(esp_netif_t *esp_netif_eth)
{
    if (s_eth_handle) {
        (void)esp_eth_stop(s_eth_handle);
    }
    if (s_eth_glue) {
        (void)esp_eth_del_netif_glue(s_eth_glue);
        s_eth_glue = NULL;
    }
    if (s_eth_handle) {
        (void)esp_eth_driver_uninstall(s_eth_handle);
        s_eth_handle = NULL;
    }
    if (s_eth_phy) {
        (void)s_eth_phy->del(s_eth_phy);
        s_eth_phy = NULL;
    }
    if (s_eth_mac) {
        (void)s_eth_mac->del(s_eth_mac);
        s_eth_mac = NULL;
    }
    if (esp_netif_eth) {
        esp_netif_destroy(esp_netif_eth);
    }
}

static esp_err_t ethernet_init_openeth(esp_netif_t **out_esp_netif_eth)
{
    if (!out_esp_netif_eth) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *esp_netif_eth = esp_netif_new(&netif_config);
    if (!esp_netif_eth) {
        return ESP_ERR_NO_MEM;
    }

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.autonego_timeout_ms = 100;

    s_eth_mac = esp_eth_mac_new_openeth(&mac_config);
    if (!s_eth_mac) {
        ethernet_release_resources(esp_netif_eth);
        return ESP_ERR_NO_MEM;
    }

    s_eth_phy = esp_eth_phy_new_dp83848(&phy_config);
    if (!s_eth_phy) {
        ethernet_release_resources(esp_netif_eth);
        return ESP_ERR_NO_MEM;
    }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(s_eth_mac, s_eth_phy);
    esp_err_t err = esp_eth_driver_install(&eth_config, &s_eth_handle);
    if (err != ESP_OK) {
        ethernet_release_resources(esp_netif_eth);
        return err;
    }

    s_eth_glue = esp_eth_new_netif_glue(s_eth_handle);
    if (!s_eth_glue) {
        ethernet_release_resources(esp_netif_eth);
        return ESP_ERR_NO_MEM;
    }

    err = esp_netif_attach(esp_netif_eth, s_eth_glue);
    if (err != ESP_OK) {
        ethernet_release_resources(esp_netif_eth);
        return err;
    }

    err = esp_eth_start(s_eth_handle);
    if (err != ESP_OK) {
        ethernet_release_resources(esp_netif_eth);
        return err;
    }

    ESP_LOGI(TAG_ETH, "OpenCores Ethernet started");
    *out_esp_netif_eth = esp_netif_eth;
    return ESP_OK;
}
#endif

#if CONFIG_NETWORK_WIFI_AP && CONFIG_NETWORK_WIFI_STA
//-------------------------------------------------------------------------
// AP/STA Integration
//-------------------------------------------------------------------------

static esp_err_t softap_set_dns_addr(esp_netif_t *esp_netif_ap, esp_netif_t *esp_netif_sta)
{
    if (!esp_netif_ap || !esp_netif_sta) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_netif_dns_info_t dns;
    esp_err_t err = esp_netif_get_dns_info(esp_netif_sta, ESP_NETIF_DNS_MAIN, &dns);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t opt = DHCPS_OFFER_DNS;
    (void)esp_netif_dhcps_stop(esp_netif_ap);

    err = esp_netif_dhcps_option(esp_netif_ap,
                                 ESP_NETIF_OP_SET,
                                 ESP_NETIF_DOMAIN_NAME_SERVER,
                                 &opt, sizeof(opt));
    if (err != ESP_OK) {
        return err;
    }

    err = esp_netif_set_dns_info(esp_netif_ap, ESP_NETIF_DNS_MAIN, &dns);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_netif_dhcps_start(esp_netif_ap);
    if (err != ESP_OK) {
        return err;
    }

    return ESP_OK;
}
#endif

//-------------------------------------------------------------------------
// Lifecycle
//-------------------------------------------------------------------------

esp_err_t init_wifi(void)
{
    esp_err_t err;

    err = ensure_runtime_config_loaded();
    if (err != ESP_OK) {
        return err;
    }

#if CONFIG_NETWORK_WIFI_AP && CONFIG_NETWORK_WIFI_STA
    s_ap_shutdown_on_sta_connected_active = s_runtime_config.disable_ap_when_sta_connected;
    s_ap_runtime_disabled_for_sta = false;
    s_ap_recovery_mode = false;
#endif

#if !CONFIG_NETWORK_WIFI_AP && !CONFIG_NETWORK_WIFI_STA && !CONFIG_NETWORK_ETH_OPENETH
    ESP_LOGW(TAG_STA, "No network interfaces enabled (Wi-Fi AP/STA + OpenETH disabled)");
    return ESP_OK;
#endif

#if CONFIG_NETWORK_WIFI_STA
    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) {
        return ESP_ERR_NO_MEM;
    }
#endif

#if CONFIG_NETWORK_WIFI_AP || CONFIG_NETWORK_WIFI_STA
    err = esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID,
        &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }
#endif

#if CONFIG_NETWORK_WIFI_STA
    err = esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP,
        &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }
#endif

#if CONFIG_NETWORK_ETH_OPENETH
    err = esp_event_handler_instance_register(
        ETH_EVENT, ESP_EVENT_ANY_ID,
        &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_ETH_GOT_IP,
        &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }
#endif

#if CONFIG_NETWORK_WIFI_AP || CONFIG_NETWORK_WIFI_STA
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_cfg);
    if (err != ESP_OK) {
        return err;
    }

    /* Select mode based on Kconfig */
  #if CONFIG_NETWORK_WIFI_AP && CONFIG_NETWORK_WIFI_STA
    ESP_LOGI(TAG_STA, "Starting AP+STA mode");
    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        return err;
    }
  #elif CONFIG_NETWORK_WIFI_AP
    ESP_LOGI(TAG_AP, "Starting AP mode");
    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        return err;
    }
  #elif CONFIG_NETWORK_WIFI_STA
    ESP_LOGI(TAG_STA, "Starting STA mode");
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        return err;
    }
  #else
    return ESP_OK;
  #endif

    /* Bring up interfaces */
  #if CONFIG_NETWORK_WIFI_AP
    esp_netif_t *esp_netif_ap = NULL;
    err = wifi_init_softap(&esp_netif_ap);
    if (err != ESP_OK) {
        return err;
    }
    s_esp_netif_ap = esp_netif_ap;
  #endif

  #if CONFIG_NETWORK_WIFI_STA
    esp_netif_t *esp_netif_sta = NULL;
    err = wifi_init_sta(&esp_netif_sta);
    if (err != ESP_OK) {
        return err;
    }
    s_esp_netif_sta = esp_netif_sta;
  #endif

    err = esp_wifi_start();
    if (err != ESP_OK) {
        return err;
    }

    /* If STA is configured, keep trying in the background */
  #if CONFIG_NETWORK_WIFI_STA
    if (s_runtime_config.sta_ssid[0] != '\0') {
        ESP_LOGI(TAG_STA, "Initial STA connect scheduled in background. SSID=%s", s_runtime_config.sta_ssid);
        err = esp_netif_set_default_netif(s_esp_netif_sta);
        if (err != ESP_OK) {
            return err;
        }
    } else {
        ESP_LOGW(TAG_STA, "STA SSID is empty; skipping initial STA connection");
    }
  #endif  // CONFIG_NETWORK_WIFI_STA

    /* Enable NAPT only when it is compiled into lwIP */
  #if CONFIG_NETWORK_WIFI_AP
    #if IP_NAPT
    err = esp_netif_napt_enable(esp_netif_ap);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_AP, "Failed to enable NAPT");
        return err;
    }
    #else
    ESP_LOGW(TAG_AP,
             "NAPT disabled in build (CONFIG_LWIP_IPV4_NAPT=n). "
             "SoftAP clients will not have routed internet access.");
    #endif
  #endif
#endif

#if CONFIG_NETWORK_ETH_OPENETH
    esp_netif_t *esp_netif_eth = NULL;
    err = ethernet_init_openeth(&esp_netif_eth);
    if (err != ESP_OK) {
        return err;
    }
    s_esp_netif_eth = esp_netif_eth;

    bool set_eth_default = true;
#if CONFIG_NETWORK_WIFI_STA
    set_eth_default = (s_runtime_config.sta_ssid[0] == '\0');
#endif
    if (set_eth_default) {
        err = esp_netif_set_default_netif(s_esp_netif_eth);
        if (err != ESP_OK) {
            return err;
        }
    }
#endif

    return ESP_OK;
}
