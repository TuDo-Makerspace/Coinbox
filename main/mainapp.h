/*
 * SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
/* HTTP File Server Example, common declarations

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#pragma once

#include <stdbool.h>

#include "sdkconfig.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t start_mainapp(void);
esp_err_t mainapp_security_init(void);
bool mainapp_security_is_password_set(void);
bool mainapp_security_password_matches(const char *password);
esp_err_t mainapp_reset_security_defaults(void);
esp_err_t mainapp_reset_boot_defaults(void);
esp_err_t mainapp_reset_audio_defaults(void);

#ifdef __cplusplus
}
#endif
