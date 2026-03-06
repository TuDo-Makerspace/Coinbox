#include "logger.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

///////////////////////////////////////////////////////////////////////////////////////////////////
// Vars
///////////////////////////////////////////////////////////////////////////////////////////////////

static char s_logger_buffer[LOGGER_BUFFER_SIZE];
static size_t s_logger_size;
static vprintf_like_t s_prev_vprintf;
static bool s_logger_initialized;

///////////////////////////////////////////////////////////////////////////////////////////////////
// Private
///////////////////////////////////////////////////////////////////////////////////////////////////

static void logger_append_text(const char *text, size_t len)
{
    if (!text || len == 0) {
        return;
    }
    if (len >= LOGGER_BUFFER_SIZE) {
        text += (len - (LOGGER_BUFFER_SIZE - 1));
        len = LOGGER_BUFFER_SIZE - 1;
    }
    if (s_logger_size + len >= LOGGER_BUFFER_SIZE) {
        logger_clear();
    }
    size_t copy_len = len;
    if (s_logger_size + copy_len >= LOGGER_BUFFER_SIZE) {
        copy_len = LOGGER_BUFFER_SIZE - s_logger_size - 1;
    }
    memcpy(s_logger_buffer + s_logger_size, text, copy_len);
    s_logger_size += copy_len;
    s_logger_buffer[s_logger_size] = '\0';
}

static size_t strip_ansi_sequences(const char *src, size_t len, char *dst, size_t dst_size)
{
    if (!src || !dst || dst_size == 0) {
        return 0;
    }

    enum {
        ANSI_NORMAL = 0,
        ANSI_ESC,
        ANSI_CSI,
        ANSI_OSC,
        ANSI_OSC_ESC,
    } state = ANSI_NORMAL;

    size_t out = 0;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)src[i];
        switch (state) {
            case ANSI_NORMAL:
                if (c == 0x1B) {
                    state = ANSI_ESC;
                    break;
                }
                if (out + 1 < dst_size) {
                    dst[out++] = (char)c;
                }
                break;

            case ANSI_ESC:
                if (c == '[') {
                    state = ANSI_CSI;
                } else if (c == ']') {
                    state = ANSI_OSC;
                } else {
                    state = ANSI_NORMAL;
                }
                break;

            case ANSI_CSI:
                // CSI sequence ends with a final byte in 0x40..0x7E.
                if (c >= 0x40 && c <= 0x7E) {
                    state = ANSI_NORMAL;
                }
                break;

            case ANSI_OSC:
                // OSC terminates with BEL (0x07) or ST (ESC \).
                if (c == 0x07) {
                    state = ANSI_NORMAL;
                } else if (c == 0x1B) {
                    state = ANSI_OSC_ESC;
                }
                break;

            case ANSI_OSC_ESC:
                if (c == '\\') {
                    state = ANSI_NORMAL;
                } else if (c == 0x07) {
                    state = ANSI_NORMAL;
                } else {
                    state = ANSI_OSC;
                }
                break;
        }
    }

    dst[out] = '\0';
    return out;
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
    char clean_line[LOGGER_MESSAGE_MAX];
    va_list args_copy;
    va_copy(args_copy, args);
    int written = vsnprintf(line, sizeof(line), fmt, args_copy);
    va_end(args_copy);
    if (written > 0) {
        size_t raw_len = (written >= (int)sizeof(line)) ? (sizeof(line) - 1) : (size_t)written;
        size_t clean_len = strip_ansi_sequences(line, raw_len, clean_line, sizeof(clean_line));
        logger_append_text(clean_line, clean_len);
    }

    return ret;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Public Interface
///////////////////////////////////////////////////////////////////////////////////////////////////

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
    return s_logger_buffer;
}

size_t logger_get_size(void)
{
    return s_logger_size;
}

void logger_clear(void)
{
    s_logger_size = 0;
    s_logger_buffer[0] = '\0';
}
