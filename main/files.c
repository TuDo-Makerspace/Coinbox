#include "files.h"

#include <dirent.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>

#include "logger.h"

#define FILES_PATH_MAX 512

static const char *TAG = "files";
static char g_base_path[FILES_PATH_MAX] = {0};
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint8_t probability;
    uint8_t volume;
    uint8_t enabled;
    uint8_t reserved[3];
} file_entry_header_t;

const size_t FILE_HEADER_SIZE = sizeof(file_entry_header_t);

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

static void set_name(file_properties_t *props, const char *name)
{
    if (name) {
        strncpy(props->name, name, FILE_ENTRY_NAME_MAX - 1);
        props->name[FILE_ENTRY_NAME_MAX - 1] = '\0';
    } else {
        props->name[0] = '\0';
    }
}

void file_entry_init(file_entry_t *entry, const char *path)
{
    if (!entry) {
        return;
    }
    memset(entry, 0, sizeof(*entry));
    set_name(&entry->props, path ? basename_from_path(path) : NULL);
    entry->props.probability = 0;
    entry->props.volume = 0;
    entry->props.enabled = false;
    entry->data_size = 0;
}

void file_entry_set_props(file_entry_t *entry, const char *name, uint8_t probability, uint8_t volume, bool enabled)
{
    if (!entry) {
        return;
    }
    if (name) {
        set_name(&entry->props, basename_from_path(name));
    }
    entry->props.probability = clamp_percent(probability);
    entry->props.volume = clamp_percent(volume);
    entry->props.enabled = enabled;
    entry->modified = true;
}

static esp_err_t full_path_for_name(const char *name, char *out, size_t out_size)
{
    if (!name || !out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (g_base_path[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    int len = snprintf(out, out_size, "%s/%s", g_base_path, name);
    if (len < 0 || (size_t)len >= out_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static void build_header(file_entry_header_t *hdr, const file_entry_t *entry)
{
    hdr->magic = FILE_ENTRY_MAGIC;
    hdr->version = FILE_ENTRY_VERSION;
    hdr->probability = clamp_percent(entry->props.probability);
    hdr->volume = clamp_percent(entry->props.volume);
    hdr->enabled = entry->props.enabled ? 1 : 0;
    memset(hdr->reserved, 0, sizeof(hdr->reserved));
}

esp_err_t files_write_header(FILE *f, const file_entry_t *entry)
{
    if (!f || !entry) {
        return ESP_ERR_INVALID_ARG;
    }
    file_entry_header_t hdr;
    build_header(&hdr, entry);
    size_t written = fwrite(&hdr, 1, sizeof(hdr), f);
    return (written == sizeof(hdr)) ? ESP_OK : ESP_FAIL;
}

static esp_err_t file_entry_read_header(FILE *f, const char *path, file_entry_t *out, size_t file_size)
{
    if (!f || !path || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    file_entry_header_t hdr;
    size_t read_bytes = fread(&hdr, 1, sizeof(hdr), f);
    if (read_bytes != sizeof(hdr) || hdr.magic != FILE_ENTRY_MAGIC || hdr.version != FILE_ENTRY_VERSION) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    file_entry_init(out, basename_from_path(path));
    out->props.probability = clamp_percent(hdr.probability);
    out->props.volume = clamp_percent(hdr.volume);
    out->props.enabled = hdr.enabled ? true : false;
    out->data_size = (file_size > FILE_HEADER_SIZE) ? (file_size - FILE_HEADER_SIZE) : 0;
    out->modified = false;

    return ESP_OK;
}

static esp_err_t file_entry_load(const char *path, file_entry_t *out)
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

esp_err_t files_set_base_path(const char *base_path)
{
    if (!base_path) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlcpy(g_base_path, base_path, sizeof(g_base_path)) >= sizeof(g_base_path)) {
        logger_loge(TAG, "Base path too long");
        g_base_path[0] = '\0';
        return ESP_ERR_INVALID_ARG;
    }
    DIR *dir = opendir(g_base_path);
    if (!dir) {
        logger_loge(TAG, "Failed to open base path: %s", g_base_path);
        g_base_path[0] = '\0';
        return ESP_FAIL;
    }
    closedir(dir);
    return ESP_OK;
}

size_t files_count(void)
{
    if (g_base_path[0] == '\0') {
        return 0;
    }
    DIR *dir = opendir(g_base_path);
    if (!dir) {
        return 0;
    }
    size_t count = 0;
    struct dirent *entry = NULL;
    struct stat st;
    char entrypath[FILES_PATH_MAX];

    while ((entry = readdir(dir)) != NULL) {
        int len = snprintf(entrypath, sizeof(entrypath), "%s/%s", g_base_path, entry->d_name);
        if (len < 0 || len >= (int)sizeof(entrypath)) {
            continue;
        }
        if (stat(entrypath, &st) == -1) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            continue;
        }
        file_entry_t tmp;
        if (file_entry_load(entrypath, &tmp) == ESP_OK) {
            count++;
        }
    }

    closedir(dir);
    return count;
}

esp_err_t files_filename(size_t index, char *out_name, size_t out_size)
{
    if (!out_name || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (g_base_path[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    DIR *dir = opendir(g_base_path);
    if (!dir) {
        return ESP_FAIL;
    }

    struct dirent *entry = NULL;
    struct stat st;
    char entrypath[FILES_PATH_MAX];
    size_t current = 0;
    esp_err_t result = ESP_ERR_NOT_FOUND;

    while ((entry = readdir(dir)) != NULL) {
        int len = snprintf(entrypath, sizeof(entrypath), "%s/%s", g_base_path, entry->d_name);
        if (len < 0 || len >= (int)sizeof(entrypath)) {
            continue;
        }
        if (stat(entrypath, &st) == -1) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            continue;
        }
        file_entry_t tmp;
        if (file_entry_load(entrypath, &tmp) != ESP_OK) {
            continue;
        }
        if (current == index) {
            strlcpy(out_name, entry->d_name, out_size);
            result = ESP_OK;
            break;
        }
        current++;
    }

    closedir(dir);
    return result;
}

esp_err_t files_read_header(const char *name, file_entry_t *out)
{
    if (!name || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (g_base_path[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    char path[FILES_PATH_MAX];
    esp_err_t path_res = full_path_for_name(name, path, sizeof(path));
    if (path_res != ESP_OK) {
        return path_res;
    }
    return file_entry_load(path, out);
}
