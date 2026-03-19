/*
 *	The MIT License (MIT)
 *
 *	Copyright (c) 2026 TuDo Makerspace
 *
 *	Permission is hereby granted, free of charge, to any person obtaining a copy
 *	of this software and associated documentation files (the "Software"), to deal
 *	in the Software without restriction, including without limitation the rights
 *	to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *	copies of the Software, and to permit persons to whom the Software is
 *	furnished to do so, subject to the following conditions:
 *
 *	The above copyright notice and this permission notice shall be included in all
 *	copies or substantial portions of the Software.
 *
 *	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *	IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *	FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *	AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *	LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *	OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *	SOFTWARE.
 */

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
#define FILE_ENTRY_VERSION 3
#define FILE_ENTRY_VERSION_LEGACY 2
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
} file_entry_meta_v2_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint8_t probability;
    uint8_t volume;
    uint8_t enabled;
    uint8_t reserved;
    uint32_t trim_start_ms;
    uint32_t trim_stop_ms;
} file_entry_meta_t;

///////////////////////////////////////////////////////////////////////////////////////////////////
// Vars
///////////////////////////////////////////////////////////////////////////////////////////////////

static char s_base_path[FILES_PATH_MAX] = {0};

///////////////////////////////////////////////////////////////////////////////////////////////////
// Private Helpers
///////////////////////////////////////////////////////////////////////////////////////////////////

static esp_err_t full_path_for_name(const char *name, char *out, size_t out_size);
static bool parse_mp3_header(uint32_t header,
                             int *sample_rate,
                             int *frame_size,
                             int *samples_per_frame);
static uint32_t read_be32(const uint8_t *buf);
static bool mp3_gapless_trim_samples_from_frame(FILE *f,
                                                long frame_offset,
                                                uint32_t header,
                                                int frame_size,
                                                uint32_t *out_trim_samples);
static esp_err_t mp3_duration_ms_from_path(const char *path, uint32_t *out_duration_ms);

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
        effective_meta.trim_start_ms = preserved_meta->trim_start_ms;
        effective_meta.trim_stop_ms = preserved_meta->trim_stop_ms;
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
// Audio Duration
//-------------------------------------------------------------------------

#if CONFIG_TEST_AUDIO_MOCK_BACKEND
static uint32_t mock_playback_duration_from_name_ms(const char *name)
{
    if (!name || !name[0]) {
        return 0;
    }

    const char *base = basename_from_path(name);
    const char *dot = strrchr(base, '.');
    size_t name_len = (dot && dot > base) ? (size_t)(dot - base) : strlen(base);
    uint32_t parsed_ms = 0;

    for (size_t i = 1; (i + 1) < name_len; i++) {
        if (base[i] != 'm' || base[i + 1] != 's') {
            continue;
        }

        size_t start = i;
        while (start > 0 && base[start - 1] >= '0' && base[start - 1] <= '9') {
            start--;
        }
        if (start == i) {
            continue;
        }

        uint32_t value = 0;
        for (size_t j = start; j < i; j++) {
            value = value * 10U + (uint32_t)(base[j] - '0');
            if (value > 3600000U) {
                value = 3600000U;
                break;
            }
        }
        parsed_ms = value;
    }

    return parsed_ms;
}
#endif

static bool parse_mp3_header(uint32_t header,
                             int *sample_rate,
                             int *frame_size,
                             int *samples_per_frame)
{
    if (((header >> 21) & 0x7FF) != 0x7FF) {
        return false;
    }

    uint8_t version_id = (uint8_t)((header >> 19) & 0x3);
    uint8_t layer_id = (uint8_t)((header >> 17) & 0x3);
    uint8_t bitrate_idx = (uint8_t)((header >> 12) & 0xF);
    uint8_t sample_idx = (uint8_t)((header >> 10) & 0x3);
    uint8_t padding = (uint8_t)((header >> 9) & 0x1);

    if (version_id == 1 || layer_id != 1 || bitrate_idx == 0 || bitrate_idx == 0xF || sample_idx == 3) {
        return false;
    }

    static const int sample_rate_v1[3] = {44100, 48000, 32000};
    static const int sample_rate_v2[3] = {22050, 24000, 16000};
    static const int sample_rate_v25[3] = {11025, 12000, 8000};
    static const int bitrate_v1_l3[16] = {
        0, 32, 40, 48, 56, 64, 80, 96,
        112, 128, 160, 192, 224, 256, 320, 0
    };
    static const int bitrate_v2_l3[16] = {
        0, 8, 16, 24, 32, 40, 48, 56,
        64, 80, 96, 112, 128, 144, 160, 0
    };

    int sr = 0;
    if (version_id == 3) {
        sr = sample_rate_v1[sample_idx];
    } else if (version_id == 2) {
        sr = sample_rate_v2[sample_idx];
    } else if (version_id == 0) {
        sr = sample_rate_v25[sample_idx];
    }
    if (sr <= 0) {
        return false;
    }

    int kbps = (version_id == 3) ? bitrate_v1_l3[bitrate_idx] : bitrate_v2_l3[bitrate_idx];
    if (kbps <= 0) {
        return false;
    }

    int bitrate = kbps * 1000;
    int coeff = (version_id == 3) ? 144 : 72;
    int samples = (version_id == 3) ? 1152 : 576;
    int size = (coeff * bitrate) / sr + padding;
    if (size <= 4) {
        return false;
    }

    *sample_rate = sr;
    *frame_size = size;
    *samples_per_frame = samples;
    return true;
}

static uint32_t read_be32(const uint8_t *buf)
{
    if (!buf) {
        return 0;
    }

    return ((uint32_t)buf[0] << 24) |
           ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8) |
           (uint32_t)buf[3];
}

static bool mp3_gapless_trim_samples_from_frame(FILE *f,
                                                long frame_offset,
                                                uint32_t header,
                                                int frame_size,
                                                uint32_t *out_trim_samples)
{
    if (!f || !out_trim_samples || frame_offset < 0 || frame_size <= 0) {
        return false;
    }

    uint8_t version_id = (uint8_t)((header >> 19) & 0x3);
    uint8_t channel_mode = (uint8_t)((header >> 6) & 0x3);
    bool mono = (channel_mode == 3);
    size_t side_info_size = 0;
    if (version_id == 3) {
        side_info_size = mono ? 17U : 32U;
    } else if (version_id == 2 || version_id == 0) {
        side_info_size = mono ? 9U : 17U;
    } else {
        return false;
    }

    size_t xing_offset = 4U + side_info_size;
    if ((size_t)frame_size < (xing_offset + 8U) || frame_size > 2048) {
        return false;
    }

    uint8_t frame_buf[2048];
    long resume_pos = ftell(f);
    if (resume_pos < 0) {
        return false;
    }
    if (fseek(f, frame_offset, SEEK_SET) != 0) {
        return false;
    }

    size_t read_bytes = fread(frame_buf, 1, (size_t)frame_size, f);
    bool restored = (fseek(f, resume_pos, SEEK_SET) == 0);
    if (read_bytes != (size_t)frame_size || !restored) {
        return false;
    }

    const uint8_t *xing = frame_buf + xing_offset;
    if (memcmp(xing, "Xing", 4) != 0 && memcmp(xing, "Info", 4) != 0) {
        return false;
    }

    size_t cursor = xing_offset + 4U;
    uint32_t flags = read_be32(frame_buf + cursor);
    cursor += 4U;

    if (flags & 0x1U) {
        cursor += 4U;
    }
    if (flags & 0x2U) {
        cursor += 4U;
    }
    if (flags & 0x4U) {
        cursor += 100U;
    }
    if (flags & 0x8U) {
        cursor += 4U;
    }

    if (cursor + 24U > (size_t)frame_size) {
        return false;
    }

    const uint8_t *lame = frame_buf + cursor;
    if (memcmp(lame, "LAME", 4) != 0) {
        return false;
    }

    const uint8_t *delay_bytes = lame + 21U;
    uint32_t encoder_delay = ((uint32_t)delay_bytes[0] << 4) |
                             ((uint32_t)delay_bytes[1] >> 4);
    uint32_t encoder_padding = (((uint32_t)delay_bytes[1] & 0x0FU) << 8) |
                               (uint32_t)delay_bytes[2];
    *out_trim_samples = encoder_delay + encoder_padding;
    return (*out_trim_samples > 0U);
}

static esp_err_t mp3_duration_ms_from_path(const char *path, uint32_t *out_duration_ms)
{
    if (!path || !out_duration_ms) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }

    size_t start_offset = 0;
    uint8_t id3_header[10] = {0};
    size_t read_bytes = fread(id3_header, 1, sizeof(id3_header), f);
    if (read_bytes == sizeof(id3_header) &&
        id3_header[0] == 'I' &&
        id3_header[1] == 'D' &&
        id3_header[2] == '3') {
        bool valid_syncsafe = ((id3_header[6] & 0x80) == 0) &&
                              ((id3_header[7] & 0x80) == 0) &&
                              ((id3_header[8] & 0x80) == 0) &&
                              ((id3_header[9] & 0x80) == 0);
        if (valid_syncsafe) {
            size_t tag_size = ((size_t)id3_header[6] << 21) |
                              ((size_t)id3_header[7] << 14) |
                              ((size_t)id3_header[8] << 7) |
                              (size_t)id3_header[9];
            start_offset = 10 + tag_size;
            if (id3_header[5] & 0x10) {
                start_offset += 10;
            }
        }
    }

    if (fseek(f, (long)start_offset, SEEK_SET) != 0) {
        fclose(f);
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t window[4] = {0};
    if (fread(window, 1, sizeof(window), f) != sizeof(window)) {
        fclose(f);
        return ESP_ERR_INVALID_RESPONSE;
    }

    bool synced = false;
    uint64_t total_samples = 0;
    int final_sample_rate = 0;
    uint32_t gapless_trim_samples = 0;
    bool have_gapless_trim = false;

    while (true) {
        uint32_t header = ((uint32_t)window[0] << 24) |
                          ((uint32_t)window[1] << 16) |
                          ((uint32_t)window[2] << 8) |
                          (uint32_t)window[3];

        int sample_rate = 0;
        int frame_size = 0;
        int samples_per_frame = 0;
        if (parse_mp3_header(header, &sample_rate, &frame_size, &samples_per_frame)) {
            if (!synced) {
                long frame_offset = ftell(f) - (long)sizeof(window);
                if (!have_gapless_trim &&
                    mp3_gapless_trim_samples_from_frame(f,
                                                        frame_offset,
                                                        header,
                                                        frame_size,
                                                        &gapless_trim_samples)) {
                    have_gapless_trim = true;
                }
            }
            synced = true;
            final_sample_rate = sample_rate;
            total_samples += (uint64_t)samples_per_frame;

            if (fseek(f, frame_size - 4, SEEK_CUR) != 0) {
                break;
            }
            if (fread(window, 1, sizeof(window), f) != sizeof(window)) {
                break;
            }
            continue;
        }

        if (synced) {
            break;
        }

        int next = fgetc(f);
        if (next == EOF) {
            break;
        }
        window[0] = window[1];
        window[1] = window[2];
        window[2] = window[3];
        window[3] = (uint8_t)next;
    }

    fclose(f);
    if (!synced || total_samples == 0 || final_sample_rate <= 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (have_gapless_trim && total_samples > (uint64_t)gapless_trim_samples) {
        total_samples -= (uint64_t)gapless_trim_samples;
    }

    uint64_t duration_ms = (total_samples * 1000ULL) / (uint64_t)final_sample_rate;
    if (duration_ms > UINT32_MAX) {
        duration_ms = UINT32_MAX;
    }
    *out_duration_ms = (uint32_t)duration_ms;
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
    hdr->trim_start_ms = props->trim_start_ms;
    hdr->trim_stop_ms = props->trim_stop_ms;
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

    file_entry_meta_t hdr = {0};
    size_t read_bytes = fread(&hdr, 1, sizeof(hdr), f);
    fclose(f);
    if (read_bytes < sizeof(file_entry_meta_v2_t) || hdr.magic != FILE_ENTRY_MAGIC) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    files_props_init(out, basename_from_path(audio_path));
    if (hdr.version == FILE_ENTRY_VERSION_LEGACY) {
        if (read_bytes != sizeof(file_entry_meta_v2_t)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        file_entry_meta_v2_t *legacy = (file_entry_meta_v2_t *)&hdr;
        out->probability = clamp_probability(legacy->probability);
        out->volume = clamp_volume(legacy->volume);
        out->enabled = legacy->enabled ? true : false;
        out->trim_start_ms = 0;
        out->trim_stop_ms = 0;
        return ESP_OK;
    }

    if (hdr.version != FILE_ENTRY_VERSION || read_bytes != sizeof(hdr)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    out->probability = clamp_probability(hdr.probability);
    out->volume = clamp_volume(hdr.volume);
    out->enabled = hdr.enabled ? true : false;
    out->trim_start_ms = hdr.trim_start_ms;
    out->trim_stop_ms = hdr.trim_stop_ms;
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
    props->trim_start_ms = 0;
    props->trim_stop_ms = 0;
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

void files_props_set_trim_ms(file_properties_t *props, uint32_t trim_start_ms, uint32_t trim_stop_ms)
{
    if (!props) {
        return;
    }
    props->trim_start_ms = trim_start_ms;
    props->trim_stop_ms = trim_stop_ms;
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

esp_err_t files_get_audio_duration_ms(const char *name, uint32_t *out_duration_ms)
{
    if (!name || !out_duration_ms) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_base_path[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

#if CONFIG_TEST_AUDIO_MOCK_BACKEND
    uint32_t mock_duration_ms = mock_playback_duration_from_name_ms(name);
    if (mock_duration_ms > 0) {
        *out_duration_ms = mock_duration_ms;
        return ESP_OK;
    }
#endif

    char audio_path[FILES_PATH_MAX];
    esp_err_t err = full_path_for_name(name, audio_path, sizeof(audio_path));
    if (err == ESP_OK) {
        esp_err_t duration_err = mp3_duration_ms_from_path(audio_path, out_duration_ms);
        if (duration_err == ESP_OK) {
            return ESP_OK;
        }
        err = duration_err;
    }

    return err;
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
