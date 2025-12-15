#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#include "esp_err.h"

#define FILE_ENTRY_NAME_MAX 256
typedef struct {
    char name[FILE_ENTRY_NAME_MAX];
    uint8_t probability;  // 0-100
    uint8_t volume;       // 0-100
    bool enabled;
} file_properties_t;

void file_props_init(file_properties_t *props, const char *name);
void file_props_set(file_properties_t *props, const char *name, uint8_t probability, uint8_t volume, bool enabled);

// ------------------------------------------------------
// Files
// ------------------------------------------------------

#define FILES_MAX_ENTRIES 128 // Increase if needed

esp_err_t files_set_base_path(const char *base_path);
size_t files_count(void);
esp_err_t files_filename(size_t index, char *out_name, size_t out_size);
esp_err_t files_read_meta(const char *name, file_properties_t *out);
esp_err_t files_write_meta(const char *name, const file_properties_t *props);
esp_err_t files_delete_with_meta(const char *name);
esp_err_t files_rename_with_meta(const char *old_name, const char *new_name);
