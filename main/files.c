#include "files.h"

#include <dirent.h>
#include <stdio.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>

#include "esp_log.h"
#include "esp_littlefs.h"
#include "esp_random.h"
#include "sdkconfig.h"

///////////////////////////////////////////////////////////////////////////////////////////////////
// Constants
///////////////////////////////////////////////////////////////////////////////////////////////////

#define FILES_PATH_MAX 512
#define FILE_ENTRY_MAGIC 0x46454E54u /* 'FENT' */
#define FILE_ENTRY_VERSION 2
#define DEFAULT_SOUND_PROBABILITY 100
#define DEFAULT_SOUND_VOLUME 100

static const char *TAG = "files";
static const char *MOUNT_TAG = "mount";
static const char *BASE_PATH = CONFIG_BASE_PATH;

#if defined(COINBOX_HAS_EMBEDDED_DEFAULT_MP3)
extern const unsigned char default_mp3_start[] asm("_binary_default_mp3_start");
extern const unsigned char default_mp3_end[] asm("_binary_default_mp3_end");
#elif defined(COINBOX_HAS_EMBEDDED_FALLBACK_MP3)
extern const unsigned char fallback_mp3_start[] asm("_binary_fallback_mp3_start");
extern const unsigned char fallback_mp3_end[] asm("_binary_fallback_mp3_end");
#else
#error "A built-in default audio asset must be embedded."
#endif

///////////////////////////////////////////////////////////////////////////////////////////////////
// Structs
///////////////////////////////////////////////////////////////////////////////////////////////////

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint8_t probability;
    uint8_t volume;
    uint8_t enabled;
    uint8_t reserved;
} file_entry_meta_t;

///////////////////////////////////////////////////////////////////////////////////////////////////
// Vars
///////////////////////////////////////////////////////////////////////////////////////////////////

static char s_base_path[FILES_PATH_MAX] = {0};

///////////////////////////////////////////////////////////////////////////////////////////////////
// Private Helpers
///////////////////////////////////////////////////////////////////////////////////////////////////

static esp_err_t full_path_for_name(const char *name, char *out, size_t out_size);

//-------------------------------------------------------------------------
// Common
//-------------------------------------------------------------------------

static uint8_t clamp_probability(uint8_t value)
{
    if (value > FILE_PROBABILITY_MAX) {
        return FILE_PROBABILITY_MAX;
    }
    return value;
}

static uint8_t clamp_volume(uint8_t value)
{
    if (value > FILE_VOLUME_MAX) {
        return FILE_VOLUME_MAX;
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

bool files_is_default_sound_name(const char *name)
{
    if (!name || !*name) {
        return false;
    }
    return strcasecmp(basename_from_path(name), FILES_DEFAULT_SOUND_NAME) == 0;
}

static void default_sound_props_init(file_properties_t *props)
{
    files_props_init(props, FILES_DEFAULT_SOUND_NAME);
    files_props_set(props,
                    FILES_DEFAULT_SOUND_NAME,
                    DEFAULT_SOUND_PROBABILITY,
                    DEFAULT_SOUND_VOLUME,
                    true);
}

static const unsigned char *default_sound_payload(size_t *out_size, const char **out_source_name)
{
#if defined(COINBOX_HAS_EMBEDDED_DEFAULT_MP3)
    if (out_size) {
        *out_size = (size_t)(default_mp3_end - default_mp3_start);
    }
    if (out_source_name) {
        *out_source_name = "default.mp3";
    }
    return default_mp3_start;
#else
    if (out_size) {
        *out_size = (size_t)(fallback_mp3_end - fallback_mp3_start);
    }
    if (out_source_name) {
        *out_source_name = "fallback.mp3";
    }
    return fallback_mp3_start;
#endif
}

static esp_err_t write_default_sound_audio_file(void)
{
    char audio_path[FILES_PATH_MAX];
    esp_err_t err = full_path_for_name(FILES_DEFAULT_SOUND_NAME, audio_path, sizeof(audio_path));
    if (err != ESP_OK) {
        return err;
    }

    size_t payload_size = 0;
    const char *source_name = NULL;
    const unsigned char *payload = default_sound_payload(&payload_size, &source_name);
    if (!payload || payload_size == 0) {
        ESP_LOGE(TAG, "Built-in default sound asset is empty");
        return ESP_FAIL;
    }

    FILE *f = fopen(audio_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to create built-in default sound file: %s", audio_path);
        return ESP_FAIL;
    }

    size_t written = fwrite(payload, 1, payload_size, f);
    fclose(f);
    if (written != payload_size) {
        ESP_LOGE(TAG,
                 "Failed to write built-in default sound file completely (%u/%u bytes)",
                 (unsigned)written,
                 (unsigned)payload_size);
        unlink(audio_path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG,
             "Provisioned built-in default sound as %s from embedded %s (%u bytes)",
             FILES_DEFAULT_SOUND_NAME,
             source_name ? source_name : "asset",
             (unsigned)payload_size);
    return ESP_OK;
}

static esp_err_t ensure_default_sound(const file_properties_t *preserved_meta)
{
    file_properties_t effective_meta;
    default_sound_props_init(&effective_meta);
    if (preserved_meta) {
        effective_meta.probability = clamp_probability(preserved_meta->probability);
        effective_meta.volume = clamp_volume(preserved_meta->volume);
        effective_meta.enabled = preserved_meta->enabled;
    }

    esp_err_t err = write_default_sound_audio_file();
    if (err != ESP_OK) {
        return err;
    }

    err = files_write_meta(FILES_DEFAULT_SOUND_NAME, &effective_meta);
    if (err != ESP_OK) {
        char audio_path[FILES_PATH_MAX];
        if (full_path_for_name(FILES_DEFAULT_SOUND_NAME, audio_path, sizeof(audio_path)) == ESP_OK) {
            unlink(audio_path);
        }
        return err;
    }

    return ESP_OK;
}

//-------------------------------------------------------------------------
// Storage
//-------------------------------------------------------------------------

static esp_err_t mount_storage(void)
{
    ESP_LOGI(MOUNT_TAG, "Initializing LittleFS");
    if (!BASE_PATH || BASE_PATH[0] == '\0') {
        ESP_LOGE(MOUNT_TAG, "Base path not configured");
        return ESP_ERR_INVALID_ARG;
    }

    esp_vfs_littlefs_conf_t conf = {
        .base_path              = BASE_PATH,
        .partition_label        = "storage",
        .format_if_mount_failed = true,
        .dont_mount             = false
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(MOUNT_TAG, "Failed to mount or format LittleFS");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(MOUNT_TAG, "LittleFS partition not found");
        } else {
            ESP_LOGE(MOUNT_TAG, "Failed to initialize LittleFS (%s)", esp_err_to_name(ret));
        }
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_littlefs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(MOUNT_TAG, "Failed to get LittleFS partition info (%s)", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(MOUNT_TAG, "LittleFS partition mounted at '%s' (total: %d, used: %d)",
             BASE_PATH, total, used);
    return ESP_OK;
}

//-------------------------------------------------------------------------
// Paths
//-------------------------------------------------------------------------

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
    if (s_base_path[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    int len = snprintf(out, out_size, "%s/%s", s_base_path, name);
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
    if (s_base_path[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    if (strchr(name, '/')) {
        return ESP_ERR_INVALID_ARG;
    }
    char base[FILE_ENTRY_NAME_MAX];
    strlcpy(base, name, sizeof(base));
    int len = snprintf(out, out_size, "%s/%s.meta", s_base_path, base);
    if (len < 0 || (size_t)len >= out_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

//-------------------------------------------------------------------------
// Metadata
//-------------------------------------------------------------------------

static void build_meta(file_entry_meta_t *hdr, const file_properties_t *props)
{
    hdr->magic = FILE_ENTRY_MAGIC;
    hdr->version = FILE_ENTRY_VERSION;
    hdr->probability = clamp_probability(props->probability);
    hdr->volume = clamp_volume(props->volume);
    hdr->enabled = props->enabled ? 1 : 0;
    hdr->reserved = 0;
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

    files_props_init(out, basename_from_path(audio_path));
    out->probability = clamp_probability(hdr.probability);
    out->volume = clamp_volume(hdr.volume);
    out->enabled = hdr.enabled ? true : false;
    return ESP_OK;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Public Interface
///////////////////////////////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------
// File Properties
//-------------------------------------------------------------------------

void files_props_init(file_properties_t *props, const char *path)
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

void files_props_set(file_properties_t *props, const char *name, uint8_t probability, uint8_t volume, bool enabled)
{
    if (!props) {
        return;
    }
    if (name) {
        set_name(props, basename_from_path(name));
    }
    props->probability = clamp_probability(probability);
    props->volume = clamp_volume(volume);
    props->enabled = enabled;
}

//-------------------------------------------------------------------------
// Storage
//-------------------------------------------------------------------------

esp_err_t files_init(void)
{
    const char *base_path = BASE_PATH;
    if (!base_path || base_path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlcpy(s_base_path, base_path, sizeof(s_base_path)) >= sizeof(s_base_path)) {
        ESP_LOGE(TAG, "Base path too long");
        s_base_path[0] = '\0';
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = mount_storage();
    if (ret != ESP_OK) {
        s_base_path[0] = '\0';
        return ret;
    }
    DIR *dir = opendir(s_base_path);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open base path: %s", s_base_path);
        s_base_path[0] = '\0';
        return ESP_FAIL;
    }
    closedir(dir);

    file_properties_t default_meta;
    bool have_default_meta = (files_read_meta(FILES_DEFAULT_SOUND_NAME, &default_meta) == ESP_OK);
    esp_err_t ensure_err = ensure_default_sound(have_default_meta ? &default_meta : NULL);
    if (ensure_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to ensure built-in default sound: %s", esp_err_to_name(ensure_err));
        return ensure_err;
    }

    return ESP_OK;
}

esp_err_t files_format_storage(void)
{
    file_properties_t preserved_default_meta;
    bool have_preserved_default_meta = (files_read_meta(FILES_DEFAULT_SOUND_NAME, &preserved_default_meta) == ESP_OK);

    ESP_LOGW(MOUNT_TAG, "Formatting LittleFS partition");
    esp_err_t err = esp_vfs_littlefs_unregister("storage");
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGE(MOUNT_TAG, "Failed to unmount before format: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_littlefs_format("storage");
    if (err != ESP_OK) {
        ESP_LOGE(MOUNT_TAG, "Format failed: %s", esp_err_to_name(err));
        return err;
    }

    err = mount_storage();
    if (err != ESP_OK) {
        return err;
    }

    err = ensure_default_sound(have_preserved_default_meta ? &preserved_default_meta : NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to restore built-in default sound after format: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

//-------------------------------------------------------------------------
// Files Listing
//-------------------------------------------------------------------------

size_t files_count(void)
{
    if (s_base_path[0] == '\0') {
        return 0;
    }
    DIR *dir = opendir(s_base_path);
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
        int len = snprintf(entrypath, sizeof(entrypath), "%s/%s", s_base_path, entry->d_name);
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
    if (s_base_path[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    DIR *dir = opendir(s_base_path);
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
        int len = snprintf(entrypath, sizeof(entrypath), "%s/%s", s_base_path, entry->d_name);
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

esp_err_t files_pick_weighted_enabled(char *out_name, size_t out_size, uint32_t *out_total_weight, size_t *out_candidates)
{
    if (!out_name || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_base_path[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    out_name[0] = '\0';
    if (out_total_weight) {
        *out_total_weight = 0;
    }
    if (out_candidates) {
        *out_candidates = 0;
    }

    DIR *dir = opendir(s_base_path);
    if (!dir) {
        return ESP_FAIL;
    }

    struct dirent *entry = NULL;
    uint32_t total_weight = 0;
    size_t candidates = 0;
    bool selected = false;

    while ((entry = readdir(dir)) != NULL) {
        if (has_meta_extension(entry->d_name)) {
            continue;
        }

        file_properties_t meta;
        if (file_props_load(entry->d_name, &meta) != ESP_OK) {
            continue;
        }
        if (!meta.enabled || meta.probability == 0) {
            continue;
        }

        uint32_t weight = (uint32_t)meta.probability;
        total_weight += weight;
        candidates++;

        // Weighted replacement ensures probability proportional to file weight.
        if ((esp_random() % total_weight) < weight) {
            strlcpy(out_name, entry->d_name, out_size);
            selected = true;
        }
    }

    closedir(dir);

    if (out_total_weight) {
        *out_total_weight = total_weight;
    }
    if (out_candidates) {
        *out_candidates = candidates;
    }

    if (!selected) {
        return ESP_ERR_NOT_FOUND;
    }

    return ESP_OK;
}

//-------------------------------------------------------------------------
// Files Metadata
//-------------------------------------------------------------------------

esp_err_t files_read_meta(const char *name, file_properties_t *out)
{
    if (!name || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_base_path[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    return file_props_load(name, out);
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

//-------------------------------------------------------------------------
// Files Mutation
//-------------------------------------------------------------------------

esp_err_t files_delete_with_meta(const char *name)
{
    if (!name) return ESP_ERR_INVALID_ARG;
    if (files_is_default_sound_name(name)) return ESP_ERR_INVALID_STATE;
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
    if (files_is_default_sound_name(old_name) || files_is_default_sound_name(new_name)) {
        return ESP_ERR_INVALID_STATE;
    }
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
