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
