#pragma once

#include <stddef.h>

#define LOGGER_BUFFER_SIZE 5120
#define LOGGER_MESSAGE_MAX 128

void logger_clear(void);

void logger_loge(const char *tag, const char *fmt, ...);
void logger_logw(const char *tag, const char *fmt, ...);
void logger_logi(const char *tag, const char *fmt, ...);
void logger_logd(const char *tag, const char *fmt, ...);
void logger_logv(const char *tag, const char *fmt, ...);

const char *logger_get_buffer(void);
size_t logger_get_size(void);
