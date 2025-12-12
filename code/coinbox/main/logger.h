#pragma once

#include <stddef.h>

#define LOGGER_BUFFER_SIZE 5120
#define LOGGER_MESSAGE_MAX 128

/** @brief Clear the in-memory log buffer. */
void logger_clear(void);

/** @brief Log an error message and append to buffer. */
void logger_loge(const char *tag, const char *fmt, ...);
/** @brief Log a warning message and append to buffer. */
void logger_logw(const char *tag, const char *fmt, ...);
/** @brief Log an info message and append to buffer. */
void logger_logi(const char *tag, const char *fmt, ...);
/** @brief Log a debug message and append to buffer. */
void logger_logd(const char *tag, const char *fmt, ...);
/** @brief Log a verbose message and append to buffer. */
void logger_logv(const char *tag, const char *fmt, ...);

/** @brief Get a flattened view of the buffered log text. */
const char *logger_get_buffer(void);
/** @brief Get the current byte size of the flattened buffer. */
size_t logger_get_size(void);
