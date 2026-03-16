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

#include "unity.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_mac.h"
#include "recovery_code.h"
#include "recovery_code_vectors.h"

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

TEST_CASE("recovery_code matches python script vectors", "[recovery]")
{
    for (size_t i = 0; i < sizeof(RECOVERY_CODE_SCRIPT_VECTORS) / sizeof(RECOVERY_CODE_SCRIPT_VECTORS[0]); ++i) {
        const recovery_code_script_vector_t *vector = &RECOVERY_CODE_SCRIPT_VECTORS[i];
        int actual = -1;
        TEST_ASSERT_EQUAL(ESP_OK, recovery_code_generate_for_mac(vector->mac, &actual));
        TEST_ASSERT_EQUAL_INT(vector->code, actual);
        TEST_ASSERT_FALSE(is_blacklisted_code(actual));
    }
}

TEST_CASE("recovery_code_check accepts the local device code and rejects a wrong one", "[recovery]")
{
    uint8_t mac[RECOVERY_CODE_MAC_LEN] = {0};
    TEST_ASSERT_EQUAL(ESP_OK, esp_efuse_mac_get_default(mac));

    int expected = -1;
    TEST_ASSERT_EQUAL(ESP_OK, recovery_code_generate_for_mac(mac, &expected));
    TEST_ASSERT_TRUE(recovery_code_check(expected));
    TEST_ASSERT_FALSE(recovery_code_check(-1));
    TEST_ASSERT_FALSE(recovery_code_check(10000));
    TEST_ASSERT_FALSE(recovery_code_check((expected + 1) % 10000));
}

TEST_CASE("recovery_code rejects invalid arguments", "[recovery]")
{
    uint8_t mac[RECOVERY_CODE_MAC_LEN] = {0};
    int code = -1;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, recovery_code_generate_for_mac(NULL, &code));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, recovery_code_generate_for_mac(mac, NULL));
}
