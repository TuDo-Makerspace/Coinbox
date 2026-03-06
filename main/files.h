#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#include "esp_err.h"

///////////////////////////////////////////////////////////////////////////////////////////////////
// Constants
///////////////////////////////////////////////////////////////////////////////////////////////////

#define FILE_ENTRY_NAME_MAX 256
#define FILE_PROBABILITY_MAX 100
#define FILE_VOLUME_MAX 125

///////////////////////////////////////////////////////////////////////////////////////////////////
// Structs
///////////////////////////////////////////////////////////////////////////////////////////////////

typedef struct {
    char name[FILE_ENTRY_NAME_MAX];
    uint8_t probability;  // 0-100
    uint8_t volume;       // 0-125
    bool enabled;
} file_properties_t;

///////////////////////////////////////////////////////////////////////////////////////////////////
// Interface
///////////////////////////////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------
// File Properties
//-------------------------------------------------------------------------

void files_props_init(file_properties_t *props, const char *name);
void files_props_set(file_properties_t *props, const char *name, uint8_t probability, uint8_t volume, bool enabled);

//-------------------------------------------------------------------------
// Storage
//-------------------------------------------------------------------------

esp_err_t files_init(void);
esp_err_t files_format_storage(void);

//-------------------------------------------------------------------------
// Files Listing
//-------------------------------------------------------------------------

#define FILES_MAX_ENTRIES 128 // Increase if needed

size_t files_count(void);
esp_err_t files_filename(size_t index, char *out_name, size_t out_size);

//-------------------------------------------------------------------------
// Weighted Selection
//-------------------------------------------------------------------------

esp_err_t files_pick_weighted_enabled(char *out_name, size_t out_size, uint32_t *out_total_weight, size_t *out_candidates);

//-------------------------------------------------------------------------
// Files Metadata
//-------------------------------------------------------------------------

esp_err_t files_read_meta(const char *name, file_properties_t *out);
esp_err_t files_write_meta(const char *name, const file_properties_t *props);

//-------------------------------------------------------------------------
// Files Mutation
//-------------------------------------------------------------------------

esp_err_t files_delete_with_meta(const char *name);
esp_err_t files_rename_with_meta(const char *old_name, const char *new_name);
