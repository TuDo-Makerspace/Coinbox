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

#include "recovery_code.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_mac.h"
#include "mbedtls/md.h"

#ifndef COINBOX_RECOVERY_SEED
#define COINBOX_RECOVERY_SEED "test"
#endif

///////////////////////////////////////////////////////////////////////////////////////////////////
// Constants
///////////////////////////////////////////////////////////////////////////////////////////////////

#define RECOVERY_CODE_ATTEMPTS_MAX 256
#define RECOVERY_CODE_BLACKLIST_ASC "1234"
#define RECOVERY_CODE_BLACKLIST_DESC "4321"

///////////////////////////////////////////////////////////////////////////////////////////////////
// Private Helpers
///////////////////////////////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------
// Validation
//-------------------------------------------------------------------------

static bool is_blacklisted_code(int code)
{
    if (code == 1234 || code == 4321) {
        return true;
    }

    int thousands = (code / 1000) % 10;
    int hundreds = (code / 100) % 10;
    int tens = (code / 10) % 10;
    int ones = code % 10;
    return thousands == hundreds && hundreds == tens && tens == ones;
}

static uint32_t be32_from_bytes(const uint8_t bytes[4])
{
    return ((uint32_t)bytes[0] << 24) |
           ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) |
           (uint32_t)bytes[3];
}

//-------------------------------------------------------------------------
// HMAC Derivation
//-------------------------------------------------------------------------

static esp_err_t candidate_digest(const uint8_t mac[RECOVERY_CODE_MAC_LEN], uint32_t attempt, uint8_t out_digest[32])
{
    if (!mac || !out_digest) {
        return ESP_ERR_INVALID_ARG;
    }

    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md) {
        return ESP_FAIL;
    }

    uint8_t message[RECOVERY_CODE_MAC_LEN + 1 + sizeof(uint32_t)];
    size_t message_len = RECOVERY_CODE_MAC_LEN;
    memcpy(message, mac, RECOVERY_CODE_MAC_LEN);

    if (attempt > 0) {
        message[message_len++] = (uint8_t)'#';
        message[message_len++] = (uint8_t)((attempt >> 24) & 0xFF);
        message[message_len++] = (uint8_t)((attempt >> 16) & 0xFF);
        message[message_len++] = (uint8_t)((attempt >> 8) & 0xFF);
        message[message_len++] = (uint8_t)(attempt & 0xFF);
    }

    const char *seed = COINBOX_RECOVERY_SEED;
    int rc = mbedtls_md_hmac(md,
                             (const unsigned char *)seed,
                             strlen(seed),
                             message,
                             message_len,
                             out_digest);
    return (rc == 0) ? ESP_OK : ESP_FAIL;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Public Interface
///////////////////////////////////////////////////////////////////////////////////////////////////

esp_err_t recovery_code_generate_for_mac(const uint8_t mac[RECOVERY_CODE_MAC_LEN], int *out_code)
{
    if (!mac || !out_code) {
        return ESP_ERR_INVALID_ARG;
    }

    for (uint32_t attempt = 0; attempt < RECOVERY_CODE_ATTEMPTS_MAX; ++attempt) {
        uint8_t digest[32];
        esp_err_t err = candidate_digest(mac, attempt, digest);
        if (err != ESP_OK) {
            return err;
        }

        int code = (int)(be32_from_bytes(digest) % 10000U);
        if (!is_blacklisted_code(code)) {
            *out_code = code;
            return ESP_OK;
        }
    }

    return ESP_FAIL;
}

bool recovery_code_check(int input_code)
{
    if (input_code < 0 || input_code > 9999) {
        return false;
    }

    uint8_t mac[RECOVERY_CODE_MAC_LEN];
    if (esp_efuse_mac_get_default(mac) != ESP_OK) {
        return false;
    }

    int expected_code = 0;
    if (recovery_code_generate_for_mac(mac, &expected_code) != ESP_OK) {
        return false;
    }

    return input_code == expected_code;
}
