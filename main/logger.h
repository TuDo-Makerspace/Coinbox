#pragma once

#include <stddef.h>

#define LOGGER_BUFFER_SIZE 8192
#define LOGGER_MESSAGE_MAX 128

/** @brief Initialize logging capture so ESP_LOG* output is mirrored to the buffer. */
void logger_init(void);

/** @brief Clear the in-memory log buffer. */
void logger_clear(void);

/** @brief Get a flattened view of the buffered log text. */
const char *logger_get_buffer(void);
/** @brief Get the current byte size of the flattened buffer. */
size_t logger_get_size(void);
