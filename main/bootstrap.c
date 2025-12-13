#include "bootstrap.h"

#include <stdbool.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "logger.h"
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

static const char BOOTSTRAP_HTML_TEMPLATE[] =
    "<!DOCTYPE html>"
    "<html lang=\"en\">"
    "<head>"
    "<meta charset=\"UTF-8\">"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
    "<title>Coinbox - Starting</title>"
    "<style>"
    ":root { --text:#0f172a; --muted:#475569; --card:#fff; --border:#e5e7eb; --accent:#2563eb; --accent-2:#1d4ed8; }"
    "* { margin:0; padding:0; box-sizing:border-box; }"
    "body { font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif; background:#f5f7fb; color:var(--text); line-height:1.5; min-height:100vh; display:flex; align-items:center; justify-content:center; text-align:center; }"
    ".content { padding:clamp(1.4rem,3vw+0.5rem,2.6rem); max-width:960px; margin:0 auto; display:flex; align-items:center; justify-content:center; width:100%%; }"
    ".card { width:100%%; background:var(--card); border:1px solid var(--border); border-radius:12px; box-shadow:0 6px 18px rgba(0,0,0,0.05); padding:50px; display:flex; flex-direction:column; gap:1.1rem; align-items:center; text-align:center; }"
    "h1 { font-size:clamp(1.7rem,1vw+1.35rem,2.2rem); }"
    ".status-row { display:flex; gap:0.9rem; align-items:center; justify-content:center; flex-wrap:wrap; text-align:center; }"
    ".badge { display:inline-flex; align-items:center; justify-content:center; padding:0.6rem 0.9rem; border-radius:12px; background:#e0e7ff; color:var(--accent); font-weight:700; min-width:74px; font-size:1rem; }"
    ".status-text { color:var(--muted); font-size:0.98rem; }"
    ".actions { display:flex; gap:0.75rem; flex-wrap:wrap; margin-top:0.15rem; justify-content:center; }"
    ".btn { text-decoration:none; padding:0.75em 1.2em; border-radius:10px; border:1px solid var(--border); font-weight:700; color:var(--text); background:#eef2ff; min-width:150px; text-align:center; transition:background 0.15s, transform 0.1s, box-shadow 0.15s; }"
    ".btn:hover { background:#e0e7ff; }"
    ".btn:active { transform:scale(0.98); }"
    ".btn.primary { background:var(--accent); color:#fff; border-color:var(--accent-2); box-shadow:0 8px 20px rgba(37,99,235,0.18); }"
    ".btn.primary:hover { background:var(--accent-2); }"
    ".note { color:var(--muted); font-size:0.95rem; }"
    "@media (max-width:640px) { .card { padding:50px; } .status-row { align-items:flex-start; } .actions { flex-direction:column; align-items:stretch; } .btn { width:100%%; } }"
    "</style>"
    "</head>"
    "<body>"
    "<div class=\"content\">"
    "<div class=\"card\" id=\"card\">"
    "<h1 id=\"title\">Coinbox is starting</h1>"
    "<div class=\"status-row\" id=\"status-row\"><span class=\"badge\" id=\"countdown\">%lu</span><div class=\"status-text\" id=\"status-text\">seconds remaining</div></div>"
    "<div class=\"actions\" id=\"actions\">"
    "<a class=\"btn primary\" id=\"start-now\" href=\"/skip\">Start now</a>"
    "<a class=\"btn\" id=\"stay\" href=\"#\">Enter recovery mode</a>"
    "</div>"
    "</div>"
    "</div>"
    "<script>"
    "let remaining=%lu;"
    "let autoRefresh=%d;"
    "let isRecovery=%d;"
    "let refreshed=false;"
    "const el=document.getElementById('countdown');"
    "const statusRow=document.getElementById('status-row');"
    "const actions=document.getElementById('actions');"
    "const titleEl=document.getElementById('title');"
    "const statusText=document.getElementById('status-text');"
    "const stayBtn=document.getElementById('stay');"
    "const startBtn=document.getElementById('start-now');"
    "function renderRecovery(){titleEl.textContent='Recovery Mode';statusRow.style.display='flex';el.style.display='none';actions.style.display='flex';stayBtn.style.display='none';startBtn.textContent='Enter main mode';statusText.textContent='Awaiting OTA update...';autoRefresh=0;isRecovery=true;}"
    "async function enterRecovery(){if(isRecovery)return;try{await fetch('/recovery');}catch(_){ }renderRecovery();}"
    "function tick(){if(isRecovery){return;}if(remaining<=0){el.textContent='0';if(autoRefresh&&!refreshed){refreshed=true;setTimeout(()=>location.replace('/'),300);}return;}el.textContent=remaining;remaining-=1;}"
    "stayBtn.addEventListener('click',(e)=>{e.preventDefault();enterRecovery();});"
    "if(isRecovery){renderRecovery();}"
    "tick(); setInterval(tick,1000);"
    "</script>"
    "</body></html>";

static const char BOOTSTRAP_SKIP_HTML[] =
    "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"UTF-8\">"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
    "<title>Starting main application...</title>"
    "<style>"
    "body{margin:0;padding:0;box-sizing:border-box;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;background:#f5f7fb;color:#0f172a;min-height:100vh;display:flex;align-items:center;justify-content:center;}"
    ".card{width:100%%;max-width:640px;background:#fff;border:1px solid #e5e7eb;border-radius:12px;box-shadow:0 6px 18px rgba(0,0,0,0.05);padding:50px;text-align:center;line-height:1.5;}"
    "h1{font-size:1.8rem;margin:0 0 0.35em 0;}"
    "p{margin:0;color:#475569;font-size:1rem;}"
    "</style></head><body>"
    "<div class=\"card\"><h1>Starting main application...</h1><p>Loading interface automatically.</p></div>"
    "<script>"
    "const target='/';"
    "function probe(){fetch(target,{cache:'no-store'}).then(r=>{if(r.ok){location.replace(target);}}).catch(()=>{});} "
    "probe();setInterval(probe,800);"
    "</script>"
    "</body></html>";

static const size_t BOOTSTRAP_PAGE_MAX = 8192;

static void cancel_recovery_timer(void)
{
    if (!s_recovery_timer) {
        return;
    }
    xTimerStop(s_recovery_timer, 0);
    xTimerDelete(s_recovery_timer, 0);
    s_recovery_timer = NULL;
}

static void start_main(void)
{
    if (s_recovery_requested) {
        logger_logi(TAG, "Recovery requested; not starting main application");
        return;
    }

    if (s_main_started) {
        logger_logi(TAG, "Main application already started");
        return;
    }

    cancel_recovery_timer();

    if (s_bootstrap_server) {
        httpd_stop(s_bootstrap_server);
        s_bootstrap_server = NULL;
    }

    // ------- MAIN APPLICATION -------

    esp_err_t err = start_ws_server(s_base_path);
    if (err != ESP_OK) {
        logger_loge(TAG, "Failed to start main application: %s", esp_err_to_name(err));
    } else {
        logger_logi(TAG, "Main application started");
        s_main_started = true;
    }
}

static void start_main_task(void *arg)
{
    (void)arg;
    start_main();
    s_start_task = NULL;
    vTaskDelete(NULL);
}

static void schedule_main_start(void)
{
    if (s_recovery_requested) {
        logger_logw(TAG, "Recovery requested; skipping main start schedule");
        return;
    }

    if (s_main_started || s_start_task) {
        return;
    }

    if (xTaskCreate(start_main_task, "start-main", 4096, NULL, tskIDLE_PRIORITY + 4, &s_start_task) != pdPASS) {
        logger_loge(TAG, "Failed to schedule main start");
    }
}

static void recovery_timeout_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    if (s_recovery_requested) {
        logger_logi(TAG, "Recovery requested; staying in bootstrap mode");
        cancel_recovery_timer();
        return;
    }

    cancel_recovery_timer();
    logger_logi(TAG, "Recovery window elapsed; starting main application");
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
    int auto_refresh = s_recovery_requested ? 0 : 1;
    int is_recovery = s_recovery_requested ? 1 : 0;
    int n = snprintf(page, BOOTSTRAP_PAGE_MAX, BOOTSTRAP_HTML_TEMPLATE,
                     (unsigned long)seconds, (unsigned long)seconds, auto_refresh, is_recovery);
    if (n < 0 || n >= (int)BOOTSTRAP_PAGE_MAX) {
        free(page);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Render failed");
        return ESP_ERR_NO_MEM;
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, page, n);
    free(page);
    return ESP_OK;
}

static esp_err_t bootstrap_recovery_handler(httpd_req_t *req)
{
    s_recovery_requested = true;
    cancel_recovery_timer();

    logger_logw(TAG, "Recovery endpoint hit; countdown aborted");

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Recovery mode engaged. Bootstrap server will stay active.");
    return ESP_OK;
}

static esp_err_t bootstrap_skip_handler(httpd_req_t *req)
{
    if (s_recovery_requested) {
        logger_logi(TAG, "Recovery previously requested; starting main application anyway");
        s_recovery_requested = false;
    }

    logger_logi(TAG, "Skip requested; starting main application immediately");
    schedule_main_start();

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, BOOTSTRAP_SKIP_HTML, HTTPD_RESP_USE_STRLEN);
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
        logger_logw(TAG, "Bootstrap server already running");
        return ESP_ERR_INVALID_STATE;
    }

    s_recovery_requested = false;
    s_main_started = false;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 5;

    esp_err_t err = httpd_start(&s_bootstrap_server, &config);
    if (err != ESP_OK) {
        logger_loge(TAG, "Failed to start bootstrap server: %s", esp_err_to_name(err));
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
        logger_loge(TAG, "Failed to register bootstrap handlers");
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
        logger_loge(TAG, "Failed to create recovery timer");
        httpd_stop(s_bootstrap_server);
        s_bootstrap_server = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xTimerStart(s_recovery_timer, 0) != pdPASS) {
        logger_loge(TAG, "Failed to start recovery timer");
        cancel_recovery_timer();
        httpd_stop(s_bootstrap_server);
        s_bootstrap_server = NULL;
        return ESP_FAIL;
    }

    logger_logi(TAG, "Bootstrap server started; waiting %ds before starting main application",
             CONFIG_RECOVERY_ENTRY_TIME);
    return ESP_OK;
}
