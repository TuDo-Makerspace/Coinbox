#include "bootstrap.h"

#include <stdbool.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_random.h"
#include "esp_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "audio.h"
#include "sdkconfig.h"
#include "device_info.h"
#include "mainapp.h"
#include "network.h"
#include "files.h"
#include "ota.h"
#include "gpio.h"
#include "recovery_code.h"
#include "runtime_status.h"

///////////////////////////////////////////////////////////////////////////////////////////////////
// Constants
///////////////////////////////////////////////////////////////////////////////////////////////////

static const char *TAG = "bootstrap";
#define RECOVERY_AUTH_TOKEN_NUM_BYTES 16
#define RECOVERY_AUTH_TOKEN_HEX_LEN (RECOVERY_AUTH_TOKEN_NUM_BYTES * 2)

///////////////////////////////////////////////////////////////////////////////////////////////////
// Vars
///////////////////////////////////////////////////////////////////////////////////////////////////

static httpd_handle_t s_bootstrap_server;
static TickType_t s_recovery_deadline_ticks;
static bool s_recovery_window_active;
static bool s_recovery_requested;
static bool s_main_started;
static TaskHandle_t s_start_task;
static bool s_main_start_pending;
static bool s_laser_countdown_boost_applied;
static bool s_recovery_auth_required;
static char s_recovery_auth_token[RECOVERY_AUTH_TOKEN_HEX_LEN + 1];

static const uint32_t BOOTSTRAP_LASER_BREAKS_FOR_EXTENSION = 3;
static const uint32_t BOOTSTRAP_LASER_EXTENDED_SECONDS = 60;
static const char *BOOTSTRAP_LASER_EXTEND_NOTE_DEFAULT =
    "Interrupt the laser beam three times to extend the countdown to 60 seconds.";
static const char *BOOTSTRAP_LASER_EXTEND_NOTE_EXTENDED =
    "Laser beam interrupted three times. Countdown extended.";

///////////////////////////////////////////////////////////////////////////////////////////////////
// Forward Declarations
///////////////////////////////////////////////////////////////////////////////////////////////////

static void cancel_recovery_timer(void);
static void cancel_start_task(void);
static uint32_t get_remaining_seconds(void);
static void bootstrap_process_laser_beam_breaks(void);
static void schedule_main_start(void);
static void enter_main_task(void *arg);
static void start_main_application(void);
static esp_err_t render_bootstrap_page(httpd_req_t *req);
static bool replace_placeholder(char *buffer, size_t buffer_size, const char *placeholder, const char *replacement);
static bool replace_placeholder_any(char *buffer, size_t buffer_size,
                                    const char *placeholder_a, const char *placeholder_b,
                                    const char *replacement);
static bool json_get_int(const char *json, const char *key, int *out);
static bool json_get_string(const char *json, const char *key, char *out, size_t out_size);
static bool json_has_key(const char *json, const char *key);
static esp_err_t bootstrap_read_request_body(httpd_req_t *req, char *body, size_t body_size);
static esp_err_t bootstrap_send_unauthorized(httpd_req_t *req);
static void bootstrap_generate_recovery_auth_token(void);
static bool bootstrap_is_recovery_authenticated_request(httpd_req_t *req);
static esp_err_t bootstrap_require_recovery_auth(httpd_req_t *req);
static esp_err_t bootstrap_require_recovery_access(httpd_req_t *req, const char *conflict_message);
static esp_err_t send_device_info_json(httpd_req_t *req);
static esp_err_t bootstrap_runtime_status_handler(httpd_req_t *req);

///////////////////////////////////////////////////////////////////////////////////////////////////
// JSON and Auth Helpers
///////////////////////////////////////////////////////////////////////////////////////////////////

static bool json_get_int(const char *json, const char *key, int *out)
{
    if (!json || !key || !out) {
        return false;
    }
    const char *p = strstr(json, key);
    if (!p) {
        return false;
    }
    p = strchr(p, ':');
    if (!p) {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }
    return sscanf(p, "%d", out) == 1;
}

static bool json_get_string(const char *json, const char *key, char *out, size_t out_size)
{
    if (!json || !key || !out || out_size == 0) {
        return false;
    }

    out[0] = '\0';
    const char *p = strstr(json, key);
    if (!p) {
        return false;
    }
    p = strchr(p, ':');
    if (!p) {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }
    if (*p != '"') {
        return false;
    }
    p++;

    size_t w = 0;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p) {
            char esc = *p++;
            switch (esc) {
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                default: c = esc; break;
            }
        }

        if (w + 1 >= out_size) {
            return false;
        }
        out[w++] = c;
    }
    if (*p != '"') {
        return false;
    }

    out[w] = '\0';
    return true;
}

static bool json_has_key(const char *json, const char *key)
{
    return json && key && strstr(json, key) != NULL;
}

static esp_err_t bootstrap_read_request_body(httpd_req_t *req, char *body, size_t body_size)
{
    if (!req || !body || body_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (req->content_len <= 0 || req->content_len >= (int)body_size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request body");
        return ESP_FAIL;
    }

    int received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, body + received, req->content_len - received);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read body");
            return ESP_FAIL;
        }
        received += r;
    }
    body[req->content_len] = '\0';
    return ESP_OK;
}

static esp_err_t bootstrap_send_unauthorized(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, "Unauthorized");
    return ESP_FAIL;
}

static void bootstrap_bytes_to_hex(const uint8_t *src, size_t src_len, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789abcdef";

    if (!src || !dst || dst_size < (src_len * 2 + 1)) {
        if (dst && dst_size > 0) {
            dst[0] = '\0';
        }
        return;
    }

    for (size_t i = 0; i < src_len; ++i) {
        dst[i * 2] = hex[(src[i] >> 4) & 0x0F];
        dst[i * 2 + 1] = hex[src[i] & 0x0F];
    }
    dst[src_len * 2] = '\0';
}

static void bootstrap_generate_recovery_auth_token(void)
{
    uint8_t token[RECOVERY_AUTH_TOKEN_NUM_BYTES];

    for (size_t i = 0; i < sizeof(token); i += sizeof(uint32_t)) {
        uint32_t value = esp_random();
        size_t remaining = sizeof(token) - i;
        size_t copy_len = remaining < sizeof(uint32_t) ? remaining : sizeof(uint32_t);
        memcpy(&token[i], &value, copy_len);
    }

    bootstrap_bytes_to_hex(token, sizeof(token), s_recovery_auth_token, sizeof(s_recovery_auth_token));
}

static bool bootstrap_is_recovery_authenticated_request(httpd_req_t *req)
{
    s_recovery_auth_required = mainapp_security_is_password_set();
    if (!s_recovery_auth_required) {
        return true;
    }
    if (!req || s_recovery_auth_token[0] == '\0') {
        return false;
    }

    size_t header_len = httpd_req_get_hdr_value_len(req, "X-Recovery-Auth");
    if (header_len == 0 || header_len >= sizeof(s_recovery_auth_token)) {
        return false;
    }

    char header_value[sizeof(s_recovery_auth_token)] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-Recovery-Auth", header_value, sizeof(header_value)) != ESP_OK) {
        return false;
    }

    return strcmp(header_value, s_recovery_auth_token) == 0;
}

static esp_err_t bootstrap_require_recovery_auth(httpd_req_t *req)
{
    if (bootstrap_is_recovery_authenticated_request(req)) {
        return ESP_OK;
    }
    return bootstrap_send_unauthorized(req);
}

static esp_err_t bootstrap_require_recovery_access(httpd_req_t *req, const char *conflict_message)
{
    if (!s_recovery_requested) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, conflict_message);
        return ESP_FAIL;
    }

    return bootstrap_require_recovery_auth(req);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Handlers
///////////////////////////////////////////////////////////////////////////////////////////////////

static esp_err_t bootstrap_404_redirect_handler(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "307 Temporary Redirect");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t bootstrap_root_handler(httpd_req_t *req)
{
    return render_bootstrap_page(req);
}

static esp_err_t bootstrap_runtime_status_handler(httpd_req_t *req)
{
    return runtime_status_send_json(req, "bootstrap");
}

static esp_err_t render_bootstrap_page(httpd_req_t *req)
{
    // Get remaining seconds on recovery timer.
    uint32_t seconds = get_remaining_seconds();

    // Load the bootstrap HTML template.
    extern const unsigned char bootstrap_html_start[] asm("_binary_bootstrap_html_start");
    extern const unsigned char bootstrap_html_end[] asm("_binary_bootstrap_html_end");
    const size_t template_len = (size_t)(bootstrap_html_end - bootstrap_html_start);

    const size_t render_headroom = 512;
    if (template_len >= (SIZE_MAX - (render_headroom + 1))) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Template size overflow");
        return ESP_ERR_NO_MEM;
    }
    const size_t page_capacity = template_len + render_headroom + 1;

    // Allocate buffer for the bootstrap HTML template.
    char *page = malloc(page_capacity);
    if (!page) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    memcpy(page, bootstrap_html_start, template_len);
    page[template_len] = '\0';

    // Replace placeholders in the template.
    char seconds_buf[16];
    char refresh_buf[2];
    char recovery_buf[2];
    char laser_extended_buf[2];
    bool auto_refresh = !s_recovery_requested;
    bool is_recovery = s_recovery_requested;
    bool laser_extended = s_laser_countdown_boost_applied;
    const char *extend_note = laser_extended ? BOOTSTRAP_LASER_EXTEND_NOTE_EXTENDED : BOOTSTRAP_LASER_EXTEND_NOTE_DEFAULT;

    snprintf(seconds_buf, sizeof(seconds_buf), "%lu", (unsigned long)seconds);
    refresh_buf[0] = auto_refresh ? '1' : '0';
    refresh_buf[1] = '\0';
    recovery_buf[0] = is_recovery ? '1' : '0';
    recovery_buf[1] = '\0';
    laser_extended_buf[0] = laser_extended ? '1' : '0';
    laser_extended_buf[1] = '\0';

    if (!replace_placeholder(page, page_capacity, "{{BOOT_ID}}", runtime_status_boot_id()) ||
        !replace_placeholder_any(page, page_capacity, "{{SECONDS}}", "{{ SECONDS }}", seconds_buf) ||
        !replace_placeholder_any(page, page_capacity, "{{AUTO_REFRESH}}", "{{ AUTO_REFRESH }}", refresh_buf) ||
        !replace_placeholder_any(page, page_capacity, "{{IS_RECOVERY}}", "{{ IS_RECOVERY }}", recovery_buf) ||
        !replace_placeholder_any(page, page_capacity, "{{LASER_EXTENSION_SEEN}}", "{{ LASER_EXTENSION_SEEN }}", laser_extended_buf) ||
        !replace_placeholder_any(page, page_capacity, "{{EXTEND_NOTE}}", "{{ EXTEND_NOTE }}", extend_note)) {
        ESP_LOGE(TAG, "Failed to render bootstrap page");
        free(page);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Render failed");
        return ESP_FAIL;
    }

    // Send the rendered page.
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
    free(page);
    return ESP_OK;
}

static esp_err_t bootstrap_recovery_handler(httpd_req_t *req)
{
    s_recovery_requested = true;
    cancel_recovery_timer();

    ESP_LOGW(TAG, "Recovery endpoint hit; countdown aborted");

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Recovery mode engaged. Bootstrap server will stay active.");
    return ESP_OK;
}

static esp_err_t bootstrap_skip_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Skip requested; starting main application immediately");
    cancel_recovery_timer();

    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Starting main application");

    // Start handoff only after finishing the /skip response to reduce socket-close races.
    schedule_main_start();
    return ESP_OK;
}

static esp_err_t bootstrap_format_storage_handler(httpd_req_t *req)
{
    esp_err_t access_err = bootstrap_require_recovery_access(
        req,
        "Enter recovery mode before formatting storage.");
    if (access_err != ESP_OK) {
        return access_err;
    }

    ESP_LOGW(TAG, "Formatting storage from recovery mode");
    if (files_format_storage() != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Format failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Storage formatted");
    return ESP_OK;
}

static esp_err_t bootstrap_reset_settings_handler(httpd_req_t *req)
{
    esp_err_t access_err = bootstrap_require_recovery_access(
        req,
        "Enter recovery mode before resetting settings.");
    if (access_err != ESP_OK) {
        return access_err;
    }

    ESP_LOGW(TAG, "Resetting configured settings to defaults from recovery mode");

    // Persist defaults now, but apply network runtime changes on main-app handoff.
    esp_err_t err = network_reset_config_to_defaults(false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reset network settings: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to reset network settings");
        return ESP_FAIL;
    }

    err = network_enable_recovery_ap();
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGE(TAG, "Failed to keep recovery AP active after reset: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to keep recovery AP active");
        return ESP_FAIL;
    }

    err = mainapp_reset_security_defaults();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reset security settings: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to reset security settings");
        return ESP_FAIL;
    }

    err = mainapp_reset_boot_defaults();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reset start-up settings: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to reset start-up settings");
        return ESP_FAIL;
    }

    s_recovery_auth_required = mainapp_security_is_password_set();
    s_recovery_auth_token[0] = '\0';
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Settings reset to defaults");
    return ESP_OK;
}

static void json_escape_copy(const char *src, char *dst, size_t dst_size)
{
    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }

    size_t w = 0;
    for (size_t i = 0; src[i] != '\0' && w + 1 < dst_size; ++i) {
        char c = src[i];
        if (c == '"' || c == '\\') {
            if (w + 2 >= dst_size) {
                break;
            }
            dst[w++] = '\\';
            dst[w++] = c;
        } else if ((unsigned char)c < 0x20) {
            dst[w++] = ' ';
        } else {
            dst[w++] = c;
        }
    }
    dst[w] = '\0';
}

static esp_err_t send_device_info_json(httpd_req_t *req)
{
    s_recovery_auth_required = mainapp_security_is_password_set();

    char mac[18] = {0};
    if (device_info_get_mac_string(mac, sizeof(mac)) != ESP_OK) {
        strlcpy(mac, "unknown", sizeof(mac));
    }

    char resp[320];
    int len = snprintf(
        resp,
        sizeof(resp),
        "{\"vendor\":\"%s\",\"firmware_version\":\"%s\",\"hardware_version\":\"%s\","
        "\"source_code\":\"%s\",\"license\":\"%s\",\"mac\":\"%s\",\"auth_required\":%s}",
        device_info_vendor(),
        device_info_firmware_version(),
        device_info_hardware_version(),
        device_info_source_code_url(),
        device_info_license_name(),
        mac,
        s_recovery_auth_required ? "true" : "false");
    if (len < 0 || len >= (int)sizeof(resp)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Render failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

static esp_err_t bootstrap_recovery_auth_handler(httpd_req_t *req)
{
    if (!s_recovery_requested) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Enter recovery mode before authenticating.");
        return ESP_OK;
    }

    s_recovery_auth_required = mainapp_security_is_password_set();
    if (!s_recovery_auth_required) {
        bootstrap_generate_recovery_auth_token();
    } else {
        char body[256];
        if (bootstrap_read_request_body(req, body, sizeof(body)) != ESP_OK) {
            return ESP_FAIL;
        }

        bool password_present = json_has_key(body, "password");
        bool recovery_code_present = json_has_key(body, "recovery_code");
        if (password_present == recovery_code_present) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Provide password or recovery_code");
            return ESP_FAIL;
        }

        bool authenticated = false;
        if (password_present) {
            char password[65];
            if (!json_get_string(body, "password", password, sizeof(password))) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid password");
                return ESP_FAIL;
            }
            authenticated = mainapp_security_password_matches(password);
        } else {
            int recovery_code = 0;
            if (!json_get_int(body, "recovery_code", &recovery_code)) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid recovery code");
                return ESP_FAIL;
            }
            authenticated = recovery_code_check(recovery_code);
        }

        if (!authenticated) {
            s_recovery_auth_token[0] = '\0';
            return bootstrap_send_unauthorized(req);
        }

        bootstrap_generate_recovery_auth_token();
    }

    char resp[96];
    int len = snprintf(resp,
                       sizeof(resp),
                       "{\"ok\":true,\"token\":\"%s\"}",
                       s_recovery_auth_token);
    if (len < 0 || len >= (int)sizeof(resp)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Render failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

static esp_err_t bootstrap_device_info_handler(httpd_req_t *req)
{
    return send_device_info_json(req);
}

static esp_err_t bootstrap_network_ips_handler(httpd_req_t *req)
{
    char ap_ip[16] = {0};
    char sta_ip[16] = {0};
    char sta_ssid[33] = {0};
    char ap_ip_esc[32] = {0};
    char sta_ip_esc[32] = {0};
    char sta_ssid_esc[80] = {0};
    (void)network_get_ipv4_strings(ap_ip, sizeof(ap_ip), sta_ip, sizeof(sta_ip));
    (void)network_get_connected_sta_ssid(sta_ssid, sizeof(sta_ssid));
    json_escape_copy(ap_ip, ap_ip_esc, sizeof(ap_ip_esc));
    json_escape_copy(sta_ip, sta_ip_esc, sizeof(sta_ip_esc));
    json_escape_copy(sta_ssid, sta_ssid_esc, sizeof(sta_ssid_esc));

    char resp[256];
    int len = snprintf(resp, sizeof(resp),
                       "{\"ap\":\"%s\",\"sta\":\"%s\",\"sta_ssid\":\"%s\"}",
                       ap_ip_esc,
                       sta_ip_esc,
                       sta_ssid_esc);
    if (len < 0 || len >= (int)sizeof(resp)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Render failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

static esp_err_t bootstrap_glyphs_js_handler(httpd_req_t *req)
{
    extern const unsigned char glyphs_js_start[] asm("_binary_glyphs_js_start");
    extern const unsigned char glyphs_js_end[] asm("_binary_glyphs_js_end");
    const size_t glyphs_js_size = (size_t)(glyphs_js_end - glyphs_js_start);

    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, (const char *)glyphs_js_start, glyphs_js_size);
    return ESP_OK;
}

static esp_err_t bootstrap_connection_monitor_js_handler(httpd_req_t *req)
{
    extern const unsigned char connection_monitor_js_start[] asm("_binary_connection_monitor_js_start");
    extern const unsigned char connection_monitor_js_end[] asm("_binary_connection_monitor_js_end");
    const size_t connection_monitor_js_size = (size_t)(connection_monitor_js_end - connection_monitor_js_start);

    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, (const char *)connection_monitor_js_start, connection_monitor_js_size);
    return ESP_OK;
}

static esp_err_t bootstrap_glyphs_css_handler(httpd_req_t *req)
{
    extern const unsigned char glyphs_css_start[] asm("_binary_glyphs_css_start");
    extern const unsigned char glyphs_css_end[] asm("_binary_glyphs_css_end");
    const size_t glyphs_css_size = (size_t)(glyphs_css_end - glyphs_css_start);

    httpd_resp_set_type(req, "text/css");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, (const char *)glyphs_css_start, glyphs_css_size);
    return ESP_OK;
}

#if CONFIG_TEST_GPIO_INJECTION
static esp_err_t bootstrap_test_gpio_send_level_json(httpd_req_t *req, const char *name, int level)
{
    char json[64];
    int len = snprintf(json, sizeof(json), "{\"name\":\"%s\",\"level\":%d}", name ? name : "", level ? 1 : 0);
    if (len < 0 || len >= (int)sizeof(json)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Render failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, json, len);
    return ESP_OK;
}

static esp_err_t bootstrap_test_gpio_parse_level_query(httpd_req_t *req, int *out_level)
{
    if (!req || !out_level) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request");
        return ESP_FAIL;
    }

    int query_len = httpd_req_get_url_query_len(req);
    if (query_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing level");
        return ESP_FAIL;
    }

    char query[48];
    if (query_len >= (int)sizeof(query)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Query too long");
        return ESP_FAIL;
    }
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad query");
        return ESP_FAIL;
    }

    char level_str[8] = {0};
    if (httpd_query_key_value(query, "level", level_str, sizeof(level_str)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing level");
        return ESP_FAIL;
    }

    if (strcmp(level_str, "0") == 0) {
        *out_level = 0;
        return ESP_OK;
    }
    if (strcmp(level_str, "1") == 0) {
        *out_level = 1;
        return ESP_OK;
    }

    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid level");
    return ESP_FAIL;
}

static esp_err_t bootstrap_test_gpio_laser_get_handler(httpd_req_t *req)
{
    return bootstrap_test_gpio_send_level_json(req, "laser", gpio_get_laser_level());
}

static esp_err_t bootstrap_test_gpio_laser_post_handler(httpd_req_t *req)
{
    int level = 0;
    if (bootstrap_test_gpio_parse_level_query(req, &level) != ESP_OK) {
        return ESP_FAIL;
    }

    esp_err_t err = gpio_test_set_laser_level(level);
    if (err != ESP_OK) {
        if (err == ESP_ERR_INVALID_ARG) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid level");
        } else {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "GPIO update failed");
        }
        return ESP_FAIL;
    }

    // Process immediately so test requests can observe countdown effects without waiting for polling tick.
    bootstrap_process_laser_beam_breaks();
    return bootstrap_test_gpio_send_level_json(req, "laser", gpio_get_laser_level());
}
#endif

///////////////////////////////////////////////////////////////////////////////////////////////////
// Recovery Countdown
///////////////////////////////////////////////////////////////////////////////////////////////////

static void cancel_recovery_timer(void)
{
    s_recovery_window_active = false;
}

static void cancel_start_task(void)
{
    if (!s_start_task) {
        return;
    }
    vTaskDelete(s_start_task);
    s_start_task = NULL;
    s_main_start_pending = false;
}

static uint32_t get_remaining_seconds(void)
{
    if (!s_recovery_window_active) {
        return 0;
    }

    TickType_t now = xTaskGetTickCount();
    if (s_recovery_deadline_ticks <= now) {
        return 0;
    }
    TickType_t ticks_remaining = s_recovery_deadline_ticks - now;
    return (ticks_remaining + configTICK_RATE_HZ - 1U) / configTICK_RATE_HZ;
}

static void bootstrap_process_laser_beam_breaks(void)
{
    if (!s_recovery_window_active || s_recovery_requested || s_main_started || s_laser_countdown_boost_applied) {
        return;
    }

    uint32_t breaks = gpio_get_laser_breaks();
    if (breaks < BOOTSTRAP_LASER_BREAKS_FOR_EXTENSION) {
        return;
    }

    TickType_t now = xTaskGetTickCount();
    TickType_t extended_deadline = now + pdMS_TO_TICKS(BOOTSTRAP_LASER_EXTENDED_SECONDS * 1000U);
    if (extended_deadline > s_recovery_deadline_ticks) {
        s_recovery_deadline_ticks = extended_deadline;
        ESP_LOGW(TAG,
                 "Bootstrap countdown extended to %us after %u beam breaks",
                 (unsigned)BOOTSTRAP_LASER_EXTENDED_SECONDS,
                 (unsigned)breaks);
    }
    s_laser_countdown_boost_applied = true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Transition to main application
///////////////////////////////////////////////////////////////////////////////////////////////////

static void schedule_main_start(void)
{
    if (s_main_started || s_main_start_pending) {
        return;
    }

    // Bootstrap creates this task once. It runs the handoff outside timer-service context.
    if (!s_start_task) {
        ESP_LOGE(TAG, "Main start task not available");
        return;
    }

    s_main_start_pending = true;
    xTaskNotifyGive(s_start_task);
}

static void enter_main_task(void *arg)
{
    (void)arg;

    for (;;) {
        bootstrap_process_laser_beam_breaks();

        TickType_t wait_ticks = portMAX_DELAY;
        if (s_recovery_window_active) {
            TickType_t now = xTaskGetTickCount();
            wait_ticks = (s_recovery_deadline_ticks > now) ? (s_recovery_deadline_ticks - now) : 0;
            TickType_t poll_ticks = pdMS_TO_TICKS(200);
            if (wait_ticks > poll_ticks) {
                wait_ticks = poll_ticks;
            }
        }
        uint32_t notify_count = ulTaskNotifyTake(pdTRUE, wait_ticks);

        bootstrap_process_laser_beam_breaks();

        if (s_main_started) {
            continue;
        }

        bool should_start_main = false;
        if (notify_count > 0) {
            if (!s_main_start_pending) {
                continue;
            }
            s_main_start_pending = false;
            should_start_main = true;
        } else if (s_recovery_window_active) {
            TickType_t now = xTaskGetTickCount();
            if (s_recovery_deadline_ticks > now) {
                continue;
            }
            s_recovery_window_active = false;
            if (s_recovery_requested) {
                ESP_LOGI(TAG, "Recovery requested; staying in bootstrap mode");
                continue;
            }
            ESP_LOGI(TAG, "Recovery window elapsed; starting main application");
            should_start_main = true;
        }

        if (!should_start_main) {
            continue;
        }

        // Cancel the recovery timer if still running.
        cancel_recovery_timer();

        // Stop the bootstrap server.
        if (s_bootstrap_server) {
            httpd_stop(s_bootstrap_server);
            s_bootstrap_server = NULL;
        }

        // Start the main application.
        start_main_application();

        // Mark this task as no longer scheduled/running.
        s_start_task = NULL;
        vTaskDelete(NULL);
    }
}

static void start_main_application(void)
{
    gpio_set_runtime_mode(GPIO_RUNTIME_MAIN_APP);

    esp_err_t sta_err = network_restore_configured_sta();
    if (sta_err != ESP_OK && sta_err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGE(TAG, "Failed to restore configured STA before main handoff: %s", esp_err_to_name(sta_err));
    }

    esp_err_t ap_err = network_restore_configured_ap();
    if (ap_err != ESP_OK && ap_err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGE(TAG, "Failed to restore configured AP before main handoff: %s", esp_err_to_name(ap_err));
    }

    esp_err_t err = start_mainapp();  // Start WebSocket server
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start main application: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Main application started");
        s_main_started = true;
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Template Rendering
///////////////////////////////////////////////////////////////////////////////////////////////////

static bool replace_placeholder(char *buffer, size_t buffer_size, const char *placeholder, const char *replacement)
{
    const size_t placeholder_len = strlen(placeholder);
    const size_t replacement_len = strlen(replacement);
    if (placeholder_len == 0) {
        return false;
    }

    bool replaced = false;
    size_t current_len = strlen(buffer);
    char *cursor = buffer;

    while ((cursor = strstr(cursor, placeholder)) != NULL) {
        size_t new_len = current_len - placeholder_len + replacement_len;
        if (new_len >= buffer_size) {
            return false;
        }

        size_t tail_len = current_len - (size_t)(cursor - buffer) - placeholder_len + 1;
        memmove(cursor + replacement_len, cursor + placeholder_len, tail_len);
        memcpy(cursor, replacement, replacement_len);

        replaced = true;
        current_len = new_len;
        cursor += replacement_len;
    }

    return replaced;
}

static bool replace_placeholder_any(char *buffer, size_t buffer_size,
                                    const char *placeholder_a, const char *placeholder_b,
                                    const char *replacement)
{
    bool replaced = false;
    if (placeholder_a) {
        replaced |= replace_placeholder(buffer, buffer_size, placeholder_a, replacement);
    }
    if (placeholder_b) {
        replaced |= replace_placeholder(buffer, buffer_size, placeholder_b, replacement);
    }
    return replaced;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Entry 
///////////////////////////////////////////////////////////////////////////////////////////////////

esp_err_t bootstrap(void)
{
    if (s_bootstrap_server) {
        ESP_LOGW(TAG, "Bootstrap server already running");
        return ESP_ERR_INVALID_STATE;
    }

    s_recovery_requested = false;
    s_main_started = false;
    s_start_task = NULL;
    s_main_start_pending = false;
    s_recovery_window_active = true;
    s_recovery_deadline_ticks = xTaskGetTickCount() + pdMS_TO_TICKS(CONFIG_RECOVERY_ENTRY_TIME * 1000);
    s_laser_countdown_boost_applied = false;
    s_recovery_auth_token[0] = '\0';
    gpio_set_runtime_mode(GPIO_RUNTIME_BOOTSTRAP);

    esp_err_t security_err = mainapp_security_init();
    if (security_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load security state for bootstrap: %s", esp_err_to_name(security_err));
        return security_err;
    }
    s_recovery_auth_required = mainapp_security_is_password_set();

    esp_err_t ap_err = network_enable_recovery_ap();
    if (ap_err != ESP_OK && ap_err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "Failed to switch to recovery AP during bootstrap startup: %s", esp_err_to_name(ap_err));
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_uri_handlers = 16;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_bootstrap_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start bootstrap server: %s", esp_err_to_name(err));
        return err;
    }

    if (xTaskCreate(enter_main_task, "start-main", 6144, NULL, tskIDLE_PRIORITY + 4, &s_start_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create main start task");
        httpd_stop(s_bootstrap_server);
        s_bootstrap_server = NULL;
        return ESP_ERR_NO_MEM;
    }

    // Redirect any missing paths to '/' so the UI keeps working during bootstrap.
    httpd_register_err_handler(s_bootstrap_server, HTTPD_404_NOT_FOUND, bootstrap_404_redirect_handler);

    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = bootstrap_root_handler,
        .user_ctx = NULL
    };
    httpd_uri_t recovery = {
        .uri = "/recovery",
        .method = HTTP_GET,
        .handler = bootstrap_recovery_handler,
        .user_ctx = NULL
    };
    httpd_uri_t skip = {
        .uri = "/skip",
        .method = HTTP_GET,
        .handler = bootstrap_skip_handler,
        .user_ctx = NULL
    };
    httpd_uri_t ota_update = {
        .uri = "/update",
        .method = HTTP_POST,
        .handler = ota_update_handler,
        .user_ctx = NULL
    };
    httpd_uri_t format_storage = {
        .uri = "/format",
        .method = HTTP_POST,
        .handler = bootstrap_format_storage_handler,
        .user_ctx = NULL
    };
    httpd_uri_t reset_settings = {
        .uri = "/settings/reset",
        .method = HTTP_POST,
        .handler = bootstrap_reset_settings_handler,
        .user_ctx = NULL
    };
    httpd_uri_t network_ips = {
        .uri = "/network/ips",
        .method = HTTP_GET,
        .handler = bootstrap_network_ips_handler,
        .user_ctx = NULL
    };
    httpd_uri_t recovery_auth = {
        .uri = "/recovery/auth",
        .method = HTTP_POST,
        .handler = bootstrap_recovery_auth_handler,
        .user_ctx = NULL
    };
    httpd_uri_t device_info = {
        .uri = "/device/info",
        .method = HTTP_GET,
        .handler = bootstrap_device_info_handler,
        .user_ctx = NULL
    };
    httpd_uri_t runtime_status = {
        .uri = "/runtime/status",
        .method = HTTP_GET,
        .handler = bootstrap_runtime_status_handler,
        .user_ctx = NULL
    };
    httpd_uri_t glyphs_js = {
        .uri = "/glyphs.js",
        .method = HTTP_GET,
        .handler = bootstrap_glyphs_js_handler,
        .user_ctx = NULL
    };
    httpd_uri_t connection_monitor_js = {
        .uri = "/connection_monitor.js",
        .method = HTTP_GET,
        .handler = bootstrap_connection_monitor_js_handler,
        .user_ctx = NULL
    };
    httpd_uri_t glyphs_css = {
        .uri = "/glyphs.css",
        .method = HTTP_GET,
        .handler = bootstrap_glyphs_css_handler,
        .user_ctx = NULL
    };
#if CONFIG_TEST_GPIO_INJECTION
    httpd_uri_t test_gpio_laser_get = {
        .uri = "/test/gpio/laser",
        .method = HTTP_GET,
        .handler = bootstrap_test_gpio_laser_get_handler,
        .user_ctx = NULL
    };
    httpd_uri_t test_gpio_laser_post = {
        .uri = "/test/gpio/laser",
        .method = HTTP_POST,
        .handler = bootstrap_test_gpio_laser_post_handler,
        .user_ctx = NULL
    };
#endif

    if (httpd_register_uri_handler(s_bootstrap_server, &connection_monitor_js) != ESP_OK ||
        httpd_register_uri_handler(s_bootstrap_server, &glyphs_js) != ESP_OK ||
        httpd_register_uri_handler(s_bootstrap_server, &glyphs_css) != ESP_OK ||
        httpd_register_uri_handler(s_bootstrap_server, &ota_update) != ESP_OK ||
        httpd_register_uri_handler(s_bootstrap_server, &root) != ESP_OK ||
        httpd_register_uri_handler(s_bootstrap_server, &recovery) != ESP_OK ||
        httpd_register_uri_handler(s_bootstrap_server, &skip) != ESP_OK ||
        httpd_register_uri_handler(s_bootstrap_server, &format_storage) != ESP_OK ||
        httpd_register_uri_handler(s_bootstrap_server, &reset_settings) != ESP_OK ||
        httpd_register_uri_handler(s_bootstrap_server, &network_ips) != ESP_OK ||
        httpd_register_uri_handler(s_bootstrap_server, &recovery_auth) != ESP_OK ||
        httpd_register_uri_handler(s_bootstrap_server, &device_info) != ESP_OK ||
        httpd_register_uri_handler(s_bootstrap_server, &runtime_status) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register bootstrap handlers");
        cancel_recovery_timer();
        cancel_start_task();
        httpd_stop(s_bootstrap_server);
        s_bootstrap_server = NULL;
        return ESP_FAIL;
    }

#if CONFIG_TEST_GPIO_INJECTION
    if (httpd_register_uri_handler(s_bootstrap_server, &test_gpio_laser_get) != ESP_OK ||
        httpd_register_uri_handler(s_bootstrap_server, &test_gpio_laser_post) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register bootstrap test GPIO handlers");
        cancel_recovery_timer();
        cancel_start_task();
        httpd_stop(s_bootstrap_server);
        s_bootstrap_server = NULL;
        return ESP_FAIL;
    }
#endif

    ESP_LOGI(TAG, "Bootstrap server started; waiting %ds before starting main application",
             CONFIG_RECOVERY_ENTRY_TIME);
    return ESP_OK;
}
