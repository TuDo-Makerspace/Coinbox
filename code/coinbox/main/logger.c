#include "logger.h"

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

static char logger_buffer[LOGGER_BUFFER_SIZE];
static size_t logger_size;

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

static void logger_append(const char *level, const char *tag, const char *message)
{
    if (!message) {
        return;
    }

    char entry[LOGGER_MESSAGE_MAX + 32];
    int entry_len = snprintf(entry, sizeof(entry), "%s %s: %s\n",
                             level ? level : "", tag ? tag : "", message);
    if (entry_len < 0) {
        return;
    }
    if ((size_t)entry_len >= sizeof(entry)) {
        entry_len = sizeof(entry) - 1;
    }

    if (logger_size + (size_t)entry_len >= LOGGER_BUFFER_SIZE) {
        logger_clear();
    }

    size_t copy_len = (size_t)entry_len;
    if (copy_len >= LOGGER_BUFFER_SIZE) {
        copy_len = LOGGER_BUFFER_SIZE - 1;
    }
    if (logger_size + copy_len >= LOGGER_BUFFER_SIZE) {
        copy_len = LOGGER_BUFFER_SIZE - logger_size - 1;
    }

    memcpy(logger_buffer + logger_size, entry, copy_len);
    logger_size += copy_len;
    logger_buffer[logger_size] = '\0';
}

static void logger_write(const char *level, const char *tag, const char *fmt, va_list args)
{
    char truncated[LOGGER_MESSAGE_MAX];

    va_list args_for_trunc;
    va_copy(args_for_trunc, args);
    int msg_len = vsnprintf(truncated, sizeof(truncated), fmt, args_for_trunc);
    va_end(args_for_trunc);
    if (msg_len < 0) {
        return;
    }
    if ((size_t)msg_len >= sizeof(truncated)) {
        msg_len = sizeof(truncated) - 1;
    }
    truncated[msg_len] = '\0';

    char *full_message = NULL;
    va_list args_for_full;
    va_copy(args_for_full, args);
    int full_len = vsnprintf(NULL, 0, fmt, args_for_full);
    va_end(args_for_full);
    if (full_len >= 0) {
        full_message = malloc((size_t)full_len + 1);
        if (full_message) {
            va_list args_for_full_write;
            va_copy(args_for_full_write, args);
            vsnprintf(full_message, (size_t)full_len + 1, fmt, args_for_full_write);
            va_end(args_for_full_write);
        }
    }

    const char *out = full_message ? full_message : truncated;

    if (level && level[0] == 'E') {
        ESP_LOGE(tag, "%s", out);
    } else if (level && level[0] == 'W') {
        ESP_LOGW(tag, "%s", out);
    } else if (level && level[0] == 'I') {
        ESP_LOGI(tag, "%s", out);
    } else if (level && level[0] == 'D') {
        ESP_LOGD(tag, "%s", out);
    } else {
        ESP_LOGV(tag, "%s", out);
    }

    logger_append(level, tag, truncated);

    if (full_message) {
        free(full_message);
    }
}

void logger_loge(const char *tag, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    logger_write("E", tag, fmt, args);
    va_end(args);
}

void logger_logw(const char *tag, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    logger_write("W", tag, fmt, args);
    va_end(args);
}

void logger_logi(const char *tag, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    logger_write("I", tag, fmt, args);
    va_end(args);
}

void logger_logd(const char *tag, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    logger_write("D", tag, fmt, args);
    va_end(args);
}

void logger_logv(const char *tag, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    logger_write("V", tag, fmt, args);
    va_end(args);
}
