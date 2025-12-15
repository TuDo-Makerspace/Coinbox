#include "files.h"

#include <dirent.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>

#include "esp_log.h"

#define FILES_PATH_MAX 512
#define FILE_ENTRY_MAGIC 0x46454E54u /* 'FENT' */
#define FILE_ENTRY_VERSION 2

static const char *TAG = "files";
static char g_base_path[FILES_PATH_MAX] = {0};
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint8_t probability;
    uint8_t volume;
    uint8_t enabled;
    uint8_t reserved;
} file_entry_meta_t;

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

void file_props_init(file_properties_t *props, const char *path)
{
    if (!props) {
        return;
    }
    memset(props, 0, sizeof(*props));
    set_name(props, path ? basename_from_path(path) : NULL);
    props->probability = 0;
    props->volume = 0;
    props->enabled = false;
}

void file_props_set(file_properties_t *props, const char *name, uint8_t probability, uint8_t volume, bool enabled)
{
    if (!props) {
        return;
    }
    if (name) {
        set_name(props, basename_from_path(name));
    }
    props->probability = clamp_percent(probability);
    props->volume = clamp_percent(volume);
    props->enabled = enabled;
}

static bool has_meta_extension(const char *name)
{
    if (!name) return false;
    const char *dot = strrchr(name, '.');
    return dot && strcmp(dot, ".meta") == 0;
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

static esp_err_t meta_path_for_name(const char *name, char *out, size_t out_size)
{
    if (!name || !out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (g_base_path[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    if (strchr(name, '/')) {
        return ESP_ERR_INVALID_ARG;
    }
    char base[FILE_ENTRY_NAME_MAX];
    strlcpy(base, name, sizeof(base));
    int len = snprintf(out, out_size, "%s/%s.meta", g_base_path, base);
    if (len < 0 || (size_t)len >= out_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static void build_meta(file_entry_meta_t *hdr, const file_properties_t *props)
{
    hdr->magic = FILE_ENTRY_MAGIC;
    hdr->version = FILE_ENTRY_VERSION;
    hdr->probability = clamp_percent(props->probability);
    hdr->volume = clamp_percent(props->volume);
    hdr->enabled = props->enabled ? 1 : 0;
    hdr->reserved = 0;
}

esp_err_t files_write_meta(const char *name, const file_properties_t *props)
{
    if (!name || !props) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[FILES_PATH_MAX];
    esp_err_t err = meta_path_for_name(name, path, sizeof(path));
    if (err != ESP_OK) {
        return err;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        return ESP_FAIL;
    }
    file_entry_meta_t hdr;
    build_meta(&hdr, props);
    size_t written = fwrite(&hdr, 1, sizeof(hdr), f);
    fclose(f);
    return (written == sizeof(hdr)) ? ESP_OK : ESP_FAIL;
}

static esp_err_t file_props_load(const char *name, file_properties_t *out)
{
    if (!name || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    char audio_path[FILES_PATH_MAX];
    char meta_path[FILES_PATH_MAX];
    esp_err_t path_res = full_path_for_name(name, audio_path, sizeof(audio_path));
    if (path_res != ESP_OK) {
        return path_res;
    }
    if (has_meta_extension(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    struct stat audio_stat = {0};
    if (stat(audio_path, &audio_stat) != 0 || S_ISDIR(audio_stat.st_mode)) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t meta_res = meta_path_for_name(name, meta_path, sizeof(meta_path));
    if (meta_res != ESP_OK) {
        return meta_res;
    }
    FILE *f = fopen(meta_path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }

    file_entry_meta_t hdr;
    size_t read_bytes = fread(&hdr, 1, sizeof(hdr), f);
    fclose(f);
    if (read_bytes != sizeof(hdr) || hdr.magic != FILE_ENTRY_MAGIC || hdr.version != FILE_ENTRY_VERSION) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    file_props_init(out, basename_from_path(audio_path));
    out->probability = clamp_percent(hdr.probability);
    out->volume = clamp_percent(hdr.volume);
    out->enabled = hdr.enabled ? true : false;
    return ESP_OK;
}

esp_err_t files_set_base_path(const char *base_path)
{
    if (!base_path) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlcpy(g_base_path, base_path, sizeof(g_base_path)) >= sizeof(g_base_path)) {
        ESP_LOGE(TAG, "Base path too long");
        g_base_path[0] = '\0';
        return ESP_ERR_INVALID_ARG;
    }
    DIR *dir = opendir(g_base_path);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open base path: %s", g_base_path);
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
        if (has_meta_extension(entry->d_name)) {
            continue;
        }
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
        file_properties_t tmp;
        if (file_props_load(entry->d_name, &tmp) == ESP_OK) {
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
        if (has_meta_extension(entry->d_name)) {
            continue;
        }
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
        file_properties_t tmp;
        if (file_props_load(entry->d_name, &tmp) != ESP_OK) {
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

esp_err_t files_read_meta(const char *name, file_properties_t *out)
{
    if (!name || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (g_base_path[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    return file_props_load(name, out);
}

esp_err_t files_delete_with_meta(const char *name)
{
    if (!name) return ESP_ERR_INVALID_ARG;
    char audio_path[FILES_PATH_MAX];
    char meta_path[FILES_PATH_MAX];
    esp_err_t err = full_path_for_name(name, audio_path, sizeof(audio_path));
    if (err != ESP_OK) return err;
    err = meta_path_for_name(name, meta_path, sizeof(meta_path));
    if (err != ESP_OK) return err;
    unlink(audio_path);
    unlink(meta_path);
    return ESP_OK;
}

esp_err_t files_rename_with_meta(const char *old_name, const char *new_name)
{
    if (!old_name || !new_name) return ESP_ERR_INVALID_ARG;
    if (strchr(new_name, '/') || strchr(old_name, '/')) {
        return ESP_ERR_INVALID_ARG;
    }
    char old_path[FILES_PATH_MAX];
    char new_path[FILES_PATH_MAX];
    char old_meta[FILES_PATH_MAX];
    char new_meta[FILES_PATH_MAX];
    esp_err_t err = full_path_for_name(old_name, old_path, sizeof(old_path));
    if (err != ESP_OK) return err;
    err = full_path_for_name(new_name, new_path, sizeof(new_path));
    if (err != ESP_OK) return err;
    err = meta_path_for_name(old_name, old_meta, sizeof(old_meta));
    if (err != ESP_OK) return err;
    err = meta_path_for_name(new_name, new_meta, sizeof(new_meta));
    if (err != ESP_OK) return err;

    if (rename(old_path, new_path) != 0) {
        return ESP_FAIL;
    }
    if (rename(old_meta, new_meta) != 0) {
        /* best-effort rollback */
        rename(new_path, old_path);
        return ESP_FAIL;
    }
    return ESP_OK;
}
