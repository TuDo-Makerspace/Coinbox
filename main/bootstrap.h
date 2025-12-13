#pragma once

#include "esp_err.h"

/**
 * @brief Start the bootstrap web server.
 *
 * Serves a minimal page while offering a window to enter recovery mode.
 * If recovery is not requested within the configured timeout, the full
 * web server is started automatically.
 */
esp_err_t bootstrap(const char *base_path);
