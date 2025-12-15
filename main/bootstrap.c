#include "bootstrap.h"

#include <stdbool.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "audio.h"
#include "sdkconfig.h"
#include "ws_server.h"
#include "ota.h"

static const char *TAG = "bootstrap";

static httpd_handle_t s_bootstrap_server;
static TimerHandle_t s_recovery_timer;
static bool s_recovery_requested;
static bool s_main_started;
static TaskHandle_t s_start_task;
static char s_base_path[ESP_VFS_PATH_MAX + 1];

static const size_t BOOTSTRAP_PAGE_MAX = 8192;

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

static esp_err_t render_bootstrap_page(char *out, size_t out_size, uint32_t seconds, bool auto_refresh, bool is_recovery)
{
    extern const unsigned char bootstrap_html_start[] asm("_binary_bootstrap_html_start");
    extern const unsigned char bootstrap_html_end[] asm("_binary_bootstrap_html_end");
    const size_t template_len = (size_t)(bootstrap_html_end - bootstrap_html_start);

    if (template_len + 1 > out_size) {
        return ESP_ERR_NO_MEM;
    }

    memcpy(out, bootstrap_html_start, template_len);
    out[template_len] = '\0';

    char seconds_buf[16];
    char refresh_buf[2];
    char recovery_buf[2];
    snprintf(seconds_buf, sizeof(seconds_buf), "%lu", (unsigned long)seconds);
    refresh_buf[0] = auto_refresh ? '1' : '0';
    refresh_buf[1] = '\0';
    recovery_buf[0] = is_recovery ? '1' : '0';
    recovery_buf[1] = '\0';

    if (!replace_placeholder(out, out_size, "{{SECONDS}}", seconds_buf) ||
        !replace_placeholder(out, out_size, "{{AUTO_REFRESH}}", refresh_buf) ||
        !replace_placeholder(out, out_size, "{{IS_RECOVERY}}", recovery_buf)) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void start_main_application() {
    esp_err_t err = start_ws_server(s_base_path);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start main application: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Main application started");
        s_main_started = true;
    }
}

static void cancel_recovery_timer(void)
{
    if (!s_recovery_timer) {
        return;
    }
    xTimerStop(s_recovery_timer, 0);
    xTimerDelete(s_recovery_timer, 0);
    s_recovery_timer = NULL;
}

static void enter_main(void)
{
    if (s_recovery_requested) {
        ESP_LOGI(TAG, "Recovery requested; not starting main application");
        return;
    }

    if (s_main_started) {
        ESP_LOGI(TAG, "Main application already started");
        return;
    }

    cancel_recovery_timer();

    if (s_bootstrap_server) {
        httpd_stop(s_bootstrap_server);
        s_bootstrap_server = NULL;
    }

    start_main_application();
}

static void enter_main_task(void *arg)
{
    (void)arg;
    enter_main();
    s_start_task = NULL;
    vTaskDelete(NULL);
}

static void schedule_main_start(void)
{
    if (s_recovery_requested) {
        ESP_LOGW(TAG, "Recovery requested; skipping main start schedule");
        return;
    }

    if (s_main_started || s_start_task) {
        return;
    }

    if (xTaskCreate(enter_main_task, "start-main", 4096, NULL, tskIDLE_PRIORITY + 4, &s_start_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to schedule main start");
    }
}

static void recovery_timeout_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    if (s_recovery_requested) {
        ESP_LOGI(TAG, "Recovery requested; staying in bootstrap mode");
        cancel_recovery_timer();
        return;
    }

    cancel_recovery_timer();
    ESP_LOGI(TAG, "Recovery window elapsed; starting main application");
    schedule_main_start();
}

static uint32_t get_remaining_seconds(void)
{
    if (!s_recovery_timer) {
        return 0;
    }

    TickType_t now = xTaskGetTickCount();
    TickType_t expiry = xTimerGetExpiryTime(s_recovery_timer);

    if (expiry <= now) {
        return 0;
    }
    TickType_t ticks_remaining = expiry - now;
    return (ticks_remaining + configTICK_RATE_HZ - 1U) / configTICK_RATE_HZ;
}

static esp_err_t bootstrap_root_handler(httpd_req_t *req)
{
    uint32_t seconds = get_remaining_seconds();
    if (seconds == 0 && s_recovery_timer) {
        seconds = CONFIG_RECOVERY_ENTRY_TIME;
    }

    char *page = malloc(BOOTSTRAP_PAGE_MAX);
    if (!page) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_ERR_NO_MEM;
    }
    bool auto_refresh = !s_recovery_requested;
    bool is_recovery = s_recovery_requested;
    esp_err_t err = render_bootstrap_page(page, BOOTSTRAP_PAGE_MAX, seconds, auto_refresh, is_recovery);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to render bootstrap page");
        free(page);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Render failed");
        return err;
    }
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
    if (s_recovery_requested) {
        ESP_LOGI(TAG, "Recovery previously requested; starting main application anyway");
        s_recovery_requested = false;
    }

    ESP_LOGI(TAG, "Skip requested; starting main application immediately");
    schedule_main_start();

    extern const unsigned char bootstrap_skip_html_start[] asm("_binary_bootstrap_skip_html_start");
    extern const unsigned char bootstrap_skip_html_end[] asm("_binary_bootstrap_skip_html_end");
    const size_t skip_html_size = (size_t)(bootstrap_skip_html_end - bootstrap_skip_html_start);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, (const char *)bootstrap_skip_html_start, skip_html_size);
    return ESP_OK;
}

esp_err_t bootstrap(const char *base_path)
{
    if (!base_path) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = strlcpy(s_base_path, base_path, sizeof(s_base_path));
    if (len >= sizeof(s_base_path)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_bootstrap_server) {
        ESP_LOGW(TAG, "Bootstrap server already running");
        return ESP_ERR_INVALID_STATE;
    }

    s_recovery_requested = false;
    s_main_started = false;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 5;

    esp_err_t err = httpd_start(&s_bootstrap_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start bootstrap server: %s", esp_err_to_name(err));
        return err;
    }

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

    if (httpd_register_uri_handler(s_bootstrap_server, &ota_update) != ESP_OK ||
        httpd_register_uri_handler(s_bootstrap_server, &root) != ESP_OK ||
        httpd_register_uri_handler(s_bootstrap_server, &recovery) != ESP_OK ||
        httpd_register_uri_handler(s_bootstrap_server, &skip) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register bootstrap handlers");
        httpd_stop(s_bootstrap_server);
        s_bootstrap_server = NULL;
        return ESP_FAIL;
    }

    s_recovery_timer = xTimerCreate("recovery-entry",
                                    pdMS_TO_TICKS(CONFIG_RECOVERY_ENTRY_TIME * 1000),
                                    pdFALSE,
                                    NULL,
                                    recovery_timeout_cb);
    if (!s_recovery_timer) {
        ESP_LOGE(TAG, "Failed to create recovery timer");
        httpd_stop(s_bootstrap_server);
        s_bootstrap_server = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xTimerStart(s_recovery_timer, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start recovery timer");
        cancel_recovery_timer();
        httpd_stop(s_bootstrap_server);
        s_bootstrap_server = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Bootstrap server started; waiting %ds before starting main application",
             CONFIG_RECOVERY_ENTRY_TIME);
    return ESP_OK;
}
