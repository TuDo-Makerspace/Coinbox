#include "file_entry.h"

#include <stdio.h>
#include <string.h>

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint8_t probability;
    uint8_t volume;
    uint8_t enabled;
    uint8_t reserved[3];
} file_entry_header_t;

static uint8_t clamp_percent(uint8_t value)
{
    if (value > 100) {
        return 100;
    }
    return value;
}

static const char *basename_from_path(const char *path)
{
    if (!path) {
        return "";
    }
    const char *slash = strrchr(path, '/');
    const char *base = (slash && *(slash + 1)) ? slash + 1 : path;
    return base;
}

static void set_name(file_entry_t *entry, const char *name)
{
    if (name) {
        strncpy(entry->name, name, FILE_ENTRY_NAME_MAX - 1);
        entry->name[FILE_ENTRY_NAME_MAX - 1] = '\0';
    } else {
        entry->name[0] = '\0';
    }
}

void file_entry_init(file_entry_t *entry, const char *path)
{
    if (!entry) {
        return;
    }
    set_name(entry, path ? basename_from_path(path) : NULL);
    entry->probability = 0;
    entry->volume = 0;
    entry->enabled = false;
    entry->data_offset = 0;
    entry->data_size = 0;
}

void file_entry_set(file_entry_t *entry, const char *path, uint8_t probability, uint8_t volume, bool enabled)
{
    if (!entry) {
        return;
    }
    set_name(entry, path ? basename_from_path(path) : NULL);
    entry->probability = clamp_percent(probability);
    entry->volume = clamp_percent(volume);
    entry->enabled = enabled;
}

size_t file_entry_header_size(void)
{
    return sizeof(file_entry_header_t);
}

static void build_header(file_entry_header_t *hdr, const file_entry_t *meta)
{
    hdr->magic = FILE_ENTRY_MAGIC;
    hdr->version = FILE_ENTRY_VERSION;
    hdr->probability = clamp_percent(meta->probability);
    hdr->volume = clamp_percent(meta->volume);
    hdr->enabled = meta->enabled ? 1 : 0;
    memset(hdr->reserved, 0, sizeof(hdr->reserved));
}

esp_err_t file_entry_write_header(FILE *f, const file_entry_t *meta)
{
    if (!f || !meta) {
        return ESP_ERR_INVALID_ARG;
    }
    file_entry_header_t hdr;
    build_header(&hdr, meta);
    size_t written = fwrite(&hdr, 1, sizeof(hdr), f);
    return (written == sizeof(hdr)) ? ESP_OK : ESP_FAIL;
}

esp_err_t file_entry_read_header(FILE *f, const char *path, file_entry_t *out, size_t file_size)
{
    if (!f || !path || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    file_entry_header_t hdr;
    size_t read_bytes = fread(&hdr, 1, sizeof(hdr), f);
    if (read_bytes != sizeof(hdr) || hdr.magic != FILE_ENTRY_MAGIC || hdr.version != FILE_ENTRY_VERSION) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    set_name(out, basename_from_path(path));
    out->probability = clamp_percent(hdr.probability);
    out->volume = clamp_percent(hdr.volume);
    out->enabled = hdr.enabled ? true : false;
    out->data_offset = sizeof(hdr);
    out->data_size = (file_size > sizeof(hdr)) ? (file_size - sizeof(hdr)) : 0;

    return ESP_OK;
}

esp_err_t file_entry_load(const char *path, file_entry_t *out)
{
    if (!path || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    // -- Get file size --

    FILE *f = fopen(path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    long file_size_long = ftell(f);
    if (file_size_long < 0) {
        fclose(f);
        return ESP_FAIL;
    }
    size_t file_size = (size_t)file_size_long;
    rewind(f);

    // -- Read header --

    esp_err_t err = file_entry_read_header(f, path, out, file_size);
    if (err != ESP_OK) {
        fclose(f);
        return err;
    }

    fclose(f);
    return ESP_OK;
}

esp_err_t file_entry_save(const char *path, const file_entry_t *meta, const uint8_t *data, size_t data_len)
{
    if (!path || !meta || (!data && data_len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        return ESP_FAIL;
    }

    if (file_entry_write_header(f, meta) != ESP_OK) {
        fclose(f);
        return ESP_FAIL;
    }

    if (data_len > 0 && fwrite(data, 1, data_len, f) != data_len) {
        fclose(f);
        return ESP_FAIL;
    }

    fclose(f);
    return ESP_OK;
}
