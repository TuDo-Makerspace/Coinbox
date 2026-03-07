#include "runtime_status.h"

#include <stdio.h>
#include <string.h>

#include "esp_random.h"

///////////////////////////////////////////////////////////////////////////////////////////////////
// Constants
///////////////////////////////////////////////////////////////////////////////////////////////////

#define RUNTIME_BOOT_ID_NUM_BYTES 8
#define RUNTIME_BOOT_ID_HEX_LEN (RUNTIME_BOOT_ID_NUM_BYTES * 2)

///////////////////////////////////////////////////////////////////////////////////////////////////
// Private State
///////////////////////////////////////////////////////////////////////////////////////////////////

static char s_runtime_boot_id[RUNTIME_BOOT_ID_HEX_LEN + 1] = {0};

///////////////////////////////////////////////////////////////////////////////////////////////////
// Private Helpers
///////////////////////////////////////////////////////////////////////////////////////////////////

static void bytes_to_hex(const uint8_t *src, size_t src_len, char *dst, size_t dst_size)
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

static void ensure_boot_id_generated(void)
{
    if (s_runtime_boot_id[0] != '\0') {
        return;
    }

    uint8_t boot_id_bytes[RUNTIME_BOOT_ID_NUM_BYTES];
    for (size_t i = 0; i < sizeof(boot_id_bytes); i += sizeof(uint32_t)) {
        uint32_t r = esp_random();
        size_t remaining = sizeof(boot_id_bytes) - i;
        size_t copy_len = remaining < sizeof(uint32_t) ? remaining : sizeof(uint32_t);
        memcpy(&boot_id_bytes[i], &r, copy_len);
    }

    bytes_to_hex(boot_id_bytes, sizeof(boot_id_bytes), s_runtime_boot_id, sizeof(s_runtime_boot_id));
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Public Interface
///////////////////////////////////////////////////////////////////////////////////////////////////

const char *runtime_status_boot_id(void)
{
    ensure_boot_id_generated();
    return s_runtime_boot_id;
}

esp_err_t runtime_status_send_json(httpd_req_t *req, const char *mode)
{
    if (!req || !mode || mode[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char resp[128];
    int len = snprintf(resp,
                       sizeof(resp),
                       "{\"boot_id\":\"%s\",\"mode\":\"%s\",\"ready\":true}",
                       runtime_status_boot_id(),
                       mode);
    if (len <= 0 || len >= (int)sizeof(resp)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Render failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}
