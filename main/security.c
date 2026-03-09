#include "security.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

///////////////////////////////////////////////////////////////////////////////////////////////////
// Constants
///////////////////////////////////////////////////////////////////////////////////////////////////

#define SECURITY_NVS_NAMESPACE "security"
#define SECURITY_NVS_KEY_UI_PASSWORD_SHA "ui_pwd_sha"
#define UI_PASSWORD_HASH_HEX_LEN 64
#define AUTH_COOKIE_NAME "coinbox_auth"
#define AUTH_TOKEN_NUM_BYTES 16
#define AUTH_TOKEN_HEX_LEN (AUTH_TOKEN_NUM_BYTES * 2)

///////////////////////////////////////////////////////////////////////////////////////////////////
// Private State
///////////////////////////////////////////////////////////////////////////////////////////////////

static const char *TAG = "file_server";

static bool s_ui_password_set;
static char s_ui_password_hash_hex[UI_PASSWORD_HASH_HEX_LEN + 1] = {0};
static char s_auth_session_token[AUTH_TOKEN_HEX_LEN + 1] = {0};
static char s_auth_cookie_header[160] = {0};

///////////////////////////////////////////////////////////////////////////////////////////////////
// Private Helpers
///////////////////////////////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------
// Encoding and Hashing
//-------------------------------------------------------------------------

static void bytes_to_hex(const uint8_t *src, size_t src_len, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789abcdef";

    if (!src || !dst || dst_size == 0) {
        return;
    }
    if (dst_size < (src_len * 2 + 1)) {
        dst[0] = '\0';
        return;
    }

    for (size_t i = 0; i < src_len; ++i) {
        dst[i * 2] = hex[(src[i] >> 4) & 0x0F];
        dst[i * 2 + 1] = hex[src[i] & 0x0F];
    }
    dst[src_len * 2] = '\0';
}

static void security_generate_auth_session_token(void)
{
    uint8_t token[AUTH_TOKEN_NUM_BYTES];

    for (size_t i = 0; i < AUTH_TOKEN_NUM_BYTES; i += sizeof(uint32_t)) {
        uint32_t value = esp_random();
        size_t remaining = AUTH_TOKEN_NUM_BYTES - i;
        size_t copy_len = remaining < sizeof(uint32_t) ? remaining : sizeof(uint32_t);
        memcpy(&token[i], &value, copy_len);
    }

    bytes_to_hex(token, sizeof(token), s_auth_session_token, sizeof(s_auth_session_token));
}

static bool security_hash_password(const char *password, char *out_hex, size_t out_hex_size)
{
    if (!password || !out_hex || out_hex_size < (UI_PASSWORD_HASH_HEX_LEN + 1)) {
        return false;
    }

    uint8_t digest[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);

    int rc = mbedtls_sha256_starts(&ctx, 0);
    if (rc == 0) {
        rc = mbedtls_sha256_update(&ctx, (const unsigned char *)password, strlen(password));
    }
    if (rc == 0) {
        rc = mbedtls_sha256_finish(&ctx, digest);
    }
    mbedtls_sha256_free(&ctx);

    if (rc != 0) {
        return false;
    }

    bytes_to_hex(digest, sizeof(digest), out_hex, out_hex_size);
    return true;
}

//-------------------------------------------------------------------------
// Validation and Request Parsing
//-------------------------------------------------------------------------

static bool security_is_valid_password_len(const char *password)
{
    if (!password) {
        return false;
    }
    size_t len = strlen(password);
    return len > 0 && len <= SECURITY_UI_PASSWORD_MAX_LEN;
}

static bool security_get_cookie_value(httpd_req_t *req, const char *name, char *out, size_t out_size)
{
    if (!req || !name || !out || out_size == 0) {
        return false;
    }

    out[0] = '\0';
    size_t value_size = out_size;
    return httpd_req_get_cookie_val(req, name, out, &value_size) == ESP_OK;
}

static void security_discard_request_body(httpd_req_t *req)
{
    if (!req || req->content_len <= 0) {
        return;
    }

    char discard[128];
    int remaining = req->content_len;
    while (remaining > 0) {
        int chunk_size = remaining < (int)sizeof(discard) ? remaining : (int)sizeof(discard);
        int received = httpd_req_recv(req, discard, chunk_size);
        if (received <= 0) {
            return;
        }
        remaining -= received;
    }
}

//-------------------------------------------------------------------------
// Persistent Storage
//-------------------------------------------------------------------------

static esp_err_t security_load_from_nvs(void)
{
    s_ui_password_set = false;
    s_ui_password_hash_hex[0] = '\0';

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(SECURITY_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    size_t required = sizeof(s_ui_password_hash_hex);
    err = nvs_get_str(nvs, SECURITY_NVS_KEY_UI_PASSWORD_SHA, s_ui_password_hash_hex, &required);
    nvs_close(nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_ui_password_hash_hex[0] = '\0';
        s_ui_password_set = false;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (required != sizeof(s_ui_password_hash_hex)) {
        ESP_LOGW(TAG, "Ignoring invalid stored UI password hash length: %u", (unsigned)required);
        s_ui_password_hash_hex[0] = '\0';
        s_ui_password_set = false;
        return ESP_OK;
    }

    s_ui_password_set = true;
    return ESP_OK;
}

static esp_err_t security_store_password_hash(const char *hash_hex)
{
    if (!hash_hex || strlen(hash_hex) != UI_PASSWORD_HASH_HEX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(SECURITY_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs, SECURITY_NVS_KEY_UI_PASSWORD_SHA, hash_hex);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        return err;
    }

    strlcpy(s_ui_password_hash_hex, hash_hex, sizeof(s_ui_password_hash_hex));
    s_ui_password_set = true;
    security_generate_auth_session_token();
    return ESP_OK;
}

//-------------------------------------------------------------------------
// Route Classification
//-------------------------------------------------------------------------

static bool security_is_ui_entry_uri(const char *uri)
{
    if (!uri) {
        return false;
    }
    if (strcmp(uri, "/") == 0 ||
        strcmp(uri, "/sounds") == 0 || strcmp(uri, "/sounds/") == 0 ||
        strcmp(uri, "/settings") == 0 || strncmp(uri, "/settings/", 10) == 0) {
        return true;
    }
    return false;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Public Interface
///////////////////////////////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------
// Password State
//-------------------------------------------------------------------------

esp_err_t security_init(void)
{
    security_generate_auth_session_token();
    return security_load_from_nvs();
}

bool security_is_password_set(void)
{
    return s_ui_password_set;
}

bool security_password_matches(const char *password)
{
    if (!s_ui_password_set || !password) {
        return false;
    }

    char hash_hex[UI_PASSWORD_HASH_HEX_LEN + 1];
    if (!security_hash_password(password, hash_hex, sizeof(hash_hex))) {
        return false;
    }
    return strcmp(hash_hex, s_ui_password_hash_hex) == 0;
}

esp_err_t security_set_password(const char *password)
{
    if (!security_is_valid_password_len(password)) {
        return ESP_ERR_INVALID_ARG;
    }

    char hash_hex[UI_PASSWORD_HASH_HEX_LEN + 1];
    if (!security_hash_password(password, hash_hex, sizeof(hash_hex))) {
        return ESP_FAIL;
    }

    return security_store_password_hash(hash_hex);
}

esp_err_t security_clear_password(void)
{
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(SECURITY_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_erase_key(nvs, SECURITY_NVS_KEY_UI_PASSWORD_SHA);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        return err;
    }

    s_ui_password_set = false;
    s_ui_password_hash_hex[0] = '\0';
    security_generate_auth_session_token();
    return ESP_OK;
}

//-------------------------------------------------------------------------
// Auth Helpers
//-------------------------------------------------------------------------

void security_sanitize_next_path(const char *candidate, char *out, size_t out_size)
{
    const char *fallback = "/sounds/";

    if (!out || out_size == 0) {
        return;
    }

    if (!candidate || candidate[0] != '/') {
        strlcpy(out, fallback, out_size);
        return;
    }

    if (strncmp(candidate, "/auth/", 6) == 0 || strncmp(candidate, "/login", 6) == 0) {
        strlcpy(out, fallback, out_size);
        return;
    }

    if (strlen(candidate) >= out_size) {
        strlcpy(out, fallback, out_size);
        return;
    }

    strlcpy(out, candidate, out_size);
}

void security_set_auth_cookie_header(httpd_req_t *req)
{
    if (!req || s_auth_session_token[0] == '\0') {
        return;
    }

    int n = snprintf(s_auth_cookie_header, sizeof(s_auth_cookie_header),
                     AUTH_COOKIE_NAME "=%s; Path=/; HttpOnly; SameSite=Strict",
                     s_auth_session_token);
    if (n > 0 && n < (int)sizeof(s_auth_cookie_header)) {
        httpd_resp_set_hdr(req, "Set-Cookie", s_auth_cookie_header);
    }
}

void security_clear_auth_cookie_header(httpd_req_t *req)
{
    if (!req) {
        return;
    }

    httpd_resp_set_hdr(req, "Set-Cookie",
                       AUTH_COOKIE_NAME "=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
}

bool security_is_authenticated_request(httpd_req_t *req)
{
    if (!s_ui_password_set) {
        return true;
    }
    if (!req || s_auth_session_token[0] == '\0') {
        return false;
    }

    char token[AUTH_TOKEN_HEX_LEN + 1];
    if (!security_get_cookie_value(req, AUTH_COOKIE_NAME, token, sizeof(token))) {
        return false;
    }
    return strcmp(token, s_auth_session_token) == 0;
}

esp_err_t security_send_unauthorized(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, "Unauthorized");
    return ESP_FAIL;
}

esp_err_t security_redirect_to_login_for_path(httpd_req_t *req, const char *next_candidate)
{
    char next_path[96];
    char location[160];

    security_sanitize_next_path(next_candidate, next_path, sizeof(next_path));
    int n = snprintf(location, sizeof(location), "/login?next=%s", next_path);
    if (n <= 0 || n >= (int)sizeof(location)) {
        strlcpy(location, "/login", sizeof(location));
    }

    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_sendstr(req, "Authentication required");
    return ESP_FAIL;
}

esp_err_t security_redirect_to_login(httpd_req_t *req)
{
    return security_redirect_to_login_for_path(req, req ? req->uri : NULL);
}

esp_err_t security_require_auth(httpd_req_t *req)
{
    if (!s_ui_password_set || security_is_authenticated_request(req)) {
        return ESP_OK;
    }

    security_discard_request_body(req);

    if (req && req->method == HTTP_GET && security_is_ui_entry_uri(req->uri)) {
        return security_redirect_to_login(req);
    }
    return security_send_unauthorized(req);
}
