#include "logger.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

static char logger_buffer[LOGGER_BUFFER_SIZE];
static size_t logger_size;
static vprintf_like_t s_prev_vprintf;
static bool s_logger_initialized;

static void logger_append_text(const char *text, size_t len)
{
    if (!text || len == 0) {
        return;
    }
    if (len >= LOGGER_BUFFER_SIZE) {
        text += (len - (LOGGER_BUFFER_SIZE - 1));
        len = LOGGER_BUFFER_SIZE - 1;
    }
    if (logger_size + len >= LOGGER_BUFFER_SIZE) {
        logger_clear();
    }
    size_t copy_len = len;
    if (logger_size + copy_len >= LOGGER_BUFFER_SIZE) {
        copy_len = LOGGER_BUFFER_SIZE - logger_size - 1;
    }
    memcpy(logger_buffer + logger_size, text, copy_len);
    logger_size += copy_len;
    logger_buffer[logger_size] = '\0';
}

static int logger_vprintf(const char *fmt, va_list args)
{
    int ret = 0;

    if (s_prev_vprintf) {
        va_list args_console;
        va_copy(args_console, args);
        ret = s_prev_vprintf(fmt, args_console);
        va_end(args_console);
    } else {
        va_list args_console;
        va_copy(args_console, args);
        ret = vprintf(fmt, args_console);
        va_end(args_console);
    }

    char line[LOGGER_MESSAGE_MAX];
    va_list args_copy;
    va_copy(args_copy, args);
    int written = vsnprintf(line, sizeof(line), fmt, args_copy);
    va_end(args_copy);
    if (written > 0) {
        size_t len = (written >= (int)sizeof(line)) ? (sizeof(line) - 1) : (size_t)written;
        logger_append_text(line, len);
    }

    return ret;
}

void logger_init(void)
{
    if (s_logger_initialized) {
        return;
    }
    logger_clear();
    s_prev_vprintf = esp_log_set_vprintf(logger_vprintf);
    s_logger_initialized = true;
}

const char *logger_get_buffer(void)
{
    return logger_buffer;
}

size_t logger_get_size(void)
{
    return logger_size;
}

void logger_clear(void)
{
    logger_size = 0;
    logger_buffer[0] = '\0';
}
