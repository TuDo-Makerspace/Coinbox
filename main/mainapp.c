#include "mainapp.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/param.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#include "esp_err.h"
#include "esp_log.h"
#include "logger.h"
#include "files.h"
#include "esp_system.h"
#if CONFIG_NETWORK_ETH_OPENETH
#include "esp_private/system_internal.h"
#endif
#include "audio.h"
#include "gpio.h"
#include "board.h"
#include "mdns_service.h"
#include "network.h"
#include "sdkconfig.h"

#include "esp_vfs.h"
#include "esp_littlefs.h"
#include "esp_http_server.h"
#include "esp_random.h"
#include "nvs.h"
#include "mbedtls/sha256.h"

#include "ota.h"
#include <ctype.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "freertos/task.h"
#include "esp_timer.h"

///////////////////////////////////////////////////////////////////////////////////////////////////
// Constants
///////////////////////////////////////////////////////////////////////////////////////////////////

#define FILE_PATH_MAX (ESP_VFS_PATH_MAX + CONFIG_LITTLEFS_OBJ_NAME_LEN)

#define MAX_FILE_SIZE   (2 * 1024 * 1024) // 2 MB
#define MAX_FILE_SIZE_STR "2MB"
#define STORAGE_PARTITION_LABEL "storage"
// Safety headroom for sidecar metadata and filesystem allocation overhead.
#define UPLOAD_STORAGE_OVERHEAD_BYTES (16 * 1024)

#define SCRATCH_BUFSIZE  8192

// Keep this list MP3-only until non-MP3 playback is fully implemented end-to-end.
#define ALLOWED_AUDIO_EXT_ITEMS(X) \
    X(mp3)

#define AUDIO_EXT_TO_CSTR(ext) #ext,
static const char *ALLOWED_AUDIO_EXTS[] = { ALLOWED_AUDIO_EXT_ITEMS(AUDIO_EXT_TO_CSTR) };
#undef AUDIO_EXT_TO_CSTR

#define AUDIO_EXT_COUNT_ONE(ext) +1
#define PP_CAT_(a, b) a##b
#define PP_CAT(a, b) PP_CAT_(a, b)
#define PP_SECOND(a, b, ...) b
#define PP_PROBE() ~, 1
#define PP_IS_PROBE(...) PP_SECOND(__VA_ARGS__, 0)
#define AUDIO_EXT_TOKEN_IS_MP3(ext) PP_IS_PROBE(PP_CAT(AUDIO_EXT_TOKEN_IS_MP3_PROBE_, ext))
#define AUDIO_EXT_TOKEN_IS_MP3_PROBE_mp3 PP_PROBE()
#define AUDIO_EXT_CHECK_MP3(ext) && AUDIO_EXT_TOKEN_IS_MP3(ext)
enum {
    ALLOWED_AUDIO_EXTS_COUNT = 0 ALLOWED_AUDIO_EXT_ITEMS(AUDIO_EXT_COUNT_ONE),
    ALLOWED_AUDIO_EXTS_ONLY_MP3 = 1 ALLOWED_AUDIO_EXT_ITEMS(AUDIO_EXT_CHECK_MP3),
};
#undef AUDIO_EXT_CHECK_MP3
#undef AUDIO_EXT_TOKEN_IS_MP3_PROBE_mp3
#undef AUDIO_EXT_TOKEN_IS_MP3
#undef PP_IS_PROBE
#undef PP_PROBE
#undef PP_SECOND
#undef PP_CAT
#undef PP_CAT_
#undef AUDIO_EXT_COUNT_ONE

_Static_assert(
    ALLOWED_AUDIO_EXTS_COUNT == 1 && ALLOWED_AUDIO_EXTS_ONLY_MP3,
    "ALLOWED_AUDIO_EXTS must remain MP3-only for now. Playback is currently wired to "
    "mp3_decoder and MP3 frame prefetch logic in main/audio.c, and main/mainapp.c enforces MP3 "
    "for direct play requests. To add other types, implement decoder + prefetch/format setup + "
    "validation/upload handling for each new format before extending this list.");

#define ALLOWED_AUDIO_EXTS_LIST ".mp3"

#define STR_VALUE_(x) #x
#define STR_VALUE(x) STR_VALUE_(x)

#define SECURITY_NVS_NAMESPACE "security"
#define SECURITY_NVS_KEY_UI_PASSWORD_SHA "ui_pwd_sha"
#define UI_PASSWORD_HASH_HEX_LEN 64
#define UI_PASSWORD_MAX_LEN 64
#define AUTH_COOKIE_NAME "coinbox_auth"
#define AUTH_TOKEN_NUM_BYTES 16
#define AUTH_TOKEN_HEX_LEN (AUTH_TOKEN_NUM_BYTES * 2)

#define BOOT_NVS_NAMESPACE "boot"
#define BOOT_NVS_KEY_SOUND_ENABLED "startup_sound"

///////////////////////////////////////////////////////////////////////////////////////////////////
// Vars
///////////////////////////////////////////////////////////////////////////////////////////////////

static const char *TAG = "file_server";

static char s_http_scratch[SCRATCH_BUFSIZE];
static bool s_ui_password_set = false;
static char s_ui_password_hash_hex[UI_PASSWORD_HASH_HEX_LEN + 1] = {0};
static char s_auth_session_token[AUTH_TOKEN_HEX_LEN + 1] = {0};
static char s_auth_cookie_header[160] = {0};
static bool s_boot_sound_enabled = true;

///////////////////////////////////////////////////////////////////////////////////////////////////
// Path and Name Parsing
///////////////////////////////////////////////////////////////////////////////////////////////////

static bool is_mp3_only(const char *name)
{
    if (!name) {
        return false;
    }
    const char *dot = strrchr(name, '.');
    return dot && strcasecmp(dot, ".mp3") == 0;
}

static bool is_meta_file(const char *name)
{
    if (!name) {
        return false;
    }
    const char *dot = strrchr(name, '.');
    return dot && strcmp(dot, ".meta") == 0;
}

static bool is_flat_name_uri(const char *uri)
{
    if (!uri || uri[0] != '/') return false;
    if (strcmp(uri, "/") == 0) return false;
    size_t n = strlen(uri);
    if (n && uri[n - 1] == '/') return false;          // no trailing slash
    return strchr(uri + 1, '/') == NULL;               // no subdirs
}

static void url_decode_inplace(char *str)
{
    char *src = str;
    char *dst = str;
    while (*src) {
        if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char hex[3] = { src[1], src[2], 0 };
            *dst = (char) strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            *dst = ' ';
            src++;
        } else {
            *dst = *src;
            src++;
        }
        dst++;
    }
    *dst = '\0';
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// JSON Helpers
///////////////////////////////////////////////////////////////////////////////////////////////////

static bool json_get_int(const char *json, const char *key, int *out)
{
    if (!json || !key || !out) return false;
    const char *p = strstr(json, key);
    if (!p) return false;
    p = strchr(p, ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return (sscanf(p, "%d", out) == 1);
}

static bool json_get_bool(const char *json, const char *key, bool *out)
{
    if (!json || !key || !out) return false;
    const char *p = strstr(json, key);
    if (!p) return false;
    p = strchr(p, ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (!strncasecmp(p, "true", 4))  { *out = true;  return true; }
    if (!strncasecmp(p, "false", 5)) { *out = false; return true; }
    int v = 0;
    if (sscanf(p, "%d", &v) == 1) { *out = (v != 0); return true; }
    return false;
}

static bool json_get_string(const char *json, const char *key, char *out, size_t out_size)
{
    if (!json || !key || !out || out_size == 0) return false;
    out[0] = '\0';

    const char *p = strstr(json, key);
    if (!p) return false;
    p = strchr(p, ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') return false;
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
        if (w + 1 >= out_size) return false;
        out[w++] = c;
    }
    if (*p != '"') return false;
    out[w] = '\0';
    return true;
}

static bool json_has_key(const char *json, const char *key)
{
    return json && key && strstr(json, key) != NULL;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Security
///////////////////////////////////////////////////////////////////////////////////////////////////

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
        uint32_t r = esp_random();
        size_t remaining = AUTH_TOKEN_NUM_BYTES - i;
        size_t copy_len = remaining < sizeof(uint32_t) ? remaining : sizeof(uint32_t);
        memcpy(&token[i], &r, copy_len);
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

static bool security_is_valid_password_len(const char *password)
{
    if (!password) {
        return false;
    }
    size_t len = strlen(password);
    return len > 0 && len <= UI_PASSWORD_MAX_LEN;
}

static void security_sanitize_next_path(const char *candidate, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }

    const char *fallback = "/sounds/";
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

static void security_set_auth_cookie_header(httpd_req_t *req)
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

static void security_clear_auth_cookie_header(httpd_req_t *req)
{
    if (!req) {
        return;
    }
    httpd_resp_set_hdr(req, "Set-Cookie",
                       AUTH_COOKIE_NAME "=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
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

static bool security_is_authenticated_request(httpd_req_t *req)
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

static bool security_password_matches(const char *password)
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

static esp_err_t security_set_password(const char *password)
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

static esp_err_t security_clear_password(void)
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

static esp_err_t security_redirect_to_login_for_path(httpd_req_t *req, const char *next_candidate)
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

static esp_err_t security_redirect_to_login(httpd_req_t *req)
{
    return security_redirect_to_login_for_path(req, req ? req->uri : NULL);
}

static esp_err_t security_send_unauthorized(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, "Unauthorized");
    return ESP_FAIL;
}

static esp_err_t security_require_auth(httpd_req_t *req)
{
    if (!s_ui_password_set || security_is_authenticated_request(req)) {
        return ESP_OK;
    }

    if (req && req->method == HTTP_GET && security_is_ui_entry_uri(req->uri)) {
        return security_redirect_to_login(req);
    }
    return security_send_unauthorized(req);
}

static esp_err_t security_init(void)
{
    security_generate_auth_session_token();
    return security_load_from_nvs();
}

esp_err_t mainapp_reset_security_defaults(void)
{
    return security_clear_password();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Start-Up Settings
///////////////////////////////////////////////////////////////////////////////////////////////////

static esp_err_t boot_config_load_from_nvs(void)
{
    s_boot_sound_enabled = true;

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(BOOT_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    uint8_t enabled = 1;
    err = nvs_get_u8(nvs, BOOT_NVS_KEY_SOUND_ENABLED, &enabled);
    nvs_close(nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_boot_sound_enabled = true;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    s_boot_sound_enabled = (enabled != 0);
    return ESP_OK;
}

static esp_err_t boot_config_store_enabled(bool enabled)
{
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(BOOT_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(nvs, BOOT_NVS_KEY_SOUND_ENABLED, enabled ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        return err;
    }

    s_boot_sound_enabled = enabled;
    return ESP_OK;
}

esp_err_t mainapp_reset_boot_defaults(void)
{
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(BOOT_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        return err;
    }

    if (err == ESP_OK) {
        err = nvs_erase_key(nvs, BOOT_NVS_KEY_SOUND_ENABLED);
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
    }

    s_boot_sound_enabled = true;
    return ESP_OK;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Content Type and URI Helpers
///////////////////////////////////////////////////////////////////////////////////////////////////

static esp_err_t set_content_type_from_file(httpd_req_t *req, const char *filename)
{
    if (!filename || !*filename) {
        return httpd_resp_set_type(req, "application/octet-stream");
    }

    const char *dot = strrchr(filename, '.');
    if (!dot || dot == filename || *(dot + 1) == '\0') {
        return httpd_resp_set_type(req, "application/octet-stream");
    }

    /* dot points to ".ext" */
    if (strcasecmp(dot, ".mp3") == 0) {
        return httpd_resp_set_type(req, "audio/mpeg");
    } else if (strcasecmp(dot, ".wav") == 0) {
        return httpd_resp_set_type(req, "audio/wav");
    } else if (strcasecmp(dot, ".ogg") == 0) {
        return httpd_resp_set_type(req, "audio/ogg");
    } else if (strcasecmp(dot, ".flac") == 0) {
        return httpd_resp_set_type(req, "audio/flac");
    } else if (strcasecmp(dot, ".aac") == 0) {
        return httpd_resp_set_type(req, "audio/aac");
    } else if (strcasecmp(dot, ".m4a") == 0) {
        return httpd_resp_set_type(req, "audio/mp4");
    } else if (strcasecmp(dot, ".pdf") == 0) {
        return httpd_resp_set_type(req, "application/pdf");
    } else if (strcasecmp(dot, ".html") == 0) {
        return httpd_resp_set_type(req, "text/html");
    } else if (strcasecmp(dot, ".jpeg") == 0 || strcasecmp(dot, ".jpg") == 0) {
        return httpd_resp_set_type(req, "image/jpeg");
    } else if (strcasecmp(dot, ".ico") == 0) {
        return httpd_resp_set_type(req, "image/x-icon");
    }

    return httpd_resp_set_type(req, "application/octet-stream");
}

static bool is_audio_filename(const char *name)
{
    if (!name || !*name) {
        return false;
    }
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name || *(dot + 1) == '\0') {
        return false;
    }
    const char *ext = dot + 1;
    for (size_t i = 0; i < ALLOWED_AUDIO_EXTS_COUNT; ++i) {
        if (strcasecmp(ext, ALLOWED_AUDIO_EXTS[i]) == 0) {
            return true;
        }
    }
    return false;
}

static const char *sound_display_name(const char *name)
{
    return files_is_default_sound_name(name) ? FILES_DEFAULT_SOUND_LABEL : name;
}

static void build_default_sound_reserved_message(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    int n = snprintf(out,
                     out_size,
                     "%s is reserved for the built-in default sound.",
                     FILES_DEFAULT_SOUND_NAME);
    if (n < 0 || n >= (int)out_size) {
        strlcpy(out, "Reserved default sound name.", out_size);
    }
}

static void build_default_sound_protected_message(char *out, size_t out_size, const char *action)
{
    if (!out || out_size == 0) {
        return;
    }
    int n = snprintf(out,
                     out_size,
                     "%s is reserved for the built-in default sound and cannot be %s.",
                     FILES_DEFAULT_SOUND_NAME,
                     action ? action : "modified");
    if (n < 0 || n >= (int)out_size) {
        strlcpy(out, "Protected default sound.", out_size);
    }
}

static const char* get_path_from_uri(char *dest, const char *base_path, const char *uri, size_t destsize)
{
    const size_t base_pathlen = strlen(base_path);
    size_t pathlen = strlen(uri);

    const char *quest = strchr(uri, '?');
    if (quest) {
        pathlen = MIN(pathlen, quest - uri);
    }
    const char *hash = strchr(uri, '#');
    if (hash) {
        pathlen = MIN(pathlen, hash - uri);
    }

    if (base_pathlen + pathlen + 1 > destsize) {
        /* Full path string won't fit into destination buffer */
        return NULL;
    }

    /* Construct full path (base + path) */
    strcpy(dest, base_path);
    strlcpy(dest + base_pathlen, uri, pathlen + 1);

    const char *result = dest + base_pathlen;
    ESP_LOGI(TAG, "Destination: %s, Base Path: %s, Return: %s", dest, base_path, result);
    return result;

    /* Return pointer to path, skipping the base */
    return dest + base_pathlen;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Static Assets
///////////////////////////////////////////////////////////////////////////////////////////////////

static esp_err_t favicon_get_handler(httpd_req_t *req)
{
    extern const unsigned char favicon_ico_start[] asm("_binary_favicon_ico_start");
    extern const unsigned char favicon_ico_end[]   asm("_binary_favicon_ico_end");
    const size_t favicon_ico_size = (favicon_ico_end - favicon_ico_start);
    httpd_resp_set_type(req, "image/x-icon");
    httpd_resp_send(req, (const char *)favicon_ico_start, favicon_ico_size);
    return ESP_OK;
}

static esp_err_t http_resp_settings_html(httpd_req_t *req)
{
    esp_err_t auth_err = security_require_auth(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }

    extern const unsigned char settings_html_start[] asm("_binary_settings_html_start");
    extern const unsigned char settings_html_end[] asm("_binary_settings_html_end");
    const size_t settings_html_size = (settings_html_end - settings_html_start);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, (const char *)settings_html_start, settings_html_size);
    return ESP_OK;
}

static esp_err_t http_resp_login_html(httpd_req_t *req)
{
    char next_path[96];
    security_sanitize_next_path("/sounds/", next_path, sizeof(next_path));

    int query_len = httpd_req_get_url_query_len(req);
    if (query_len > 0 && query_len < 120) {
        char query[120];
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
            char next_raw[96];
            if (httpd_query_key_value(query, "next", next_raw, sizeof(next_raw)) == ESP_OK) {
                security_sanitize_next_path(next_raw, next_path, sizeof(next_path));
            }
        }
    }

    if (!s_ui_password_set || security_is_authenticated_request(req)) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_set_hdr(req, "Location", next_path);
        httpd_resp_sendstr(req, "Redirecting");
        return ESP_OK;
    }

    extern const unsigned char login_html_start[] asm("_binary_login_html_start");
    extern const unsigned char login_html_end[] asm("_binary_login_html_end");
    const size_t login_html_size = (login_html_end - login_html_start);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, (const char *)login_html_start, login_html_size);
    return ESP_OK;
}

static esp_err_t http_resp_logs(httpd_req_t *req)
{
    esp_err_t auth_err = security_require_auth(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }

    const char *buf = logger_get_buffer();
    size_t len = logger_get_size();
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

static esp_err_t http_resp_navbar_js(httpd_req_t *req)
{
    extern const unsigned char navbar_js_start[] asm("_binary_navbar_js_start");
    extern const unsigned char navbar_js_end[] asm("_binary_navbar_js_end");
    const size_t navbar_js_size = (navbar_js_end - navbar_js_start);
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_send(req, (const char *)navbar_js_start, navbar_js_size);
    return ESP_OK;
}

static esp_err_t http_resp_connection_monitor_js(httpd_req_t *req)
{
    extern const unsigned char connection_monitor_js_start[] asm("_binary_connection_monitor_js_start");
    extern const unsigned char connection_monitor_js_end[] asm("_binary_connection_monitor_js_end");
    const size_t connection_monitor_js_size = (connection_monitor_js_end - connection_monitor_js_start);
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, (const char *)connection_monitor_js_start, connection_monitor_js_size);
    return ESP_OK;
}

static esp_err_t http_resp_glyphs_js(httpd_req_t *req)
{
    extern const unsigned char glyphs_js_start[] asm("_binary_glyphs_js_start");
    extern const unsigned char glyphs_js_end[] asm("_binary_glyphs_js_end");
    const size_t glyphs_js_size = (glyphs_js_end - glyphs_js_start);
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, (const char *)glyphs_js_start, glyphs_js_size);
    return ESP_OK;
}

static esp_err_t http_resp_glyphs_css(httpd_req_t *req)
{
    extern const unsigned char glyphs_css_start[] asm("_binary_glyphs_css_start");
    extern const unsigned char glyphs_css_end[] asm("_binary_glyphs_css_end");
    const size_t glyphs_css_size = (glyphs_css_end - glyphs_css_start);
    httpd_resp_set_type(req, "text/css");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, (const char *)glyphs_css_start, glyphs_css_size);
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

static esp_err_t send_security_config_json(httpd_req_t *req)
{
    char resp[48];
    int len = snprintf(resp, sizeof(resp),
                       "{\"password_set\":%s}",
                       s_ui_password_set ? "true" : "false");
    if (len < 0 || len >= (int)sizeof(resp)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Render failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

static esp_err_t send_boot_config_json(httpd_req_t *req)
{
    char resp[56];
    int len = snprintf(resp, sizeof(resp),
                       "{\"boot_sound_enabled\":%s}",
                       s_boot_sound_enabled ? "true" : "false");
    if (len < 0 || len >= (int)sizeof(resp)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Render failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

static esp_err_t auth_login_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 320) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request body");
        return ESP_FAIL;
    }

    char body[321];
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

    char next_path[96];
    security_sanitize_next_path("/sounds/", next_path, sizeof(next_path));
    if (json_has_key(body, "next")) {
        char next_raw[96];
        if (!json_get_string(body, "next", next_raw, sizeof(next_raw))) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid next path");
            return ESP_FAIL;
        }
        security_sanitize_next_path(next_raw, next_path, sizeof(next_path));
    }

    if (!s_ui_password_set) {
        security_clear_auth_cookie_header(req);
        char resp[128];
        int len = snprintf(resp, sizeof(resp),
                           "{\"ok\":true,\"next\":\"%s\",\"password_set\":false}",
                           next_path);
        if (len < 0 || len >= (int)sizeof(resp)) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Render failed");
            return ESP_FAIL;
        }
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_send(req, resp, len);
        return ESP_OK;
    }

    char password[UI_PASSWORD_MAX_LEN + 1];
    if (!json_get_string(body, "password", password, sizeof(password))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Password missing");
        return ESP_FAIL;
    }

    if (!security_password_matches(password)) {
        security_clear_auth_cookie_header(req);
        return security_send_unauthorized(req);
    }

    security_set_auth_cookie_header(req);
    char resp[128];
    int len = snprintf(resp, sizeof(resp),
                       "{\"ok\":true,\"next\":\"%s\",\"password_set\":true}",
                       next_path);
    if (len < 0 || len >= (int)sizeof(resp)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Render failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

static esp_err_t security_config_get_handler(httpd_req_t *req)
{
    esp_err_t auth_err = security_require_auth(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }
    return send_security_config_json(req);
}

static esp_err_t security_config_post_handler(httpd_req_t *req)
{
    esp_err_t auth_err = security_require_auth(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }

    if (req->content_len <= 0 || req->content_len > 320) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request body");
        return ESP_FAIL;
    }

    char body[321];
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

    bool remove = false;
    bool remove_key_present = json_has_key(body, "remove");
    if (remove_key_present && !json_get_bool(body, "remove", &remove)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid remove flag");
        return ESP_FAIL;
    }

    bool password_key_present = json_has_key(body, "password");
    esp_err_t err = ESP_OK;
    if (remove) {
        err = security_clear_password();
    } else if (password_key_present) {
        char password[UI_PASSWORD_MAX_LEN + 1];
        if (!json_get_string(body, "password", password, sizeof(password))) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid password");
            return ESP_FAIL;
        }
        if (password[0] == '\0') {
            err = security_clear_password();
        } else {
            err = security_set_password(password);
        }
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Nothing to update");
        return ESP_FAIL;
    }

    if (err != ESP_OK) {
        if (err == ESP_ERR_INVALID_ARG) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "Password must be 1-64 characters.");
        } else {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save security config");
        }
        return ESP_FAIL;
    }

    if (s_ui_password_set) {
        security_set_auth_cookie_header(req);
    } else {
        security_clear_auth_cookie_header(req);
    }
    return send_security_config_json(req);
}

static esp_err_t boot_config_get_handler(httpd_req_t *req)
{
    esp_err_t auth_err = security_require_auth(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }
    return send_boot_config_json(req);
}

static esp_err_t boot_config_post_handler(httpd_req_t *req)
{
    esp_err_t auth_err = security_require_auth(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }

    if (req->content_len <= 0 || req->content_len > 128) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request body");
        return ESP_FAIL;
    }

    char body[129];
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

    bool boot_sound_enabled = s_boot_sound_enabled;
    if (!json_has_key(body, "boot_sound_enabled")) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Nothing to update");
        return ESP_FAIL;
    }
    if (!json_get_bool(body, "boot_sound_enabled", &boot_sound_enabled)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid boot sound setting");
        return ESP_FAIL;
    }

    esp_err_t err = boot_config_store_enabled(boot_sound_enabled);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save boot config");
        return ESP_FAIL;
    }

    return send_boot_config_json(req);
}

static esp_err_t network_ips_handler(httpd_req_t *req)
{
    esp_err_t auth_err = security_require_auth(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }

    char ap_ip[16] = {0};
    char sta_ip[16] = {0};
    char sta_ssid[33] = {0};
    bool ap_disabled_for_sta = false;
    char ap_ip_esc[32] = {0};
    char sta_ip_esc[32] = {0};
    char sta_ssid_esc[80] = {0};
    (void)network_get_ipv4_strings(ap_ip, sizeof(ap_ip), sta_ip, sizeof(sta_ip));
    (void)network_get_connected_sta_ssid(sta_ssid, sizeof(sta_ssid));
    (void)network_get_ap_runtime_disabled_for_sta(&ap_disabled_for_sta);
    json_escape_copy(ap_ip, ap_ip_esc, sizeof(ap_ip_esc));
    json_escape_copy(sta_ip, sta_ip_esc, sizeof(sta_ip_esc));
    json_escape_copy(sta_ssid, sta_ssid_esc, sizeof(sta_ssid_esc));

    char resp[320];
    int len = snprintf(resp, sizeof(resp),
                       "{\"ap\":\"%s\",\"sta\":\"%s\",\"sta_ssid\":\"%s\","
                       "\"ap_disabled_for_sta\":%s}",
                       ap_ip_esc,
                       sta_ip_esc,
                       sta_ssid_esc,
                       ap_disabled_for_sta ? "true" : "false");
    if (len < 0 || len >= (int)sizeof(resp)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Render failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

static esp_err_t send_network_config_json(httpd_req_t *req)
{
    network_public_config_t cfg = {0};
    esp_err_t err = network_get_public_config(&cfg);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to load network config");
        return ESP_FAIL;
    }

    char ap_ssid_esc[80] = {0};
    char sta_ssid_esc[80] = {0};
    json_escape_copy(cfg.ap_ssid, ap_ssid_esc, sizeof(ap_ssid_esc));
    json_escape_copy(cfg.sta_ssid, sta_ssid_esc, sizeof(sta_ssid_esc));

    char resp[640];
    int len = snprintf(resp, sizeof(resp),
                       "{\"ap_supported\":%s,\"sta_supported\":%s,"
                       "\"disable_ap_when_sta_connected_supported\":%s,"
                       "\"ap_ssid\":\"%s\",\"ap_password_set\":%s,\"ap_reboot_required\":%s,"
                       "\"disable_ap_when_sta_connected\":%s,"
                       "\"disable_ap_when_sta_connected_reboot_required\":%s,"
                       "\"sta_ssid\":\"%s\",\"sta_password_set\":%s,\"sta_reboot_required\":%s}",
#if CONFIG_NETWORK_WIFI_AP
                       "true",
#else
                       "false",
#endif
#if CONFIG_NETWORK_WIFI_STA
                       "true",
#else
                       "false",
#endif
#if CONFIG_NETWORK_WIFI_AP && CONFIG_NETWORK_WIFI_STA
                       "true",
#else
                       "false",
#endif
                       ap_ssid_esc,
                       cfg.ap_password_set ? "true" : "false",
                       cfg.ap_reboot_required ? "true" : "false",
                       cfg.disable_ap_when_sta_connected ? "true" : "false",
                       cfg.disable_ap_when_sta_connected_reboot_required ? "true" : "false",
                       sta_ssid_esc,
                       cfg.sta_password_set ? "true" : "false",
                       cfg.sta_reboot_required ? "true" : "false");
    if (len < 0 || len >= (int)sizeof(resp)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Render failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

static esp_err_t network_config_get_handler(httpd_req_t *req)
{
    esp_err_t auth_err = security_require_auth(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }
    return send_network_config_json(req);
}

static esp_err_t network_config_post_handler(httpd_req_t *req)
{
    esp_err_t auth_err = security_require_auth(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }

    if (req->content_len <= 0 || req->content_len > 640) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request body");
        return ESP_FAIL;
    }

    char body[641];
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

    network_public_config_t current = {0};
    esp_err_t err = network_get_public_config(&current);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to load current config");
        return ESP_FAIL;
    }

    char ap_ssid[NETWORK_WIFI_SSID_MAX_LEN + 1];
    char sta_ssid[NETWORK_WIFI_SSID_MAX_LEN + 1];
    char ap_password[NETWORK_WIFI_PSK_MAX_LEN + 1];
    char sta_password[NETWORK_WIFI_PSK_MAX_LEN + 1];
    bool disable_ap_when_sta_connected = current.disable_ap_when_sta_connected;
    strlcpy(ap_ssid, current.ap_ssid, sizeof(ap_ssid));
    strlcpy(sta_ssid, current.sta_ssid, sizeof(sta_ssid));
    ap_password[0] = '\0';
    sta_password[0] = '\0';
    bool ap_password_provided = false;
    bool disable_ap_when_sta_connected_provided = false;
    bool sta_password_provided = false;

    if (json_has_key(body, "ap_ssid") &&
        !json_get_string(body, "ap_ssid", ap_ssid, sizeof(ap_ssid))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid AP SSID");
        return ESP_FAIL;
    }
    if (json_has_key(body, "sta_ssid") &&
        !json_get_string(body, "sta_ssid", sta_ssid, sizeof(sta_ssid))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid STA SSID");
        return ESP_FAIL;
    }
    if (json_has_key(body, "ap_password")) {
        if (!json_get_string(body, "ap_password", ap_password, sizeof(ap_password))) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid AP password");
            return ESP_FAIL;
        }
        ap_password_provided = true;
    }
    if (json_has_key(body, "disable_ap_when_sta_connected")) {
        if (!json_get_bool(body, "disable_ap_when_sta_connected", &disable_ap_when_sta_connected)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid AP auto-disable setting");
            return ESP_FAIL;
        }
        disable_ap_when_sta_connected_provided = true;
    }
    if (json_has_key(body, "sta_password")) {
        if (!json_get_string(body, "sta_password", sta_password, sizeof(sta_password))) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid STA password");
            return ESP_FAIL;
        }
        sta_password_provided = true;
    }

    err = network_update_config(ap_ssid, ap_password, ap_password_provided,
                                disable_ap_when_sta_connected, disable_ap_when_sta_connected_provided,
                                sta_ssid, sta_password, sta_password_provided);
    if (err != ESP_OK) {
        if (err == ESP_ERR_INVALID_ARG) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "Invalid network settings. SSID must be 1-32 chars, and passwords must be empty or 8-64 chars.");
        } else if (err == ESP_ERR_NOT_SUPPORTED) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "Requested Wi-Fi mode is not enabled in this firmware build.");
        } else {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save network config");
        }
        return ESP_FAIL;
    }

    return send_network_config_json(req);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Sounds Page
///////////////////////////////////////////////////////////////////////////////////////////////////

static esp_err_t sounds_index_get_handler(httpd_req_t *req)
{
    esp_err_t auth_err = security_require_auth(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }

    httpd_resp_set_type(req, "text/html");

    const char *dirpath = CONFIG_BASE_PATH;

    char entrypath[FILE_PATH_MAX];
    const char *entrytype;

    struct dirent *entry;
    struct stat entry_stat;

    DIR *dir = opendir(dirpath);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open dir: %s", dirpath);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Storage not available");
        return ESP_FAIL;
    }

    strlcpy(entrypath, dirpath, sizeof(entrypath));

    /* embedded sounds.html */
    extern const unsigned char upload_script_start[] asm("_binary_sounds_html_start");
    extern const unsigned char upload_script_end[]   asm("_binary_sounds_html_end");
    const size_t upload_script_size = (upload_script_end - upload_script_start);

    httpd_resp_send_chunk(req, (const char *)upload_script_start, upload_script_size);

    /* For a flat layout, advertise CURRENT_PATH as "/sounds/" */
    httpd_resp_sendstr_chunk(req, "<script>window.CURRENT_PATH='/sounds/';</script>");

    int row_idx = 0;
    int yield_counter = 0;

    while ((entry = readdir(dir)) != NULL) {
        entrytype = (entry->d_type == DT_DIR ? "directory" : "file");

        bool is_dir = (entry->d_type == DT_DIR);
        if (is_dir) {
            continue; // flat: do not show directories
        }
        if (is_meta_file(entry->d_name)) {
            continue;
        }

        int len = snprintf(entrypath, sizeof(entrypath), "%s/%s", dirpath, entry->d_name);
        if (len < 0 || len >= (int)sizeof(entrypath)) {
            ESP_LOGE(TAG, "Path too long: %s + %s", dirpath, entry->d_name);
            continue;
        }
        if (stat(entrypath, &entry_stat) == -1) {
            ESP_LOGE(TAG, "Failed to stat %s : %s", entrytype, entry->d_name);
            continue;
        }

        file_properties_t meta;
        if (files_read_meta(entry->d_name, &meta) != ESP_OK) {
            ESP_LOGW(TAG, "Skipping non-entry file: %s", entrypath);
            continue;
        }
        bool is_protected_sound = files_is_default_sound_name(entry->d_name);
        const char *display_name = sound_display_name(entry->d_name);

        char row_id[32];
        snprintf(row_id, sizeof(row_id), "row-%d", row_idx++);

        httpd_resp_sendstr_chunk(req, "<div class=\"file-item\" id=\"");
        httpd_resp_sendstr_chunk(req, row_id);
        httpd_resp_sendstr_chunk(req, "\" data-file-uri=\"/sounds/");
        httpd_resp_sendstr_chunk(req, entry->d_name);
        httpd_resp_sendstr_chunk(req, "\" data-file-name=\"");
        httpd_resp_sendstr_chunk(req, entry->d_name);
        httpd_resp_sendstr_chunk(req, "\" data-probability=\"");
        char numbuf[16];
        snprintf(numbuf, sizeof(numbuf), "%u", (unsigned)meta.probability);
        httpd_resp_sendstr_chunk(req, numbuf);
        httpd_resp_sendstr_chunk(req, "\" data-volume=\"");
        snprintf(numbuf, sizeof(numbuf), "%u", (unsigned)meta.volume);
        httpd_resp_sendstr_chunk(req, numbuf);
        httpd_resp_sendstr_chunk(req, "\" data-enabled=\"");
        httpd_resp_sendstr_chunk(req, meta.enabled ? "1" : "0");
        httpd_resp_sendstr_chunk(req, "\" data-protected=\"");
        httpd_resp_sendstr_chunk(req, is_protected_sound ? "1" : "0");
        httpd_resp_sendstr_chunk(req, "\">");

        httpd_resp_sendstr_chunk(req, "<div class=\"file-row-main\">");
        httpd_resp_sendstr_chunk(req, "<div class=\"file-name-wrap\"><a class=\"file-name\" href=\"/sounds/");
        httpd_resp_sendstr_chunk(req, entry->d_name);
        httpd_resp_sendstr_chunk(req, "\" title=\"");
        httpd_resp_sendstr_chunk(req, display_name);
        httpd_resp_sendstr_chunk(req, "\">");
        httpd_resp_sendstr_chunk(req, display_name);
        httpd_resp_sendstr_chunk(req, "</a>");
        httpd_resp_sendstr_chunk(req,
                                 "<input class=\"file-name-edit\" data-k=\"name-edit\" type=\"text\" "
                                 "autocomplete=\"off\" spellcheck=\"false\"");
        if (is_protected_sound) {
            httpd_resp_sendstr_chunk(req, " disabled");
        }
        httpd_resp_sendstr_chunk(req, ">");
        httpd_resp_sendstr_chunk(req, "</div>");
        httpd_resp_sendstr_chunk(req, "<div class=\"row-actions\">");
        httpd_resp_sendstr_chunk(req, "<label class=\"switch\"><input type=\"checkbox\" data-k=\"enabled-toggle\"><span class=\"slider\"></span></label>");
        httpd_resp_sendstr_chunk(req, "<button class=\"icon-btn play-btn\" data-k=\"play\" aria-label=\"Play\"><svg viewBox=\"0 0 24 24\" aria-hidden=\"true\"><path d=\"M8.5 5.5v13l9-6.5-9-6.5Z\"/></svg></button>");
        httpd_resp_sendstr_chunk(req, "<button class=\"chev\" data-row-id=\"");
        httpd_resp_sendstr_chunk(req, row_id);
        httpd_resp_sendstr_chunk(req, "\" data-open=\"0\">&#9881;</button>");
        httpd_resp_sendstr_chunk(req, "</div></div>");
        httpd_resp_sendstr_chunk(req, "<div class=\"details\" data-for-row=\"");
        httpd_resp_sendstr_chunk(req, row_id);
        httpd_resp_sendstr_chunk(req, "\">");
        httpd_resp_sendstr_chunk(req, "<div class=\"details-inner\">");

        httpd_resp_sendstr_chunk(req, "<div class=\"prop prob-row\"><label>Weight</label>");
        httpd_resp_sendstr_chunk(req, "<div class=\"slider-wrap\">");
        httpd_resp_sendstr_chunk(req, "<button class=\"nudge\" type=\"button\" data-k=\"probability-minus\">-</button>");
        httpd_resp_sendstr_chunk(req, "<input type=\"range\" min=\"0\" max=\"100\" data-k=\"probability\">");
        httpd_resp_sendstr_chunk(req, "<button class=\"nudge\" type=\"button\" data-k=\"probability-plus\">+</button>");
        httpd_resp_sendstr_chunk(req, "</div>");
        httpd_resp_sendstr_chunk(req, "<input type=\"number\" min=\"0\" max=\"100\" inputmode=\"numeric\" data-k=\"probability-num\">");
        httpd_resp_sendstr_chunk(req, "<span class=\"percent\">%</span>");
        httpd_resp_sendstr_chunk(req, "</div>");

        httpd_resp_sendstr_chunk(req, "<div class=\"prop vol-row\"><label>Volume</label>");
        httpd_resp_sendstr_chunk(req, "<div class=\"slider-wrap volume-slider\">");
        httpd_resp_sendstr_chunk(req, "<button class=\"nudge\" type=\"button\" data-k=\"volume-minus\">-</button>");
        httpd_resp_sendstr_chunk(req, "<div class=\"range-wrap\">");
        httpd_resp_sendstr_chunk(req, "<input type=\"range\" min=\"0\" max=\"" STR_VALUE(FILE_VOLUME_MAX) "\" data-k=\"volume\">");
        httpd_resp_sendstr_chunk(req, "<span class=\"volume-overdrive-marker\" aria-hidden=\"true\"></span>");
        httpd_resp_sendstr_chunk(req, "</div>");
        httpd_resp_sendstr_chunk(req, "<button class=\"nudge\" type=\"button\" data-k=\"volume-plus\">+</button>");
        httpd_resp_sendstr_chunk(req, "</div>");
        httpd_resp_sendstr_chunk(req, "<input type=\"number\" min=\"0\" max=\"" STR_VALUE(FILE_VOLUME_MAX) "\" inputmode=\"numeric\" data-k=\"volume-num\">");
        httpd_resp_sendstr_chunk(req, "<span class=\"percent\">%</span>");
        httpd_resp_sendstr_chunk(req, "</div>");

        httpd_resp_sendstr_chunk(req, "<div class=\"action-row\">");
        httpd_resp_sendstr_chunk(req, "<button class=\"delete-btn\" type=\"button\"");
        if (is_protected_sound) {
            httpd_resp_sendstr_chunk(req, " disabled");
        }
        httpd_resp_sendstr_chunk(req, ">"
                                    "<svg viewBox=\"0 0 24 24\" aria-hidden=\"true\">"
                                    "<path d=\"M9 3a1 1 0 0 0-1 1v1H5.5a1 1 0 1 0 0 2H6v12a2 2 0 0 0 2 2h8a2 2 0 0 0 2-2V7h0.5a1 1 0 1 0 0-2H16V4a1 1 0 0 0-1-1H9Zm1 2h4V5h-4V5Zm-1 4a1 1 0 1 1 2 0v8a1 1 0 1 1-2 0V9Zm6-1a1 1 0 0 1 1 1v8a1 1 0 1 1-2 0V9a1 1 0 0 1 1-1Z\"/>"
                                    "</svg>Delete file</button>");
        httpd_resp_sendstr_chunk(req, "</div>");
        httpd_resp_sendstr_chunk(req, "</div></div>"); /* .details-inner + .details */
        httpd_resp_sendstr_chunk(req, "</div>\n");

        if (++yield_counter >= 4) {
            vTaskDelay(1);
            yield_counter = 0;
        }
    }

    if (row_idx == 0) {
        httpd_resp_sendstr_chunk(req,
                                 "<div class=\"file-empty\">"
                                 "It seems that no sounds have been added yet."
                                 "</div>");
    }

    closedir(dir);

    httpd_resp_sendstr_chunk(req, "</div></div></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t sounds_handle_delete(httpd_req_t *req, const char *filepath, const char *base_name)
{
    if (files_is_default_sound_name(base_name)) {
        char msg[160];
        build_default_sound_protected_message(msg, sizeof(msg), "deleted");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
        return ESP_FAIL;
    }

    struct stat file_stat;
    if (stat(filepath, &file_stat) == -1) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File does not exist");
        return ESP_FAIL;
    }
    if (files_delete_with_meta(base_name) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to delete file");
        return ESP_FAIL;
    }
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/sounds/");
#ifdef CONFIG_EXAMPLE_HTTPD_CONN_CLOSE_HEADER
    httpd_resp_set_hdr(req, "Connection", "close");
#endif
    httpd_resp_sendstr(req, "File deleted successfully");
    return ESP_OK;
}

static esp_err_t sounds_handle_rename(httpd_req_t *req, const char *filepath, const char *base_name,
                                      const char *rename_val)
{
    if (files_is_default_sound_name(base_name)) {
        char msg[160];
        build_default_sound_protected_message(msg, sizeof(msg), "renamed");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
        return ESP_FAIL;
    }

    if (!rename_val || rename_val[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Rename target missing");
        return ESP_FAIL;
    }
    if ((strchr(rename_val, '/') || strchr(rename_val, '\\'))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid rename");
        return ESP_FAIL;
    }
    const char *old_ext = strrchr(base_name, '.');
    if (!old_ext || !is_audio_filename(base_name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid source filename");
        return ESP_FAIL;
    }

    char rename_target[FILE_ENTRY_NAME_MAX];
    size_t old_ext_len = strlen(old_ext);
    size_t base_len = strlen(rename_val);

    if (base_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Rename target missing");
        return ESP_FAIL;
    }
    if (base_len + old_ext_len >= sizeof(rename_target)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "New name too long");
        return ESP_FAIL;
    }

    memcpy(rename_target, rename_val, base_len);
    rename_target[base_len] = '\0';
    strlcat(rename_target, old_ext, sizeof(rename_target));

    if (strcmp(rename_target, base_name) == 0) {
        httpd_resp_sendstr(req, "Unchanged");
        return ESP_OK;
    }

    if (files_is_default_sound_name(rename_target)) {
        char msg[160];
        build_default_sound_reserved_message(msg, sizeof(msg));
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
        return ESP_FAIL;
    }

    if (is_meta_file(rename_target) || !is_audio_filename(rename_target)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Only audio files are allowed (" ALLOWED_AUDIO_EXTS_LIST ")");
        return ESP_FAIL;
    }

    struct stat file_stat;
    if (stat(filepath, &file_stat) == -1) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File does not exist");
        return ESP_FAIL;
    }
    char *last_slash = strrchr(filepath, '/');
    if (!last_slash) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Bad path");
        return ESP_FAIL;
    }
    size_t dir_len = (size_t)(last_slash - filepath + 1);
    char new_filepath[FILE_PATH_MAX];
    if (dir_len + strlen(rename_target) >= sizeof(new_filepath)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "New name too long");
        return ESP_FAIL;
    }
    memcpy(new_filepath, filepath, dir_len);
    strlcpy(new_filepath + dir_len, rename_target, sizeof(new_filepath) - dir_len);
    if (stat(new_filepath, &file_stat) == 0) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "Target exists");
        return ESP_OK;
    }
    char new_meta_filepath[FILE_PATH_MAX];
    if (dir_len + strlen(rename_target) + strlen(".meta") >= sizeof(new_meta_filepath)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "New name too long");
        return ESP_FAIL;
    }
    memcpy(new_meta_filepath, filepath, dir_len);
    new_meta_filepath[dir_len] = '\0';
    strlcpy(new_meta_filepath + dir_len, rename_target, sizeof(new_meta_filepath) - dir_len);
    strlcat(new_meta_filepath, ".meta", sizeof(new_meta_filepath));
    if (stat(new_meta_filepath, &file_stat) == 0) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "Target exists");
        return ESP_OK;
    }
    if (files_rename_with_meta(base_name, rename_target) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Rename failed");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "Renamed");
    return ESP_OK;
}

static esp_err_t sounds_handle_download(httpd_req_t *req, const char *filepath, const char *base_name,
                                        const char *filename)
{
    struct stat file_stat;
    if (stat(filepath, &file_stat) == -1) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File does not exist");
        return ESP_FAIL;
    }

    if (S_ISDIR(file_stat.st_mode)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_FAIL;
    }

    FILE *fd = fopen(filepath, "rb");
    if (!fd) {
        ESP_LOGE(TAG, "Failed to read existing file : %s", filepath);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read existing file");
        return ESP_FAIL;
    }

    // Ensure file has metadata, else its invalid!
    file_properties_t meta;
    if (files_read_meta(base_name, &meta) != ESP_OK) {
        fclose(fd);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File entry missing");
        return ESP_FAIL;
    }

    size_t payload_size = (size_t)file_stat.st_size;
    ESP_LOGI(TAG, "Sending file : %s (%zu bytes)...", filename, payload_size);
    set_content_type_from_file(req, filename);

    size_t remaining = payload_size;
    while (remaining > 0) {
        size_t to_read = remaining > SCRATCH_BUFSIZE ? SCRATCH_BUFSIZE : remaining;
        size_t chunksize = fread(s_http_scratch, 1, to_read, fd);
        if (chunksize == 0) {
            break;
        }
        if (httpd_resp_send_chunk(req, s_http_scratch, chunksize) != ESP_OK) {
            fclose(fd);
            ESP_LOGE(TAG, "File sending failed!");
            httpd_resp_sendstr_chunk(req, NULL);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to send file");
            return ESP_FAIL;
        }
        remaining -= chunksize;
    }

    /* Close file after sending complete */
    fclose(fd);
    ESP_LOGI(TAG, "File sending complete");

    /* Respond with an empty chunk to signal HTTP response completion */
#ifdef CONFIG_EXAMPLE_HTTPD_CONN_CLOSE_HEADER
    httpd_resp_set_hdr(req, "Connection", "close");
#endif
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t sounds_file_get_handler(httpd_req_t *req)
{
    esp_err_t auth_err = security_require_auth(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }

    /* Only allow /sounds/<flatname> */
    static const char *k_prefix = "/sounds/";
    size_t k_prefix_len = strlen(k_prefix);

    if (req->uri[0] == '\0' || strncmp(req->uri, k_prefix, k_prefix_len) != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_FAIL;
    }

    const char *suburi = req->uri + strlen(k_prefix) - 1; // "/<name>"
    if (!is_flat_name_uri(suburi)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_FAIL;
    }

    char filepath[FILE_PATH_MAX];
    bool delete_requested = false;
    bool rename_requested = false;
    char rename_val[128] = {0};

    const char *filename = get_path_from_uri(filepath, CONFIG_BASE_PATH, suburi, sizeof(filepath));

    if (!filename) {
        ESP_LOGE(TAG, "Filename is too long");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Filename too long");
        return ESP_FAIL;
    }

    const char *base_name = filename;
    const char *slash_in_filename = strrchr(filename, '/');
    if (slash_in_filename && *(slash_in_filename + 1)) {
        base_name = slash_in_filename + 1;
    }

    // Evaluate download, delete or rename
    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len) {
        char *query_str = calloc(1, query_len + 1);
        if (!query_str) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No mem");
            return ESP_FAIL;
        }
        if (httpd_req_get_url_query_str(req, query_str, query_len + 1) == ESP_OK) {
            char delete_val[2];
            if (httpd_query_key_value(query_str, "delete", delete_val, sizeof(delete_val)) == ESP_OK) {
                delete_requested = true;
            }
            if (httpd_query_key_value(query_str, "rename", rename_val, sizeof(rename_val)) == ESP_OK) {
                url_decode_inplace(rename_val);
                rename_requested = true;
            }
        }
        free(query_str);
    }

    // Ensure base file is not .meta file
    if (is_meta_file(base_name)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File does not exist");
        return ESP_FAIL;
    }

    // Handle delete
    if (delete_requested) {
        return sounds_handle_delete(req, filepath, base_name);
    }

    // Handle rename
    if (rename_requested) {
        return sounds_handle_rename(req, filepath, base_name, rename_val);
    }

    // Handle download
    return sounds_handle_download(req, filepath, base_name, filename);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Endpoints
///////////////////////////////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------
// System and Device Endpoints
//-------------------------------------------------------------------------

static void perform_restart(void)
{
#if CONFIG_NETWORK_ETH_OPENETH
    esp_restart_noos_dig();
#else
    esp_restart();
#endif
}

static void restart_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    ESP_LOGW(TAG, "Restarting...");
    perform_restart();
}

static void schedule_restart_timer(const char *timer_name)
{
    esp_err_t mdns_err = mdns_stop_service();
    if (mdns_err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Failed to stop mDNS before %s: %s",
                 timer_name ? timer_name : "restart",
                 esp_err_to_name(mdns_err));
    }

    TimerHandle_t timer = xTimerCreate(timer_name, pdMS_TO_TICKS(500), pdFALSE, NULL, restart_timer_cb);
    if (!timer) {
        ESP_LOGE(TAG, "Failed to create %s timer; restarting immediately", timer_name ? timer_name : "restart");
        perform_restart();
        return;
    }
    if (xTimerStart(timer, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start %s timer; restarting immediately", timer_name ? timer_name : "restart");
        perform_restart();
    }
}

static esp_err_t restart_handler(httpd_req_t *req)
{
    esp_err_t auth_err = security_require_auth(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, "Restarting");

    schedule_restart_timer("restart");
    return ESP_OK;
}

static esp_err_t reset_settings_handler(httpd_req_t *req)
{
    esp_err_t auth_err = security_require_auth(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }

    ESP_LOGW(TAG, "Resetting configured settings to defaults from main app");

    esp_err_t err = network_reset_config_to_defaults(false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reset network settings: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to reset network settings");
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

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    security_clear_auth_cookie_header(req);
    httpd_resp_sendstr(req, "Settings reset to defaults");

    schedule_restart_timer("settings-reset");
    return ESP_OK;
}

static esp_err_t gpio_state_handler(httpd_req_t *req)
{
    esp_err_t auth_err = security_require_auth(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }

    int laser_level = gpio_get_laser_level();
    int hall_level = gpio_get_hall_level();
    bool laser_blocked = laser_level == LASER_BLOCKED;
    bool lid_open = hall_level != HALL_LID_CLOSED;
    const uint64_t uptime_ms = (uint64_t)esp_timer_get_time() / 1000ULL;
    gpio_laser_event_t laser_events[GPIO_EVENT_BUFFER_CAPACITY];
    gpio_hall_event_t hall_events[GPIO_EVENT_BUFFER_CAPACITY];
    uint32_t laser_events_dropped = 0;
    uint32_t hall_events_dropped = 0;
    size_t laser_event_count = gpio_laser_events_drain(
        laser_events,
        GPIO_EVENT_BUFFER_CAPACITY,
        &laser_events_dropped);
    size_t hall_event_count = gpio_hall_events_drain(
        hall_events,
        GPIO_EVENT_BUFFER_CAPACITY,
        &hall_events_dropped);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char chunk[256];
    int len = snprintf(chunk, sizeof(chunk),
                       "{\"uptime_ms\":%llu,"
                       "\"laser\":{\"level\":%d,\"laser_blocked\":%s,\"changes\":%u,\"events\":[",
                       (unsigned long long)uptime_ms,
                       laser_level,
                       laser_blocked ? "true" : "false",
                       (unsigned)gpio_get_laser_changes());
    if (len < 0 || len >= (int)sizeof(chunk)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Render failed");
        return ESP_FAIL;
    }
    if (httpd_resp_sendstr_chunk(req, chunk) != ESP_OK) {
        return ESP_FAIL;
    }

    for (size_t i = 0; i < laser_event_count; ++i) {
        const gpio_laser_event_t *evt = &laser_events[i];
        len = snprintf(chunk, sizeof(chunk),
                       "%s{\"t_ms\":%llu,\"level\":%u}",
                       (i == 0) ? "" : ",",
                       (unsigned long long)(evt->timestamp_us / 1000ULL),
                       (unsigned)evt->level);
        if (len < 0 || len >= (int)sizeof(chunk)) {
            httpd_resp_sendstr_chunk(req, NULL);
            return ESP_FAIL;
        }
        if (httpd_resp_sendstr_chunk(req, chunk) != ESP_OK) {
            httpd_resp_sendstr_chunk(req, NULL);
            return ESP_FAIL;
        }
    }

    len = snprintf(chunk, sizeof(chunk),
                   "],\"events_dropped\":%u},"
                   "\"hall\":{\"level\":%d,\"lid_open\":%s,\"changes\":%u,\"events\":[",
                   (unsigned)laser_events_dropped,
                   hall_level,
                   lid_open ? "true" : "false",
                   (unsigned)get_hall_changes());
    if (len < 0 || len >= (int)sizeof(chunk)) {
        httpd_resp_sendstr_chunk(req, NULL);
        return ESP_FAIL;
    }
    if (httpd_resp_sendstr_chunk(req, chunk) != ESP_OK) {
        httpd_resp_sendstr_chunk(req, NULL);
        return ESP_FAIL;
    }

    for (size_t i = 0; i < hall_event_count; ++i) {
        const gpio_hall_event_t *evt = &hall_events[i];
        len = snprintf(chunk, sizeof(chunk),
                       "%s{\"t_ms\":%llu,\"level\":%u}",
                       (i == 0) ? "" : ",",
                       (unsigned long long)(evt->timestamp_us / 1000ULL),
                       (unsigned)evt->level);
        if (len < 0 || len >= (int)sizeof(chunk)) {
            httpd_resp_sendstr_chunk(req, NULL);
            return ESP_FAIL;
        }
        if (httpd_resp_sendstr_chunk(req, chunk) != ESP_OK) {
            httpd_resp_sendstr_chunk(req, NULL);
            return ESP_FAIL;
        }
    }

    len = snprintf(chunk, sizeof(chunk), "],\"events_dropped\":%u}}", (unsigned)hall_events_dropped);
    if (len < 0 || len >= (int)sizeof(chunk)) {
        httpd_resp_sendstr_chunk(req, NULL);
        return ESP_FAIL;
    }
    if (httpd_resp_sendstr_chunk(req, chunk) != ESP_OK) {
        httpd_resp_sendstr_chunk(req, NULL);
        return ESP_FAIL;
    }
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

#if CONFIG_TEST_GPIO_INJECTION
typedef esp_err_t (*test_gpio_setter_fn_t)(int level);
typedef int (*test_gpio_getter_fn_t)(void);

static esp_err_t test_gpio_send_level_json(httpd_req_t *req, const char *name, int level)
{
    if (!name) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Render failed");
        return ESP_FAIL;
    }

    char resp[96];
    int len = snprintf(resp, sizeof(resp), "{\"name\":\"%s\",\"level\":%d}", name, level ? 1 : 0);
    if (len < 0 || len >= (int)sizeof(resp)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Render failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

static esp_err_t test_gpio_parse_level_query(httpd_req_t *req, int *out_level)
{
    if (!out_level) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Missing output");
        return ESP_FAIL;
    }

    char query[64] = {0};
    int query_len = httpd_req_get_url_query_len(req);
    if (query_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing level");
        return ESP_FAIL;
    }
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

static esp_err_t test_gpio_handle_get(httpd_req_t *req, const char *name, test_gpio_getter_fn_t getter)
{
    esp_err_t auth_err = security_require_auth(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }
    if (!getter) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Handler unavailable");
        return ESP_FAIL;
    }
    return test_gpio_send_level_json(req, name, getter());
}

static esp_err_t test_gpio_handle_post(
    httpd_req_t *req,
    const char *name,
    test_gpio_setter_fn_t setter,
    test_gpio_getter_fn_t getter)
{
    esp_err_t auth_err = security_require_auth(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }
    if (!setter || !getter) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Handler unavailable");
        return ESP_FAIL;
    }

    int level = 0;
    if (test_gpio_parse_level_query(req, &level) != ESP_OK) {
        return ESP_FAIL;
    }

    esp_err_t err = setter(level);
    if (err != ESP_OK) {
        if (err == ESP_ERR_INVALID_ARG) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid level");
        } else {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "GPIO update failed");
        }
        return ESP_FAIL;
    }

    return test_gpio_send_level_json(req, name, getter());
}

static esp_err_t test_gpio_laser_get_handler(httpd_req_t *req)
{
    return test_gpio_handle_get(req, "laser", gpio_get_laser_level);
}

static esp_err_t test_gpio_laser_post_handler(httpd_req_t *req)
{
    return test_gpio_handle_post(req, "laser", gpio_test_set_laser_level, gpio_get_laser_level);
}

static esp_err_t test_gpio_hall_get_handler(httpd_req_t *req)
{
    return test_gpio_handle_get(req, "hall", gpio_get_hall_level);
}

static esp_err_t test_gpio_hall_post_handler(httpd_req_t *req)
{
    return test_gpio_handle_post(req, "hall", gpio_test_set_hall_level, gpio_get_hall_level);
}

static esp_err_t test_gpio_amp_mute_get_handler(httpd_req_t *req)
{
    return test_gpio_handle_get(req, "amp_mute", audio_board_test_get_amp_mute_level);
}

static esp_err_t test_gpio_amp_mute_post_handler(httpd_req_t *req)
{
    return test_gpio_handle_post(
        req,
        "amp_mute",
        audio_board_test_set_amp_mute_level,
        audio_board_test_get_amp_mute_level);
}

static esp_err_t test_gpio_dac_mute_get_handler(httpd_req_t *req)
{
    return test_gpio_handle_get(req, "dac_mute", audio_board_test_get_dac_mute_level);
}

static esp_err_t test_gpio_dac_mute_post_handler(httpd_req_t *req)
{
    return test_gpio_handle_post(
        req,
        "dac_mute",
        audio_board_test_set_dac_mute_level,
        audio_board_test_get_dac_mute_level);
}
#endif

//-------------------------------------------------------------------------
// Audio Endpoints
//-------------------------------------------------------------------------

static esp_err_t audio_test_send_state(httpd_req_t *req)
{
    char resp[224];
    float freq = audio_test_current_freq();
    uint16_t amp = audio_test_current_amplitude();
    uint16_t amp_max = audio_test_max_amplitude();
    bool running = audio_test_is_running();
    bool sweep_running = audio_test_sweep_is_running();
    bool playback_active = audio_is_playing();
    int len = snprintf(resp, sizeof(resp),
                       "{\"running\":%s,\"freq_hz\":%.1f,\"min_hz\":%.1f,\"max_hz\":%.1f,"
                       "\"amp\":%u,\"amp_max\":%u,\"volume_pct\":%.1f,"
                       "\"playback_active\":%s,\"sweep_running\":%s}",
                       running ? "true" : "false",
                       (double)freq,
                       (double)AUDIO_TEST_MIN_HZ,
                       (double)AUDIO_TEST_MAX_HZ,
                       (unsigned)amp,
                       (unsigned)amp_max,
                       amp_max ? (100.0 * (double)amp / (double)amp_max) : 0.0,
                       playback_active ? "true" : "false",
                       sweep_running ? "true" : "false");
    if (len < 0 || len >= (int)sizeof(resp)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Render failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

static esp_err_t audio_test_get_handler(httpd_req_t *req)
{
    esp_err_t auth_err = security_require_auth(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }
    return audio_test_send_state(req);
}

static esp_err_t audio_test_set_handler(httpd_req_t *req)
{
    esp_err_t auth_err = security_require_auth(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }

    uint16_t amp = audio_test_current_amplitude();
    uint16_t amp_max = audio_test_max_amplitude();

    char query[96] = {0};
    int query_len = httpd_req_get_url_query_len(req);
    if (query_len > 0) {
        if (query_len >= (int)sizeof(query)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Query too long");
            return ESP_FAIL;
        }
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad query");
            return ESP_FAIL;
        }
    }

    char action[8] = {0};
    if (httpd_query_key_value(query, "action", action, sizeof(action)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing action");
        return ESP_FAIL;
    }

    char freq_str[16] = {0};
    char vol_str[16] = {0};
    float new_freq = audio_test_current_freq();
    if (httpd_query_key_value(query, "freq", freq_str, sizeof(freq_str)) == ESP_OK) {
        char *end = NULL;
        float parsed = strtof(freq_str, &end);
        if (!end || end == freq_str || *end != '\0' || !isfinite(parsed) ||
            parsed < AUDIO_TEST_MIN_HZ || parsed > AUDIO_TEST_MAX_HZ) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid freq");
            return ESP_FAIL;
        }
        new_freq = parsed;
    }
    float volume_pct = amp_max ? (100.0f * ((float)amp / (float)amp_max)) : 0.0f;
    if (httpd_query_key_value(query, "volume", vol_str, sizeof(vol_str)) == ESP_OK) {
        char *end = NULL;
        float parsed = strtof(vol_str, &end);
        if (!end || end == vol_str || *end != '\0' || !isfinite(parsed) ||
            parsed < 0.0f || parsed > 100.0f) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid volume");
            return ESP_FAIL;
        }
        volume_pct = parsed;
    }
    uint16_t new_amp = amp_max ? (uint16_t)((volume_pct / 100.0f) * (float)amp_max) : 0;

    if (strcmp(action, "start") == 0) {
        if (audio_is_playing()) {
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            httpd_resp_sendstr(req, "Audio playback in progress");
            return ESP_FAIL;
        }
        if (audio_test_sweep_is_running()) {
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            httpd_resp_sendstr(req, "Sweep in progress");
            return ESP_FAIL;
        }
        audio_test_set_targets(new_freq, new_amp);
        esp_err_t err = audio_test_start(new_freq, new_amp);
        if (err != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to start tone");
            return ESP_FAIL;
        }
    } else if (strcmp(action, "stop") == 0) {
        audio_test_set_targets(new_freq, new_amp);
        audio_stop();
    } else if (strcmp(action, "update") == 0) {
        if (audio_test_sweep_is_running()) {
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            httpd_resp_sendstr(req, "Sweep in progress");
            return ESP_FAIL;
        }
        audio_test_set_targets(new_freq, new_amp);
    } else if (strcmp(action, "sweep") == 0) {
        if (audio_is_playing()) {
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            httpd_resp_sendstr(req, "Audio playback in progress");
            return ESP_FAIL;
        }
        if (audio_test_is_running()) {
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            httpd_resp_sendstr(req, "Test tone in progress");
            return ESP_FAIL;
        }
        audio_test_set_targets(new_freq, new_amp);
        esp_err_t err = audio_test_start_sweep();
        if (err != ESP_OK) {
            if (err == ESP_ERR_INVALID_STATE) {
                httpd_resp_set_status(req, "409 Conflict");
                httpd_resp_set_type(req, "text/plain");
                httpd_resp_set_hdr(req, "Cache-Control", "no-store");
                httpd_resp_sendstr(req, "Sweep already running");
                return ESP_FAIL;
            }
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to start sweep");
            return ESP_FAIL;
        }
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid action");
        return ESP_FAIL;
    }

    return audio_test_send_state(req);
}

static esp_err_t audio_playback_handler(httpd_req_t *req)
{
    esp_err_t auth_err = security_require_auth(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }

    char query[128] = {0};
    if (httpd_req_get_url_query_len(req) > 0) {
        if (httpd_req_get_url_query_len(req) >= (int)sizeof(query)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Query too long");
            return ESP_FAIL;
        }
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad query");
            return ESP_FAIL;
        }
    }

    char action[8] = "start";
    httpd_query_key_value(query, "action", action, sizeof(action));

    if (strcmp(action, "stop") == 0) {
        audio_stop();
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_sendstr(req, "{\"status\":\"stopped\"}");
        return ESP_OK;
    }

    if (audio_test_is_running()) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_sendstr(req, "Audio test in progress");
        return ESP_FAIL;
    }
    if (audio_test_sweep_is_running()) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_sendstr(req, "Sweep in progress");
        return ESP_FAIL;
    }

    // Stop current audio activity before starting MP3 playback
    audio_stop();

    char name[FILE_ENTRY_NAME_MAX] = {0};
    if (httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK || name[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing name");
        return ESP_FAIL;
    }

    if (!is_mp3_only(name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Only MP3 playback is supported right now");
        return ESP_FAIL;
    }

    esp_err_t err = audio_start_file(name);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NOT_FOUND) {
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        } else if (err == ESP_ERR_INVALID_ARG) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
        } else {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to start playback");
        }
        return ESP_FAIL;
    }

    char resp[128];
    int len = snprintf(resp, sizeof(resp), "{\"status\":\"started\",\"name\":\"%s\"}", name);
    if (len < 0 || len >= (int)sizeof(resp)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Render failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

//-------------------------------------------------------------------------
// Storage Endpoints
//-------------------------------------------------------------------------

static esp_err_t format_storage_handler(httpd_req_t *req)
{
    esp_err_t auth_err = security_require_auth(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }

    if (files_format_storage() != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Format failed");
        return ESP_FAIL;
    }
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_sendstr(req, "Formatted");
    return ESP_OK;
}

//-------------------------------------------------------------------------
// Upload Endpoint
//-------------------------------------------------------------------------

static bool discard_upload_body(httpd_req_t *req)
{
    if (!req || req->content_len <= 0) {
        return true;
    }

    int remaining = req->content_len;
    int timeout_retries = 0;
    while (remaining > 0) {
        int received = httpd_req_recv(req, s_http_scratch, MIN(remaining, SCRATCH_BUFSIZE));
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeout_retries >= 10) {
                ESP_LOGW(TAG, "Timed out discarding upload body (remaining=%d)", remaining);
                return false;
            }
            continue;
        }
        if (received <= 0) {
            ESP_LOGW(TAG, "Failed while discarding upload body (received=%d, remaining=%d)",
                     received, remaining);
            return false;
        }
        timeout_retries = 0;
        remaining -= received;
    }

    return true;
}

static esp_err_t send_upload_error(httpd_req_t *req, httpd_err_code_t status, const char *msg, bool discard_body)
{
    bool body_drained = true;
    if (discard_body) {
        body_drained = discard_upload_body(req);
    }

    const char *status_str = NULL;
    switch (status) {
        case HTTPD_400_BAD_REQUEST:
            status_str = "400 Bad Request";
            break;
        case HTTPD_500_INTERNAL_SERVER_ERROR:
            status_str = "500 Internal Server Error";
            break;
        case HTTPD_413_CONTENT_TOO_LARGE:
            status_str = "413 Content Too Large";
            break;
        default:
            break;
    }
    if (status_str) {
        httpd_resp_set_status(req, status_str);
    }
    httpd_resp_set_type(req, "text/plain");
    if (!body_drained) {
        httpd_resp_set_hdr(req, "Connection", "close");
    }
    return httpd_resp_sendstr(req, msg ? msg : "Upload failed");
}

static esp_err_t upload_post_handler(httpd_req_t *req)
{
    esp_err_t auth_err = security_require_auth(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }

    /* Only allow /sounds/<flatname> */
    static const char *k_prefix = "/sounds/";
    size_t k_prefix_len = strlen(k_prefix);

    if (req->uri[0] == '\0' || strncmp(req->uri, k_prefix, k_prefix_len) != 0) {
        return send_upload_error(req, HTTPD_400_BAD_REQUEST, "Invalid filename", true);
    }

    const char *suburi = req->uri + strlen(k_prefix) - 1; // "/<name>"
    if (!is_flat_name_uri(suburi)) {
        return send_upload_error(req, HTTPD_400_BAD_REQUEST, "Invalid filename", true);
    }

    char filepath[FILE_PATH_MAX];
    FILE *fd = NULL;
    struct stat file_stat;

    const char *filename = get_path_from_uri(filepath, CONFIG_BASE_PATH, suburi, sizeof(filepath));

    if (!filename) {
        /* Respond with 500 Internal Server Error */
        return send_upload_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Filename too long", true);
    }

    size_t filename_len = strlen(filename);

    /* Filename must exist and cannot have a trailing '/' */
    if (filename_len == 0 || filename[filename_len - 1] == '/') {
        ESP_LOGE(TAG, "Invalid filename : %s", filename);
        return send_upload_error(req, HTTPD_400_BAD_REQUEST, "Invalid filename", true);
    }

    const char *base_name = filename;
    const char *slash_pos = strrchr(filename, '/');
    if (slash_pos && *(slash_pos + 1)) {
        base_name = slash_pos + 1;
    }

    if (!is_audio_filename(base_name)) {
        ESP_LOGE(TAG, "Rejected non-audio upload : %s", base_name);
        return send_upload_error(req, HTTPD_400_BAD_REQUEST,
                                 "Only audio files are allowed (" ALLOWED_AUDIO_EXTS_LIST ")", true);
    }

    if (files_is_default_sound_name(base_name)) {
        char msg[160];
        build_default_sound_reserved_message(msg, sizeof(msg));
        ESP_LOGE(TAG, "Rejected reserved upload filename : %s", base_name);
        return send_upload_error(req, HTTPD_400_BAD_REQUEST, msg, true);
    }

    if (stat(filepath, &file_stat) == 0) {
        ESP_LOGE(TAG, "File already exists : %s", filepath);
        /* Respond with 400 Bad Request */
        return send_upload_error(req, HTTPD_400_BAD_REQUEST, "File already exists", true);
    }

    if (files_count() >= FILES_MAX_ENTRIES) {
        ESP_LOGE(TAG, "Max file entries reached");
        return send_upload_error(req, HTTPD_400_BAD_REQUEST, "File entry limit reached", true);
    }

    /* File cannot be larger than a limit */
    if (req->content_len > MAX_FILE_SIZE) {
        ESP_LOGE(TAG, "File too large : %d bytes", req->content_len);
        /* Respond with 413 and consume body for deterministic client behavior. */
        return send_upload_error(req, HTTPD_413_CONTENT_TOO_LARGE,
                                 "File size must be less than " MAX_FILE_SIZE_STR "!", true);
    }

    // Proactive storage check: fail early if LittleFS has insufficient space.
    size_t total_bytes = 0;
    size_t used_bytes = 0;
    esp_err_t info_err = esp_littlefs_info(STORAGE_PARTITION_LABEL, &total_bytes, &used_bytes);
    if (info_err == ESP_OK) {
        if (used_bytes > total_bytes) {
            used_bytes = total_bytes;
        }

        size_t free_bytes = total_bytes - used_bytes;
        size_t required_bytes = (size_t)req->content_len + UPLOAD_STORAGE_OVERHEAD_BYTES;
        if (required_bytes > free_bytes) {
            ESP_LOGE(TAG, "Insufficient storage for upload: need=%u free=%u file=%d overhead=%u",
                     (unsigned)required_bytes,
                     (unsigned)free_bytes,
                     req->content_len,
                     (unsigned)UPLOAD_STORAGE_OVERHEAD_BYTES);

            char msg[160];
            int n = snprintf(msg, sizeof(msg),
                             "Not enough storage space. Need %u bytes, only %u bytes free.",
                             (unsigned)required_bytes,
                             (unsigned)free_bytes);
            if (n < 0 || n >= (int)sizeof(msg)) {
                strlcpy(msg, "Not enough storage space for upload.", sizeof(msg));
            }
            bool drained = discard_upload_body(req);
            httpd_resp_set_status(req, "507 Insufficient Storage");
            httpd_resp_set_type(req, "text/plain");
            if (!drained) {
                httpd_resp_set_hdr(req, "Connection", "close");
            }
            return httpd_resp_sendstr(req, msg);
        }
    } else {
        // Keep legacy behavior if storage stats are temporarily unavailable.
        ESP_LOGW(TAG, "Could not query storage info before upload: %s", esp_err_to_name(info_err));
    }

    fd = fopen(filepath, "wb");
    if (!fd) {
        ESP_LOGE(TAG, "Failed to create file : %s", filepath);
        /* Respond with 500 Internal Server Error */
        return send_upload_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create file", false);
    }

    size_t bytes_written = 0;

    ESP_LOGI(TAG, "Receiving file : %s...", filename);

    int received;

    /* Content length of the request gives
     * the size of the file being uploaded */
    int remaining = req->content_len;

    while (remaining > 0) {
        ESP_LOGD(TAG, "Remaining size : %d", remaining);
        /* Receive the file part by part into a buffer */
        if ((received = httpd_req_recv(req, s_http_scratch, MIN(remaining, SCRATCH_BUFSIZE))) <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                /* Retry if timeout occurred */
                continue;
            }

            /* In case of unrecoverable error,
             * close and delete the unfinished file*/
            fclose(fd);
            unlink(filepath);

            ESP_LOGE(TAG, "File reception failed!");
            /* Respond with 500 Internal Server Error */
            return send_upload_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive file", false);
        }

        /* Write buffer content to file on storage */
        if (received && (received != fwrite(s_http_scratch, 1, received, fd))) {
            /* Couldn't write everything to file!
             * Storage may be full? */
            fclose(fd);
            unlink(filepath);

            ESP_LOGE(TAG, "File write failed!");
            /* Respond with 500 Internal Server Error */
            return send_upload_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to write file to storage", false);
        }
        bytes_written += (size_t)received;

        /* Keep track of remaining size of
         * the file left to be uploaded */
        remaining -= received;
    }

    /* Close file upon upload completion */
    fclose(fd);
    ESP_LOGI(TAG, "File reception complete (%zu bytes)", bytes_written);

    file_properties_t meta;
    files_props_init(&meta, base_name);
    files_props_set(&meta, base_name, 100, 100, true);
    if (files_write_meta(base_name, &meta) != ESP_OK) {
        unlink(filepath);
        ESP_LOGE(TAG, "Failed to write meta for %s", base_name);
        return send_upload_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to write metadata", false);
    }

    /* Redirect onto sounds/ to see the updated file list */
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/sounds/");
#ifdef CONFIG_EXAMPLE_HTTPD_CONN_CLOSE_HEADER
    httpd_resp_set_hdr(req, "Connection", "close");
#endif
    httpd_resp_sendstr(req, "File uploaded successfully");
    return ESP_OK;
}

//-------------------------------------------------------------------------
// File Metadata Endpoint
//-------------------------------------------------------------------------

static esp_err_t file_meta_handler(httpd_req_t *req)
{
    esp_err_t auth_err = security_require_auth(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }

    char filepath[FILE_PATH_MAX];
    struct stat st;

    /* Expect: /sounds/file-meta/<name> */
    const char *suffix = req->uri + sizeof("/sounds/file-meta") - 1;
    if (!suffix || suffix[0] != '/' || suffix[1] == '\0' || strchr(suffix + 1, '/')) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
        return ESP_FAIL;
    }

    const char *filename = get_path_from_uri(
        filepath,
        CONFIG_BASE_PATH,
        suffix,
        sizeof(filepath)
    );

    if (!filename) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Filename too long");
        return ESP_FAIL;
    }

    size_t filename_len = strlen(filename);
    if (filename_len == 0 || filename[filename_len - 1] == '/') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
        return ESP_FAIL;
    }

    const char *base_name = filename;
    const char *slash_in_filename = strrchr(filename, '/');
    if (slash_in_filename && *(slash_in_filename + 1)) {
        base_name = slash_in_filename + 1;
    }
    if (is_meta_file(base_name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
        return ESP_FAIL;
    }

    if (stat(filepath, &st) == -1) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File does not exist");
        return ESP_FAIL;
    }

    file_properties_t meta;
    if (files_read_meta(base_name, &meta) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File entry missing");
        return ESP_FAIL;
    }

    if (req->method == HTTP_GET) {
        char buf[128];
        int n = snprintf(buf, sizeof(buf),
                         "{\"probability\":%u,\"volume\":%u,\"enabled\":%s}\n",
                         (unsigned)meta.probability,
                         (unsigned)meta.volume,
                         meta.enabled ? "true" : "false");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, buf, n);
        return ESP_OK;
    }

    if (req->method == HTTP_POST) {
        // small JSON body expected
        int len = req->content_len;
        if (len <= 0 || len > 256) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON size");
            return ESP_FAIL;
        }

        char body[257];
        int received = 0;
        while (received < len) {
            int r = httpd_req_recv(req, body + received, len - received);
            if (r <= 0) {
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read body");
                return ESP_FAIL;
            }
            received += r;
        }
        body[len] = '\0';

        int p = (int)meta.probability;
        int v = (int)meta.volume;
        bool e = meta.enabled;

        bool p_present = json_has_key(body, "probability");
        bool v_present = json_has_key(body, "volume");
        bool e_present = json_has_key(body, "enabled");

        // keys must match what the browser sends
        if (p_present && !json_get_int(body, "probability", &p)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid probability");
            return ESP_FAIL;
        }
        if (v_present && !json_get_int(body, "volume", &v)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid volume");
            return ESP_FAIL;
        }
        if (e_present && !json_get_bool(body, "enabled", &e)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid enabled");
            return ESP_FAIL;
        }

        if (p < 0 || p > FILE_PROBABILITY_MAX) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Probability out of range");
            return ESP_FAIL;
        }
        if (v < 0 || v > FILE_VOLUME_MAX) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Volume out of range");
            return ESP_FAIL;
        }

        files_props_set(&meta, NULL, (uint8_t)p, (uint8_t)v, e);
        if (files_write_meta(base_name, &meta) != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save properties");
            return ESP_FAIL;
        }
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "OK");
        return ESP_OK;
    }

    httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "Method not allowed");
    return ESP_FAIL;
}

//-------------------------------------------------------------------------
// Redirect Endpoint
//-------------------------------------------------------------------------

static esp_err_t redirect_to_sounds_handler(httpd_req_t *req)
{
    if (s_ui_password_set && !security_is_authenticated_request(req)) {
        return security_redirect_to_login_for_path(req, "/sounds/");
    }

    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/sounds/");
#ifdef CONFIG_EXAMPLE_HTTPD_CONN_CLOSE_HEADER
    httpd_resp_set_hdr(req, "Connection", "close");
#endif
    httpd_resp_sendstr(req, "Redirecting to /sounds/");
    return ESP_OK;
}

static void maybe_play_startup_sound(void)
{
    if (!s_boot_sound_enabled) {
        return;
    }

    esp_err_t err = audio_start_file(FILES_DEFAULT_SOUND_NAME);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Failed to start startup sound %s: %s",
                 FILES_DEFAULT_SOUND_NAME,
                 esp_err_to_name(err));
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Entry
///////////////////////////////////////////////////////////////////////////////////////////////////

esp_err_t start_mainapp(void)
{
    esp_err_t sec_err = security_init();
    if (sec_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize security state: %s", esp_err_to_name(sec_err));
        return sec_err;
    }

    esp_err_t boot_cfg_err = boot_config_load_from_nvs();
    if (boot_cfg_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize start-up settings: %s", esp_err_to_name(boot_cfg_err));
        return boot_cfg_err;
    }

    ESP_LOGI(TAG, "UI password lock: %s", s_ui_password_set ? "enabled" : "disabled");
    ESP_LOGI(TAG, "Startup sound: %s", s_boot_sound_enabled ? "enabled" : "disabled");

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    /* Directory listings plus metadata lookups use a bit more stack now that
     * payloads and metadata are separate; give the HTTPD task extra room. */
    config.stack_size = 8192;
    config.max_uri_handlers = 44;

    /* Use the URI wildcard matching function in order to
     * allow the same handler to respond to multiple different
     * target URIs which match the wildcard scheme */
    config.uri_match_fn = httpd_uri_match_wildcard;

    ESP_LOGI(TAG, "Starting HTTP Server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start file server!");
        return ESP_FAIL;
    }

    httpd_uri_t login_page = {
        .uri = "/login",
        .method = HTTP_GET,
        .handler = http_resp_login_html,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &login_page);

    httpd_uri_t auth_login = {
        .uri = "/auth/login",
        .method = HTTP_POST,
        .handler = auth_login_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &auth_login);

    httpd_uri_t root_redirect = {
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = redirect_to_sounds_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &root_redirect);

    httpd_uri_t sounds_redirect = {
        .uri       = "/sounds",
        .method    = HTTP_GET,
        .handler   = redirect_to_sounds_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &sounds_redirect);

    httpd_uri_t skip_redirect = {
        .uri       = "/skip",
        .method    = HTTP_GET,
        .handler   = redirect_to_sounds_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &skip_redirect);

    httpd_uri_t recovery_redirect = {
        .uri       = "/recovery",
        .method    = HTTP_GET,
        .handler   = redirect_to_sounds_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &recovery_redirect);

    httpd_uri_t sounds_index = {
        .uri       = "/sounds/",
        .method    = HTTP_GET,
        .handler   = sounds_index_get_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &sounds_index);

    httpd_uri_t navbar_js = {
        .uri       = "/navbar.js",
        .method    = HTTP_GET,
        .handler   = http_resp_navbar_js,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &navbar_js);

    httpd_uri_t connection_monitor_js = {
        .uri       = "/connection_monitor.js",
        .method    = HTTP_GET,
        .handler   = http_resp_connection_monitor_js,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &connection_monitor_js);

    httpd_uri_t glyphs_js = {
        .uri       = "/glyphs.js",
        .method    = HTTP_GET,
        .handler   = http_resp_glyphs_js,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &glyphs_js);

    httpd_uri_t glyphs_css = {
        .uri       = "/glyphs.css",
        .method    = HTTP_GET,
        .handler   = http_resp_glyphs_css,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &glyphs_css);

    httpd_uri_t ota_update = {
        .uri = "/update",
        .method = HTTP_POST,
        .handler = ota_update_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &ota_update);

    httpd_uri_t logs_get = {
        .uri = "/logs",
        .method = HTTP_GET,
        .handler = http_resp_logs,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &logs_get);

    httpd_uri_t network_ips_uri = {
        .uri = "/network/ips",
        .method = HTTP_GET,
        .handler = network_ips_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &network_ips_uri);

    httpd_uri_t network_config_get_uri = {
        .uri = "/network/config",
        .method = HTTP_GET,
        .handler = network_config_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &network_config_get_uri);

    httpd_uri_t network_config_post_uri = {
        .uri = "/network/config",
        .method = HTTP_POST,
        .handler = network_config_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &network_config_post_uri);

    httpd_uri_t security_config_get_uri = {
        .uri = "/security/config",
        .method = HTTP_GET,
        .handler = security_config_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &security_config_get_uri);

    httpd_uri_t security_config_post_uri = {
        .uri = "/security/config",
        .method = HTTP_POST,
        .handler = security_config_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &security_config_post_uri);

    httpd_uri_t boot_config_get_uri = {
        .uri = "/boot/config",
        .method = HTTP_GET,
        .handler = boot_config_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &boot_config_get_uri);

    httpd_uri_t boot_config_post_uri = {
        .uri = "/boot/config",
        .method = HTTP_POST,
        .handler = boot_config_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &boot_config_post_uri);

    httpd_uri_t gpio_state_uri = {
        .uri = "/gpio/state",
        .method = HTTP_GET,
        .handler = gpio_state_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &gpio_state_uri);

#if CONFIG_TEST_GPIO_INJECTION
    httpd_uri_t test_gpio_laser_get_uri = {
        .uri = "/test/gpio/laser",
        .method = HTTP_GET,
        .handler = test_gpio_laser_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &test_gpio_laser_get_uri);

    httpd_uri_t test_gpio_laser_post_uri = {
        .uri = "/test/gpio/laser",
        .method = HTTP_POST,
        .handler = test_gpio_laser_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &test_gpio_laser_post_uri);

    httpd_uri_t test_gpio_hall_get_uri = {
        .uri = "/test/gpio/hall",
        .method = HTTP_GET,
        .handler = test_gpio_hall_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &test_gpio_hall_get_uri);

    httpd_uri_t test_gpio_hall_post_uri = {
        .uri = "/test/gpio/hall",
        .method = HTTP_POST,
        .handler = test_gpio_hall_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &test_gpio_hall_post_uri);

    httpd_uri_t test_gpio_amp_mute_get_uri = {
        .uri = "/test/gpio/amp-mute",
        .method = HTTP_GET,
        .handler = test_gpio_amp_mute_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &test_gpio_amp_mute_get_uri);

    httpd_uri_t test_gpio_amp_mute_post_uri = {
        .uri = "/test/gpio/amp-mute",
        .method = HTTP_POST,
        .handler = test_gpio_amp_mute_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &test_gpio_amp_mute_post_uri);

    httpd_uri_t test_gpio_dac_mute_get_uri = {
        .uri = "/test/gpio/dac-mute",
        .method = HTTP_GET,
        .handler = test_gpio_dac_mute_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &test_gpio_dac_mute_get_uri);

    httpd_uri_t test_gpio_dac_mute_post_uri = {
        .uri = "/test/gpio/dac-mute",
        .method = HTTP_POST,
        .handler = test_gpio_dac_mute_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &test_gpio_dac_mute_post_uri);
#endif

    httpd_uri_t audio_test_get_uri = {
        .uri = "/audio/test",
        .method = HTTP_GET,
        .handler = audio_test_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &audio_test_get_uri);

    httpd_uri_t audio_test_set_uri = {
        .uri = "/audio/test",
        .method = HTTP_POST,
        .handler = audio_test_set_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &audio_test_set_uri);

    httpd_uri_t audio_playback_uri = {
        .uri = "/audio/playback",
        .method = HTTP_POST,
        .handler = audio_playback_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &audio_playback_uri);

    httpd_uri_t settings_page = {
        .uri = "/settings",
        .method = HTTP_GET,
        .handler = http_resp_settings_html,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &settings_page);

    httpd_uri_t settings_reset_uri = {
        .uri = "/settings/reset",
        .method = HTTP_POST,
        .handler = reset_settings_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &settings_reset_uri);

    httpd_uri_t format_storage_uri = {
        .uri = "/format",
        .method = HTTP_POST,
        .handler = format_storage_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &format_storage_uri);

    httpd_uri_t restart_uri = {
        .uri = "/restart",
        .method = HTTP_POST,
        .handler = restart_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &restart_uri);

    httpd_uri_t file_meta = {
        .uri = "/sounds/file-meta/*",
        .method = HTTP_ANY,
        .handler = file_meta_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &file_meta);

    httpd_uri_t favicon = {
        .uri       = "/favicon.ico",
        .method    = HTTP_GET,
        .handler   = favicon_get_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &favicon);

    /* URI handler for getting uploaded files */
    httpd_uri_t file_download = {
        .uri       = "/sounds/*",
        .method    = HTTP_GET,
        .handler   = sounds_file_get_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &file_download);

    /* URI handler for uploading files to server (same path space as downloads) */
    httpd_uri_t file_upload = {
        .uri       = "/sounds/*",
        .method    = HTTP_POST,
        .handler   = upload_post_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &file_upload);

    maybe_play_startup_sound();

    return ESP_OK;
}
