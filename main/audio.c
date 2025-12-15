#include "audio.h"

#include <string.h>
#include <unistd.h>

#include "audio_common.h"
#include "audio_element.h"
#include "audio_event_iface.h"
#include "audio_pipeline.h"
#include "driver/gpio.h"
#include "esp_idf_version.h"
#if (ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0))
#include "driver/i2s.h"
#endif
#include "esp_log.h"
#include "esp_vfs.h"
#include "files.h"
#include "fatfs_stream.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2s_stream.h"
#include "mp3_decoder.h"
#include <sys/stat.h>

#define TAG "audio"

// GPIO layout matches the PCB used for the previous I2S test
#define GPIO_I2S_DATA GPIO_NUM_17
#define GPIO_I2S_WS   GPIO_NUM_18
#define GPIO_I2S_BCK  GPIO_NUM_19
#define GPIO_MUTE_DAC GPIO_NUM_21  // active low (keep high to unmute)
#define GPIO_MUTE_AMP GPIO_NUM_22  // active high

#define AUDIO_MIN_DB           (-80)
#define AUDIO_MAX_DB           (0)
#define AUDIO_PLAY_TASK_STACK  (4096)
#define AUDIO_PLAY_TASK_PRIO   (tskIDLE_PRIORITY + 5)

static char s_base_path[ESP_VFS_PATH_MAX] = {0};
static char s_test_file_path[ESP_VFS_PATH_MAX] = {0};
static bool s_initialized;

static TaskHandle_t s_play_task;
static volatile bool s_playing;
static volatile bool s_stop_requested;
static volatile bool s_volume_dirty = true;

static audio_pipeline_handle_t s_pipeline;
static audio_element_handle_t s_stream_reader;
static audio_element_handle_t s_decoder;
static audio_element_handle_t s_i2s_stream;
static audio_event_iface_handle_t s_evt;
static size_t s_current_file_size;
static size_t s_test_offset_bytes;

static bool s_pins_configured;

static volatile uint8_t s_master_volume = 35;
static volatile uint8_t s_track_volume = 35;
static volatile uint8_t s_lid_open_volume = 25;
static volatile bool s_lid_open;
static int s_current_volume_db = AUDIO_TEST_QUIET_DB;

static void configure_mute_pins_once(void)
{
    if (s_pins_configured) {
        return;
    }
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << GPIO_MUTE_AMP) | (1ULL << GPIO_MUTE_DAC),
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = 0,
        .pull_up_en = 0,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(GPIO_MUTE_AMP, 1);  // mute amp (active high)
    gpio_set_level(GPIO_MUTE_DAC, 0);  // mute DAC (active low)
    s_pins_configured = true;
}

static void mute_outputs(void)
{
    if (!s_pins_configured) {
        return;
    }
    gpio_set_level(GPIO_MUTE_DAC, 0);  // mute DAC first
    gpio_set_level(GPIO_MUTE_AMP, 1);
}

static void unmute_outputs(void)
{
    if (!s_pins_configured) {
        return;
    }
    gpio_set_level(GPIO_MUTE_AMP, 0);
    vTaskDelay(pdMS_TO_TICKS(20));  // allow amp to wake before enabling DAC ramp
    gpio_set_level(GPIO_MUTE_DAC, 1);  // unmute DAC (built-in ramp)
    vTaskDelay(pdMS_TO_TICKS(30));
}

static inline uint8_t clamp_percent(uint8_t value)
{
    return (value > 100) ? 100 : value;
}

static int compute_volume_db(void)
{
    float master = (float)s_master_volume / 100.0f;
    float track = (float)s_track_volume / 100.0f;
    float lid = s_lid_open ? ((float)s_lid_open_volume / 100.0f) : 1.0f;

    float effective_pct = master * track * lid * 100.0f;
    if (effective_pct > 100.0f) {
        effective_pct = 100.0f;
    }
    float db = -60.0f + (0.6f * effective_pct);  // map 0-100% to roughly -60..0 dB
    if (db < AUDIO_MIN_DB) db = AUDIO_MIN_DB;
    if (db > AUDIO_MAX_DB) db = AUDIO_MAX_DB;
    return (int)db;
}

static void set_stream_volume_db(int db, bool log_change)
{
    if (!s_i2s_stream) {
        return;
    }

    if (db < AUDIO_MIN_DB) db = AUDIO_MIN_DB;
    if (db > AUDIO_MAX_DB) db = AUDIO_MAX_DB;

    if (i2s_alc_volume_set(s_i2s_stream, db) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set I2S volume");
        return;
    }

    s_current_volume_db = db;
    if (log_change) {
        ESP_LOGI(TAG, "Volume set to %d dB (master=%u track=%u lid_open=%u open=%s)",
                 s_current_volume_db,
                 (unsigned)s_master_volume,
                 (unsigned)s_track_volume,
                 (unsigned)s_lid_open_volume,
                 s_lid_open ? "true" : "false");
    }
}

static void apply_volume_to_stream(void)
{
    int target_db = compute_volume_db();
    s_volume_dirty = false;

    if (!s_i2s_stream) {
        return;
    }

    set_stream_volume_db(target_db, true);
}

static void destroy_pipeline(void)
{
    if (s_pipeline) {
        audio_pipeline_stop(s_pipeline);
        audio_pipeline_wait_for_stop(s_pipeline);
        audio_pipeline_terminate(s_pipeline);
    }

    if (s_pipeline && s_stream_reader) {
        audio_pipeline_unregister(s_pipeline, s_stream_reader);
    }
    if (s_pipeline && s_decoder) {
        audio_pipeline_unregister(s_pipeline, s_decoder);
    }
    if (s_pipeline && s_i2s_stream) {
        audio_pipeline_unregister(s_pipeline, s_i2s_stream);
    }

    if (s_pipeline) {
        audio_pipeline_remove_listener(s_pipeline);
        audio_pipeline_deinit(s_pipeline);
    }
    if (s_evt) {
        audio_event_iface_destroy(s_evt);
    }
    if (s_stream_reader) {
        audio_element_deinit(s_stream_reader);
    }
    if (s_decoder) {
        audio_element_deinit(s_decoder);
    }
    if (s_i2s_stream) {
        audio_element_deinit(s_i2s_stream);
    }

    s_pipeline = NULL;
    s_stream_reader = NULL;
    s_decoder = NULL;
    s_i2s_stream = NULL;
    s_evt = NULL;
}

static void audio_play_task(void *arg)
{
    (void)arg;
    esp_err_t err = ESP_OK;

    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    s_pipeline = audio_pipeline_init(&pipeline_cfg);
    if (!s_pipeline) {
        err = ESP_ERR_NO_MEM;
        goto exit;
    }

    fatfs_stream_cfg_t fs_cfg = FATFS_STREAM_CFG_DEFAULT();
    fs_cfg.type = AUDIO_STREAM_READER;
    s_stream_reader = fatfs_stream_init(&fs_cfg);
    if (!s_stream_reader) {
        err = ESP_ERR_NO_MEM;
        goto exit;
    }

    mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
    s_decoder = mp3_decoder_init(&mp3_cfg);
    if (!s_decoder) {
        err = ESP_ERR_NO_MEM;
        goto exit;
    }

    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_cfg.use_alc = true;
    i2s_cfg.volume = s_current_volume_db;
#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0))
    i2s_cfg.std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    i2s_cfg.std_cfg.gpio_cfg.bclk = GPIO_I2S_BCK;
    i2s_cfg.std_cfg.gpio_cfg.ws = GPIO_I2S_WS;
    i2s_cfg.std_cfg.gpio_cfg.dout = GPIO_I2S_DATA;
    i2s_cfg.std_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
#endif
    s_i2s_stream = i2s_stream_init(&i2s_cfg);
    if (!s_i2s_stream) {
        err = ESP_ERR_NO_MEM;
        goto exit;
    }
#if (ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0))
    i2s_pin_config_t pin_cfg = {
        .bck_io_num = GPIO_I2S_BCK,
        .ws_io_num = GPIO_I2S_WS,
        .data_out_num = GPIO_I2S_DATA,
        .data_in_num = I2S_PIN_NO_CHANGE,
#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 4, 0))
        .mck_io_num = I2S_PIN_NO_CHANGE,
#endif
    };
    if (i2s_set_pin(i2s_cfg.i2s_port, &pin_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set I2S pins; MP3 output may be muted");
    }
#endif

    audio_pipeline_register(s_pipeline, s_stream_reader, "file");
    audio_pipeline_register(s_pipeline, s_decoder, "mp3");
    audio_pipeline_register(s_pipeline, s_i2s_stream, "i2s");

    const char *link_tag[3] = {"file", "mp3", "i2s"};
    audio_pipeline_link(s_pipeline, link_tag, 3);

    audio_element_set_uri(s_stream_reader, s_test_file_path);
    audio_element_info_t el_info = {0};
    audio_element_getinfo(s_stream_reader, &el_info);
    el_info.byte_pos = (int64_t)s_test_offset_bytes;
    el_info.total_bytes = s_current_file_size;
    audio_element_setinfo(s_stream_reader, &el_info);

    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    s_evt = audio_event_iface_init(&evt_cfg);
    if (!s_evt) {
        err = ESP_ERR_NO_MEM;
        goto exit;
    }
    audio_pipeline_set_listener(s_pipeline, s_evt);

    ESP_LOGI(TAG, "Starting MP3 test playback: %s", s_test_file_path);
    err = audio_pipeline_run(s_pipeline);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio_pipeline_run failed: %s", esp_err_to_name(err));
        goto exit;
    }

    s_stop_requested = false;
    s_volume_dirty = true;
    s_playing = true;

    bool outputs_unmuted = false;
    while (!s_stop_requested) {
        audio_event_iface_msg_t msg;
        esp_err_t res = audio_event_iface_listen(s_evt, &msg, pdMS_TO_TICKS(500));
        if (res == ESP_ERR_TIMEOUT) {
            if (s_volume_dirty) {
                apply_volume_to_stream();
            }
            continue;
        }
        if (res != ESP_OK) {
            continue;
        }

        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT &&
            msg.source == (void *)s_decoder &&
            msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
            audio_element_info_t music_info = {0};
            audio_element_getinfo(s_decoder, &music_info);
            ESP_LOGI(TAG, "MP3 info: %d Hz, %d bit, %d ch",
                     (int)music_info.sample_rates,
                     music_info.bits,
                     music_info.channels);
            audio_element_setinfo(s_i2s_stream, &music_info);
            i2s_stream_set_clk(s_i2s_stream, music_info.sample_rates, music_info.bits, music_info.channels);
            apply_volume_to_stream();
            if (!outputs_unmuted) {
                unmute_outputs();
                outputs_unmuted = true;
            }
        }

        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT &&
            msg.cmd == AEL_MSG_CMD_REPORT_STATUS) {
            int state = (int)msg.data;
            if (msg.source == (void *)s_i2s_stream &&
                // Wait for the sink to finish so we don't cut off buffered audio
                (state == AEL_STATUS_STATE_FINISHED || state == AEL_STATUS_STATE_STOPPED)) {
                ESP_LOGI(TAG, "Playback finished");
                break;
            }
        }
    }

exit:
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Stopping MP3 playback due to error: %s", esp_err_to_name(err));
    }
    mute_outputs();
    destroy_pipeline();
    s_stop_requested = false;
    s_playing = false;
    s_play_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t audio_init(const char *base_path)
{
    if (!base_path) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t len = strlcpy(s_base_path, base_path, sizeof(s_base_path));
    if (len == 0 || len >= sizeof(s_base_path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    int full_len = snprintf(s_test_file_path, sizeof(s_test_file_path), "%s/%s", s_base_path, AUDIO_TEST_FILENAME);
    if (full_len < 0 || (size_t)full_len >= sizeof(s_test_file_path)) {
        s_base_path[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }

    configure_mute_pins_once();
    mute_outputs();
    s_initialized = true;
    ESP_LOGI(TAG, "Audio initialized with base path %s", s_base_path);
    return ESP_OK;
}

static esp_err_t load_meta_or_stat(const char *name)
{
    struct stat st = {0};
    if (stat(s_test_file_path, &st) != 0 || st.st_size <= 0) {
        ESP_LOGE(TAG, "Audio file not found or invalid at %s", s_test_file_path);
        return ESP_ERR_NOT_FOUND;
    }

    file_properties_t props;
    esp_err_t hdr_err = files_read_meta(name, &props);
    s_test_offset_bytes = 0;
    s_current_file_size = (size_t)st.st_size;
    if (hdr_err != ESP_OK) {
        ESP_LOGW(TAG, "Playing raw file without header: %s (size=%zu)", s_test_file_path, s_current_file_size);
    }
    ESP_LOGI(TAG, "Using payload size=%zu bytes", s_current_file_size);
    return ESP_OK;
}

esp_err_t audio_start_file(const char *name)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "audio_init not called");
        return ESP_ERR_INVALID_STATE;
    }
    if (!name || !name[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strchr(name, '/') || strstr(name, "..")) {
        ESP_LOGE(TAG, "Invalid filename: %s", name);
        return ESP_ERR_INVALID_ARG;
    }

    // Stop any in-flight playback first
    if (s_play_task) {
        audio_stop();
        for (int i = 0; i < 100 && s_play_task; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (s_play_task) {
            ESP_LOGE(TAG, "Previous audio task did not stop in time");
            return ESP_ERR_INVALID_STATE;
        }
    }

    int full_len = snprintf(s_test_file_path, sizeof(s_test_file_path), "%s/%s", s_base_path, name);
    if (full_len < 0 || (size_t)full_len >= sizeof(s_test_file_path)) {
        ESP_LOGE(TAG, "File path too long: %s/%s", s_base_path, name);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t meta_err = load_meta_or_stat(name);
    if (meta_err != ESP_OK) {
        return meta_err;
    }

    ESP_LOGI(TAG, "Starting playback: %s", name);

    s_stop_requested = false;
    BaseType_t res = xTaskCreate(audio_play_task,
                                 "mp3-play",
                                 AUDIO_PLAY_TASK_STACK,
                                 NULL,
                                 AUDIO_PLAY_TASK_PRIO,
                                 &s_play_task);
    if (res != pdPASS) {
        s_play_task = NULL;
        ESP_LOGE(TAG, "Failed to create audio play task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void audio_stop(void)
{
    if (!s_play_task) {
        return;
    }
    s_stop_requested = true;
    for (int i = 0; i < 100 && s_play_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_play_task) {
        ESP_LOGW(TAG, "Audio task did not stop in time");
    }
}

bool audio_is_playing(void)
{
    return s_playing;
}

void set_master_volume_level(uint8_t level)
{
    s_master_volume = clamp_percent(level);
    s_volume_dirty = true;
}

void set_track_volume_level(uint8_t level)
{
    s_track_volume = clamp_percent(level);
    s_volume_dirty = true;
}

void set_lid_open_volume_level(uint8_t level)
{
    s_lid_open_volume = clamp_percent(level);
    s_volume_dirty = true;
}

void set_lid_level(bool lid_open)
{
    s_lid_open = lid_open;
    s_volume_dirty = true;
}

TaskHandle_t create_play_audio_task(void)
{
    if (s_play_task) {
        return s_play_task;
    }
    if (audio_start_file(AUDIO_TEST_FILENAME) == ESP_OK) {
        return s_play_task;
    }
    return NULL;
}
