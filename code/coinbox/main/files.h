#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#include "esp_err.h"

// ------------------------------------------------------
// File Entry
// ------------------------------------------------------

#define FILE_ENTRY_NAME_MAX 256
#define FILE_ENTRY_MAGIC 0x46454E54u /* 'FENT' */
#define FILE_ENTRY_VERSION 1

typedef struct {
    char name[FILE_ENTRY_NAME_MAX];
    uint8_t probability;  // 0-100
    uint8_t volume;       // 0-100
    bool enabled;
} file_properties_t;

typedef struct {
    bool modified;               // true when properties were changed
    file_properties_t props;     // stored header/properties
    size_t data_size;            // payload length
} file_entry_t;

void file_entry_init(file_entry_t *entry, const char *name);
void file_entry_set_props(file_entry_t *entry, const char *name, uint8_t probability, uint8_t volume, bool enabled);

// ------------------------------------------------------
// Files
// ------------------------------------------------------

#define FILES_MAX_ENTRIES 128 // Increase if needed

extern const size_t FILE_HEADER_SIZE;

esp_err_t files_set_base_path(const char *base_path);
size_t files_count(void);
esp_err_t files_filename(size_t index, char *out_name, size_t out_size);
esp_err_t files_read_header(const char *name, file_entry_t *out);
esp_err_t files_write_header(FILE *f, const file_entry_t *entry);
