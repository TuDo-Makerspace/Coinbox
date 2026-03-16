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

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

///////////////////////////////////////////////////////////////////////////////////////////////////
// Constants
///////////////////////////////////////////////////////////////////////////////////////////////////

#define SECURITY_UI_PASSWORD_MAX_LEN 64

///////////////////////////////////////////////////////////////////////////////////////////////////
// Interface
///////////////////////////////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------
// Password State
//-------------------------------------------------------------------------

esp_err_t security_init(void);
bool security_is_password_set(void);
bool security_password_matches(const char *password);
esp_err_t security_set_password(const char *password);
esp_err_t security_clear_password(void);

//-------------------------------------------------------------------------
// Auth Helpers
//-------------------------------------------------------------------------

void security_sanitize_next_path(const char *candidate, char *out, size_t out_size);
void security_set_auth_cookie_header(httpd_req_t *req);
void security_clear_auth_cookie_header(httpd_req_t *req);
bool security_is_authenticated_request(httpd_req_t *req);
esp_err_t security_send_unauthorized(httpd_req_t *req);
esp_err_t security_redirect_to_login_for_path(httpd_req_t *req, const char *next_candidate);
esp_err_t security_redirect_to_login(httpd_req_t *req);
esp_err_t security_require_auth(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
