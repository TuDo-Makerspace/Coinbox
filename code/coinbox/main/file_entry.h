#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#include "esp_err.h"

#define FILE_ENTRY_NAME_MAX 256
#define FILE_ENTRY_MAGIC 0x46454E54u /* 'FENT' */
#define FILE_ENTRY_VERSION 1

typedef struct {
    char name[FILE_ENTRY_NAME_MAX];
    uint8_t probability;  // 0-100
    uint8_t volume;       // 0-100
    bool enabled;
    size_t data_offset;   // byte offset to file payload on flash
    size_t data_size;     // payload length
} file_entry_t;

void file_entry_init(file_entry_t *entry, const char *name);
void file_entry_set(file_entry_t *entry, const char *name, uint8_t probability, uint8_t volume, bool enabled);

esp_err_t file_entry_load(const char *path, file_entry_t *out);
esp_err_t file_entry_save(const char *path, const file_entry_t *meta, const uint8_t *data, size_t data_len);

size_t file_entry_header_size(void);
esp_err_t file_entry_write_header(FILE *f, const file_entry_t *meta);
esp_err_t file_entry_read_header(FILE *f, const char *path, file_entry_t *out, size_t file_size);
