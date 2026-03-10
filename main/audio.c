#include "audio.h"

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "audio_common.h"
#include "audio_element.h"
#include "audio_event_iface.h"
#include "audio_pipeline.h"
#include "board.h"
#include "driver/i2s.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs.h"
#include "fatfs_stream.h"
#include "files.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gpio.h"
#include "i2s_stream.h"
#include "mp3_decoder.h"
#include "raw_stream.h"

#define TAG "audio"

#ifndef AUDIO_ENABLE_FADE_OUT
#define AUDIO_ENABLE_FADE_OUT 1
#endif

///////////////////////////////////////////////////////////////////////////////////////////////////
// Constants
///////////////////////////////////////////////////////////////////////////////////////////////////

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define GPIO_I2S_DATA GPIO_NUM_17
#define GPIO_I2S_WS   GPIO_NUM_18
#define GPIO_I2S_BCK  GPIO_NUM_19

#define AUDIO_MIN_DB           (-80)
#define AUDIO_MAX_DB           (0)
#define AUDIO_PLAY_TASK_STACK  (4096)
#define AUDIO_PLAY_TASK_PRIO   (tskIDLE_PRIORITY + 5)
#define AUDIO_PLAY_EVENT_WAIT_MS (50)
#define AUDIO_MP3_DECODER_OUT_RB_SIZE (8 * 1024)
#define AUDIO_MP3_PREFETCH_SCAN_LIMIT (64 * 1024)
#define AUDIO_TEST_TASK_STACK  (4096)
#define AUDIO_TEST_TASK_PRIO   (tskIDLE_PRIORITY + 4)
#define AUDIO_TEST_SWEEP_DURATION_MS (6000)
#define TONE_TABLE_LEN         (256)
#define AUDIO_FILE_PATH_MAX    (ESP_VFS_PATH_MAX + FILE_ENTRY_NAME_MAX + 2)
#define AUDIO_MP3_PCM_BITS     (16)

///////////////////////////////////////////////////////////////////////////////////////////////////
// Static Vars
///////////////////////////////////////////////////////////////////////////////////////////////////

// Module
static bool s_initialized;
static audio_mode_t s_mode = AUDIO_MODE_UNINITIALIZED;
static SemaphoreHandle_t s_lock;

// FS
static char s_base_path[AUDIO_FILE_PATH_MAX] = {0};
static char s_current_file_path[AUDIO_FILE_PATH_MAX] = {0};
static char s_current_file_name[FILE_ENTRY_NAME_MAX] = {0};

// Volumes
static volatile uint8_t s_master_volume = 35;
static volatile uint8_t s_track_volume = 35;
static volatile bool s_volume_dirty = true;
static int s_current_volume_db = AUDIO_TEST_QUIET_DB;

// Playback mode runtime
static TaskHandle_t s_play_task;
static volatile bool s_playing;
static volatile bool s_play_stop_requested;
static size_t s_current_file_size;
static size_t s_current_offset_bytes;
static uint32_t s_playback_skip_notice_seq;
static uint64_t s_playback_skip_notice_timestamp_ms;
static audio_playback_skip_reason_t s_playback_skip_notice_reason;
static char s_playback_skip_notice_name[FILE_ENTRY_NAME_MAX] = {0};
static audio_pipeline_handle_t s_pipeline;
static audio_element_handle_t s_stream_reader;
static audio_element_handle_t s_decoder;
static audio_element_handle_t s_playback_i2s_stream;
static audio_event_iface_handle_t s_evt;

// Test mode runtime
static TaskHandle_t s_tone_task;
static TaskHandle_t s_sweep_task;
static volatile bool s_test_stop_requested;
static volatile bool s_sweep_stop_requested;
static volatile float s_target_freq_hz = AUDIO_TEST_DEFAULT_HZ;
static volatile float s_target_volume_pct = AUDIO_TEST_DEFAULT_VOLUME_PCT;
static volatile uint16_t s_target_amp = AUDIO_TEST_DEFAULT_AMPLITUDE;
static bool s_test_i2s_configured;
static audio_pipeline_handle_t s_test_pipeline;
static audio_element_handle_t s_test_raw_stream;

///////////////////////////////////////////////////////////////////////////////////////////////////
// Forward Declarations
///////////////////////////////////////////////////////////////////////////////////////////////////

static void mute_outputs(void);
static esp_err_t switch_mode_locked(audio_mode_t next_mode);

static esp_err_t configure_test_i2s_locked(void);
static void deconfigure_test_i2s_locked(void);
static void destroy_test_pipeline_locked(void);
static void stop_tone_task_locked(void);
static void stop_sweep_task_locked(void);
static esp_err_t restart_test_pipeline_locked(void);
static bool write_test_stream_locked(volatile bool *stop_requested,
                                     const int16_t *buffer,
                                     size_t size_bytes,
                                     const char *context);
static void tone_task(void *arg);
static void sweep_task(void *arg);

static void destroy_active_playback_pipeline(bool keep_i2s);
static void set_stream_volume_db(int db, bool log_change);
static esp_err_t init_playback_i2s_stream_locked(void);
static void deinit_playback_i2s_stream_locked(void);
static bool prefetch_mp3_format(const char *path, int *sample_rate, int *bits, int *channels);
static esp_err_t load_meta_or_stat_locked(const char *name);
static audio_playback_skip_reason_t playback_skip_reason_locked(void);
static void clear_playback_skip_notice_locked(void);
static void record_playback_skip_notice_locked(const char *name, audio_playback_skip_reason_t reason);
static esp_err_t audio_start_file_internal(const char *name,
                                           const uint8_t *override_volume_pct,
                                           audio_playback_start_result_t *out_result,
                                           audio_playback_skip_reason_t *out_skip_reason);
static void play_task(void *arg);
static void stop_playback_task_locked(void);

///////////////////////////////////////////////////////////////////////////////////////////////////
// Common
///////////////////////////////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------
// Mode Switching
//-------------------------------------------------------------------------

static inline void audio_lock(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static inline void audio_unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

static esp_err_t switch_mode_locked(audio_mode_t next_mode)
{
    if (s_mode == next_mode) {
        return ESP_OK;
    }

    if (next_mode == AUDIO_MODE_PLAYBACK) {
        stop_tone_task_locked();
        stop_sweep_task_locked();
#if !CONFIG_TEST_AUDIO_MOCK_BACKEND
        deconfigure_test_i2s_locked();

        esp_err_t err = init_playback_i2s_stream_locked();
        if (err != ESP_OK) {
            return err;
        }
#endif

        mute_outputs();
        s_mode = AUDIO_MODE_PLAYBACK;
        ESP_LOGI(TAG, "Audio mode -> PLAYBACK");
        return ESP_OK;
    }

    if (next_mode == AUDIO_MODE_TEST) {
        stop_playback_task_locked();
#if !CONFIG_TEST_AUDIO_MOCK_BACKEND
        destroy_active_playback_pipeline(true);
        deinit_playback_i2s_stream_locked();

        esp_err_t err = configure_test_i2s_locked();
        if (err != ESP_OK) {
            return err;
        }
#endif

        mute_outputs();
        s_mode = AUDIO_MODE_TEST;
        ESP_LOGI(TAG, "Audio mode -> TEST");
        return ESP_OK;
    }

    return ESP_ERR_INVALID_ARG;
}

//-------------------------------------------------------------------------
// Muting
//-------------------------------------------------------------------------

static void mute_outputs(void)
{
    audio_board_outputs_mute();
}

static void unmute_outputs(void)
{
    audio_board_outputs_unmute();
}

//-------------------------------------------------------------------------
// Public
//-------------------------------------------------------------------------

esp_err_t audio_init(void)
{
    const char *base_path = CONFIG_BASE_PATH;
    if (!base_path || base_path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    size_t len = strlcpy(s_base_path, base_path, sizeof(s_base_path));
    if (len == 0 || len >= sizeof(s_base_path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }

    audio_lock();

    esp_err_t err = audio_board_outputs_init();
    if (err != ESP_OK) {
        audio_unlock();
        return err;
    }

    mute_outputs();

    s_initialized = true;
    s_mode = AUDIO_MODE_UNINITIALIZED;

    err = switch_mode_locked(AUDIO_MODE_PLAYBACK);
    if (err != ESP_OK) {
        audio_unlock();
        return err;
    }

    ESP_LOGI(TAG, "Audio initialized. Default mode: PLAYBACK");
    audio_unlock();
    return ESP_OK;
}

audio_mode_t audio_mode(void)
{
    return s_mode;
}

void audio_stop(void)
{
    audio_lock();
    stop_playback_task_locked();
    stop_tone_task_locked();
    stop_sweep_task_locked();
    mute_outputs();
    audio_unlock();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Test Mode
///////////////////////////////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------
// Helpers
//-------------------------------------------------------------------------

static float clamp_freq(float hz)
{
    if (isnan(hz) || hz <= 0.0f) {
        return AUDIO_TEST_DEFAULT_HZ;
    }
    if (hz < AUDIO_TEST_MIN_HZ) {
        return AUDIO_TEST_MIN_HZ;
    }
    if (hz > AUDIO_TEST_MAX_HZ) {
        return AUDIO_TEST_MAX_HZ;
    }
    return hz;
}

static float clamp_volume_pct(float volume_pct)
{
    if (!isfinite(volume_pct) || volume_pct < 0.0f) {
        return 0.0f;
    }
    if (volume_pct > 100.0f) {
        return 100.0f;
    }
    return volume_pct;
}

static uint16_t clamp_amp(uint16_t amp)
{
    if (amp > AUDIO_TEST_SINE_AMPLITUDE) {
        return AUDIO_TEST_SINE_AMPLITUDE;
    }
    return amp;
}

//-------------------------------------------------------------------------
// Tone Generation
//-------------------------------------------------------------------------

static void fill_tone_block(int16_t *buffer, float *phase, float freq_hz)
{
    const float amplitude = (float)s_target_amp;
    const float phase_step = (2.0f * (float)M_PI * freq_hz) / (float)AUDIO_TEST_SAMPLE_RATE;

    for (int i = 0; i < TONE_TABLE_LEN; i++) {
        *phase += phase_step;
        if (*phase > (2.0f * (float)M_PI)) {
            *phase -= (2.0f * (float)M_PI);
        }

        int16_t sample = (int16_t)(amplitude * sinf(*phase));
        buffer[2 * i] = sample;
        buffer[2 * i + 1] = sample;
    }
}

//-------------------------------------------------------------------------
// I2S
//-------------------------------------------------------------------------

static esp_err_t configure_test_i2s_locked(void)
{
    if (s_test_i2s_configured) {
        return ESP_OK;
    }

    esp_err_t err = init_playback_i2s_stream_locked();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init test I2S stream: %s", esp_err_to_name(err));
        return err;
    }

    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type = AUDIO_STREAM_WRITER;
    s_test_raw_stream = raw_stream_init(&raw_cfg);
    if (!s_test_raw_stream) {
        return ESP_ERR_NO_MEM;
    }

    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    s_test_pipeline = audio_pipeline_init(&pipeline_cfg);
    if (!s_test_pipeline) {
        destroy_test_pipeline_locked();
        return ESP_ERR_NO_MEM;
    }

    err = audio_pipeline_register(s_test_pipeline, s_test_raw_stream, "raw");
    if (err != ESP_OK) {
        destroy_test_pipeline_locked();
        return err;
    }
    err = audio_pipeline_register(s_test_pipeline, s_playback_i2s_stream, "i2s");
    if (err != ESP_OK) {
        destroy_test_pipeline_locked();
        return err;
    }

    const char *link_tag[2] = {"raw", "i2s"};
    err = audio_pipeline_link(s_test_pipeline, link_tag, 2);
    if (err != ESP_OK) {
        destroy_test_pipeline_locked();
        return err;
    }

    err = i2s_stream_set_clk(s_playback_i2s_stream,
                             AUDIO_TEST_SAMPLE_RATE,
                             16,
                             2);
    if (err != ESP_OK) {
        destroy_test_pipeline_locked();
        return err;
    }

    // Test tone should run at full stream gain; playback volume policy is applied
    // separately when switching back to playback mode.
    set_stream_volume_db(AUDIO_MAX_DB, false);

    err = audio_pipeline_run(s_test_pipeline);
    if (err != ESP_OK) {
        destroy_test_pipeline_locked();
        return err;
    }

    s_test_i2s_configured = true;
    return ESP_OK;
}

static void deconfigure_test_i2s_locked(void)
{
    if (!s_test_i2s_configured) {
        return;
    }

    destroy_test_pipeline_locked();
    deinit_playback_i2s_stream_locked();
    s_test_i2s_configured = false;
}

static void destroy_test_pipeline_locked(void)
{
    if (s_test_pipeline) {
        audio_pipeline_stop(s_test_pipeline);
        audio_pipeline_wait_for_stop(s_test_pipeline);
        audio_pipeline_terminate(s_test_pipeline);
    }

    if (s_test_pipeline && s_test_raw_stream) {
        audio_pipeline_unregister(s_test_pipeline, s_test_raw_stream);
    }
    if (s_test_pipeline && s_playback_i2s_stream) {
        audio_pipeline_unregister(s_test_pipeline, s_playback_i2s_stream);
    }

    if (s_test_pipeline) {
        audio_pipeline_deinit(s_test_pipeline);
        s_test_pipeline = NULL;
    }
    if (s_test_raw_stream) {
        audio_element_deinit(s_test_raw_stream);
        s_test_raw_stream = NULL;
    }
}

static esp_err_t restart_test_pipeline_locked(void)
{
    deconfigure_test_i2s_locked();
    return configure_test_i2s_locked();
}

//-------------------------------------------------------------------------
// Tasks
//-------------------------------------------------------------------------

static bool write_test_stream_locked(volatile bool *stop_requested,
                                     const int16_t *buffer,
                                     size_t size_bytes,
                                     const char *context)
{
    const char *cursor = (const char *)buffer;
    size_t remaining = size_bytes;

    while (remaining > 0 && !(*stop_requested)) {
        int bytes_written = raw_stream_write(s_test_raw_stream, (char *)cursor, (int)remaining);
        if (bytes_written > 0) {
            cursor += bytes_written;
            remaining -= (size_t)bytes_written;
            continue;
        }

        if (bytes_written == AEL_IO_TIMEOUT) {
            continue;
        }
        if (bytes_written == AEL_IO_ABORT) {
            return false;
        }

        ESP_LOGE(TAG, "raw_stream_write failed during %s: %d", context, bytes_written);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    return remaining == 0;
}

static void tone_task(void *arg)
{
    (void)arg;

#if CONFIG_TEST_AUDIO_MOCK_BACKEND
    while (!s_test_stop_requested) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    mute_outputs();
    s_test_stop_requested = false;
    s_tone_task = NULL;
    vTaskDelete(NULL);
#else

    float phase = 0.0f;
    int16_t block[TONE_TABLE_LEN * 2];

    while (!s_test_stop_requested) {
        float freq = s_target_freq_hz;
        fill_tone_block(block, &phase, freq);

        if (!write_test_stream_locked(&s_test_stop_requested,
                                      block,
                                      sizeof(block),
                                      "tone")) {
            if (!s_test_stop_requested) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
    }

    mute_outputs();

    s_test_stop_requested = false;
    s_tone_task = NULL;
    vTaskDelete(NULL);
#endif
}

static void sweep_task(void *arg)
{
    (void)arg;

#if CONFIG_TEST_AUDIO_MOCK_BACKEND
    TickType_t start_tick = xTaskGetTickCount();
    TickType_t duration_ticks = pdMS_TO_TICKS(AUDIO_TEST_SWEEP_DURATION_MS);
    if (duration_ticks == 0) {
        duration_ticks = 1;
    }

    bool sweep_complete = true;
    while (!s_sweep_stop_requested) {
        TickType_t elapsed_ticks = xTaskGetTickCount() - start_tick;
        if (elapsed_ticks > duration_ticks) {
            break;
        }

        float progress = (float)elapsed_ticks / (float)duration_ticks;
        if (progress < 0.0f) {
            progress = 0.0f;
        }
        if (progress > 1.0f) {
            progress = 1.0f;
        }

        s_target_freq_hz = AUDIO_TEST_MIN_HZ + (AUDIO_TEST_MAX_HZ - AUDIO_TEST_MIN_HZ) * progress;
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (s_sweep_stop_requested) {
        sweep_complete = false;
    }

    if (sweep_complete) {
        s_target_freq_hz = AUDIO_TEST_MAX_HZ;
        ESP_LOGI(TAG, "Sweep finished (mock)");
    } else {
        ESP_LOGI(TAG, "Sweep stopped (mock)");
    }

    mute_outputs();
    s_sweep_stop_requested = false;
    s_sweep_task = NULL;
    vTaskDelete(NULL);
#else

    float phase = 0.0f;
    int16_t block[TONE_TABLE_LEN * 2];

    TickType_t start_tick = xTaskGetTickCount();
    TickType_t duration_ticks = pdMS_TO_TICKS(AUDIO_TEST_SWEEP_DURATION_MS);
    if (duration_ticks == 0) {
        duration_ticks = 1;
    }

    bool sweep_complete = true;
    while (!s_sweep_stop_requested) {
        TickType_t elapsed_ticks = xTaskGetTickCount() - start_tick;
        if (elapsed_ticks > duration_ticks) {
            break;
        }

        float progress = (float)elapsed_ticks / (float)duration_ticks;
        if (progress < 0.0f) {
            progress = 0.0f;
        }
        if (progress > 1.0f) {
            progress = 1.0f;
        }

        float freq = AUDIO_TEST_MIN_HZ + (AUDIO_TEST_MAX_HZ - AUDIO_TEST_MIN_HZ) * progress;
        s_target_freq_hz = freq;
        fill_tone_block(block, &phase, freq);

        if (!write_test_stream_locked(&s_sweep_stop_requested,
                                      block,
                                      sizeof(block),
                                      "sweep")) {
            if (!s_sweep_stop_requested) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
    }

    if (s_sweep_stop_requested) {
        sweep_complete = false;
    }

    if (sweep_complete) {
        s_target_freq_hz = AUDIO_TEST_MAX_HZ;
        ESP_LOGI(TAG, "Sweep finished");
    } else {
        ESP_LOGI(TAG, "Sweep stopped");
    }

    mute_outputs();

    s_sweep_stop_requested = false;
    s_sweep_task = NULL;
    vTaskDelete(NULL);
#endif
}

static void stop_tone_task_locked(void)
{
    if (!s_tone_task) {
        return;
    }

    s_test_stop_requested = true;
    if (s_test_raw_stream) {
        audio_element_abort_output_ringbuf(s_test_raw_stream);
    }
    for (int i = 0; i < 50 && s_tone_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (s_tone_task) {
        ESP_LOGW(TAG, "Tone task did not stop in time");
    }
}

static void stop_sweep_task_locked(void)
{
    if (!s_sweep_task) {
        return;
    }

    s_sweep_stop_requested = true;
    if (s_test_raw_stream) {
        audio_element_abort_output_ringbuf(s_test_raw_stream);
    }
    for (int i = 0; i < 80 && s_sweep_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (s_sweep_task) {
        ESP_LOGW(TAG, "Sweep task did not stop in time");
    }
}

//-------------------------------------------------------------------------
// Public
//-------------------------------------------------------------------------

void audio_test_set_targets(float freq_hz, uint16_t amplitude)
{
    s_target_freq_hz = clamp_freq(freq_hz);
    s_target_amp = clamp_amp(amplitude);
}

void audio_test_set_volume_pct(float volume_pct)
{
    s_target_volume_pct = clamp_volume_pct(volume_pct);
}

esp_err_t audio_test_start(float freq_hz, uint16_t amplitude)
{
    audio_lock();

    if (!s_initialized) {
        audio_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    audio_test_set_targets(freq_hz, amplitude);

    esp_err_t err = switch_mode_locked(AUDIO_MODE_TEST);
    if (err != ESP_OK) {
        audio_unlock();
        return err;
    }

    if (s_sweep_task) {
        audio_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    if (s_tone_task) {
        unmute_outputs();
        audio_unlock();
        return ESP_OK;
    }

#if !CONFIG_TEST_AUDIO_MOCK_BACKEND
    err = restart_test_pipeline_locked();
    if (err != ESP_OK) {
        audio_unlock();
        return err;
    }
#endif

    s_test_stop_requested = false;
    s_sweep_stop_requested = false;
    if (xTaskCreate(tone_task,
                    "audio-test",
                    AUDIO_TEST_TASK_STACK,
                    NULL,
                    AUDIO_TEST_TASK_PRIO,
                    &s_tone_task) != pdPASS) {
        s_tone_task = NULL;
        audio_unlock();
        return ESP_ERR_NO_MEM;
    }

    unmute_outputs();
    ESP_LOGI(TAG, "Started test tone at %.1f Hz (amp=%u)",
             (double)s_target_freq_hz,
             (unsigned)s_target_amp);

    audio_unlock();
    return ESP_OK;
}

esp_err_t audio_test_start_sweep(void)
{
    audio_lock();

    if (!s_initialized) {
        audio_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = switch_mode_locked(AUDIO_MODE_TEST);
    if (err != ESP_OK) {
        audio_unlock();
        return err;
    }

    if (s_sweep_task) {
        audio_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    stop_tone_task_locked();

#if !CONFIG_TEST_AUDIO_MOCK_BACKEND
    err = restart_test_pipeline_locked();
    if (err != ESP_OK) {
        audio_unlock();
        return err;
    }
#endif

    s_sweep_stop_requested = false;
    s_test_stop_requested = false;
    s_target_freq_hz = AUDIO_TEST_MIN_HZ;
    if (xTaskCreate(sweep_task,
                    "audio-sweep",
                    AUDIO_TEST_TASK_STACK,
                    NULL,
                    AUDIO_TEST_TASK_PRIO,
                    &s_sweep_task) != pdPASS) {
        s_sweep_task = NULL;
        audio_unlock();
        return ESP_ERR_NO_MEM;
    }

    unmute_outputs();
    ESP_LOGI(TAG, "Started one-shot sweep (%.1f Hz -> %.1f Hz, %d ms)",
             (double)AUDIO_TEST_MIN_HZ,
             (double)AUDIO_TEST_MAX_HZ,
             AUDIO_TEST_SWEEP_DURATION_MS);

    audio_unlock();
    return ESP_OK;
}

bool audio_test_is_running(void)
{
    return s_tone_task != NULL;
}

bool audio_test_sweep_is_running(void)
{
    return s_sweep_task != NULL;
}

float audio_test_current_freq(void)
{
    return s_target_freq_hz;
}

float audio_test_current_volume_pct(void)
{
    return s_target_volume_pct;
}

uint16_t audio_test_current_amplitude(void)
{
    return s_target_amp;
}

uint16_t audio_test_max_amplitude(void)
{
    return AUDIO_TEST_SINE_AMPLITUDE;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Playback Mode
///////////////////////////////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------
// Helpers
//-------------------------------------------------------------------------

static inline uint8_t clamp_percent(uint8_t value)
{
    return (value > 100) ? 100 : value;
}

static inline uint8_t clamp_track_volume(uint8_t value)
{
    return (value > FILE_VOLUME_MAX) ? FILE_VOLUME_MAX : value;
}

static inline bool playback_should_unmute(void)
{
    if (s_master_volume == 0) {
        return false;
    }
    if (s_track_volume == 0) {
        return false;
    }
    return true;
}

const char *audio_playback_skip_reason_text(audio_playback_skip_reason_t reason)
{
    switch (reason) {
        case AUDIO_PLAYBACK_SKIP_TRACK_VOLUME_ZERO:
            return "track volume is 0%";
        case AUDIO_PLAYBACK_SKIP_NONE:
        default:
            return "";
    }
}

static audio_playback_skip_reason_t playback_skip_reason_locked(void)
{
    if (s_track_volume == 0) {
        return AUDIO_PLAYBACK_SKIP_TRACK_VOLUME_ZERO;
    }
    return AUDIO_PLAYBACK_SKIP_NONE;
}

static void record_playback_skip_notice_locked(const char *name, audio_playback_skip_reason_t reason)
{
    s_playback_skip_notice_seq++;
    s_playback_skip_notice_timestamp_ms = (uint64_t)esp_timer_get_time() / 1000ULL;
    s_playback_skip_notice_reason = reason;
    if (name && name[0]) {
        strlcpy(s_playback_skip_notice_name, name, sizeof(s_playback_skip_notice_name));
    } else {
        s_playback_skip_notice_name[0] = '\0';
    }
}

static void clear_playback_skip_notice_locked(void)
{
    s_playback_skip_notice_timestamp_ms = 0;
    s_playback_skip_notice_reason = AUDIO_PLAYBACK_SKIP_NONE;
    s_playback_skip_notice_name[0] = '\0';
}

//-------------------------------------------------------------------------
// Volume
//-------------------------------------------------------------------------

static int compute_volume_db(void)
{
    float master = (float)s_master_volume / 100.0f;
    float track = (float)s_track_volume / 100.0f;
    float effective_pct = master * track * 100.0f;
    if (effective_pct < 0.0f) {
        effective_pct = 0.0f;
    }

    float db = -60.0f + (0.6f * effective_pct);
    if (db < AUDIO_MIN_DB) {
        db = AUDIO_MIN_DB;
    }
    if (db > AUDIO_MAX_DB) {
        db = AUDIO_MAX_DB;
    }
    return (int)db;
}

static void set_stream_volume_db(int db, bool log_change)
{
    if (!s_playback_i2s_stream) {
        return;
    }

    if (db < AUDIO_MIN_DB) {
        db = AUDIO_MIN_DB;
    }
    if (db > AUDIO_MAX_DB) {
        db = AUDIO_MAX_DB;
    }

    if (i2s_alc_volume_set(s_playback_i2s_stream, db) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set I2S volume");
        return;
    }

    s_current_volume_db = db;
    if (log_change) {
        ESP_LOGI(TAG, "Volume set to %d dB (master=%u track=%u)",
                 s_current_volume_db,
                 (unsigned)s_master_volume,
                 (unsigned)s_track_volume);
    }
}

static void apply_volume_to_stream(void)
{
    int target_db = compute_volume_db();
    s_volume_dirty = false;
    set_stream_volume_db(target_db, true);
}

//-------------------------------------------------------------------------
// Muting
//-------------------------------------------------------------------------

static void fade_out_then_mute(void)
{
    if (AUDIO_ENABLE_FADE_OUT && s_playback_i2s_stream) {
        const int target_db = AUDIO_MIN_DB;
        const int steps = 6;
        const int step_delay_ms = 8;
        int current = s_current_volume_db;

        if (current > target_db) {
            int delta = current - target_db;
            for (int i = 1; i <= steps; i++) {
                int next = current - (delta * i) / steps;
                set_stream_volume_db(next, false);
                vTaskDelay(pdMS_TO_TICKS(step_delay_ms));
            }
        } else {
            set_stream_volume_db(target_db, false);
            vTaskDelay(pdMS_TO_TICKS(step_delay_ms));
        }
    }

    mute_outputs();
}

//-------------------------------------------------------------------------
// I2S + Pipeline
//-------------------------------------------------------------------------

static void destroy_active_playback_pipeline(bool keep_i2s)
{
    if (s_pipeline) {
        audio_pipeline_stop(s_pipeline);
        audio_pipeline_wait_for_stop(s_pipeline);
        audio_pipeline_terminate(s_pipeline);
        audio_pipeline_unlink(s_pipeline);
    }

    if (s_pipeline && s_stream_reader) {
        audio_pipeline_unregister(s_pipeline, s_stream_reader);
    }
    if (s_pipeline && s_decoder) {
        audio_pipeline_unregister(s_pipeline, s_decoder);
    }
    if (s_pipeline && s_playback_i2s_stream) {
        audio_pipeline_unregister(s_pipeline, s_playback_i2s_stream);
    }

    if (s_pipeline) {
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
    if (!keep_i2s && s_playback_i2s_stream) {
        audio_element_deinit(s_playback_i2s_stream);
        s_playback_i2s_stream = NULL;
    }

    s_pipeline = NULL;
    s_stream_reader = NULL;
    s_decoder = NULL;
    s_evt = NULL;
}

static esp_err_t init_playback_i2s_stream_locked(void)
{
    if (s_playback_i2s_stream) {
        return ESP_OK;
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

    s_playback_i2s_stream = i2s_stream_init(&i2s_cfg);
    if (!s_playback_i2s_stream) {
        return ESP_ERR_NO_MEM;
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
        ESP_LOGW(TAG, "Failed to set I2S pins; playback output may be muted");
    }
#endif

    apply_volume_to_stream();
    return ESP_OK;
}

static void deinit_playback_i2s_stream_locked(void)
{
    if (!s_playback_i2s_stream) {
        return;
    }

    audio_element_deinit(s_playback_i2s_stream);
    s_playback_i2s_stream = NULL;
}

static bool parse_mp3_header(uint32_t header, int *sample_rate, int *channels, int *frame_size)
{
    if (((header >> 21) & 0x7FF) != 0x7FF) {
        return false;
    }

    uint8_t version_id = (uint8_t)((header >> 19) & 0x3);
    uint8_t layer_id = (uint8_t)((header >> 17) & 0x3);
    uint8_t bitrate_idx = (uint8_t)((header >> 12) & 0xF);
    uint8_t sample_idx = (uint8_t)((header >> 10) & 0x3);
    uint8_t padding = (uint8_t)((header >> 9) & 0x1);
    uint8_t channel_mode = (uint8_t)((header >> 6) & 0x3);

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
    int coeff = (version_id == 3) ? 144 : 72; // MPEG1 Layer III vs MPEG2/2.5 Layer III
    int size = (coeff * bitrate) / sr + padding;
    if (size <= 4) {
        return false;
    }

    *sample_rate = sr;
    *channels = (channel_mode == 3) ? 1 : 2;
    *frame_size = size;
    return true;
}

static bool prefetch_mp3_format(const char *path, int *sample_rate, int *bits, int *channels)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "Prefetch open failed: %s", path);
        return false;
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
                start_offset += 10; // Optional ID3v2 footer
            }
        }
    }

    if (fseek(f, (long)start_offset, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }

    uint8_t window[4] = {0};
    if (fread(window, 1, sizeof(window), f) != sizeof(window)) {
        fclose(f);
        return false;
    }

    size_t scanned = sizeof(window);
    while (scanned <= AUDIO_MP3_PREFETCH_SCAN_LIMIT) {
        uint32_t header = ((uint32_t)window[0] << 24) |
                          ((uint32_t)window[1] << 16) |
                          ((uint32_t)window[2] << 8) |
                          (uint32_t)window[3];

        int sr = 0;
        int ch = 0;
        int frame_size = 0;
        if (parse_mp3_header(header, &sr, &ch, &frame_size)) {
            *sample_rate = sr;
            *channels = ch;
            *bits = AUDIO_MP3_PCM_BITS;
            fclose(f);
            return true;
        }

        int next = fgetc(f);
        if (next == EOF) {
            break;
        }
        window[0] = window[1];
        window[1] = window[2];
        window[2] = window[3];
        window[3] = (uint8_t)next;
        scanned++;
    }

    fclose(f);
    return false;
}

//-------------------------------------------------------------------------
// File Loading
//-------------------------------------------------------------------------

static esp_err_t load_meta_or_stat_locked(const char *name)
{
    struct stat st = {0};
    if (stat(s_current_file_path, &st) != 0 || st.st_size <= 0) {
        ESP_LOGE(TAG, "Audio file not found or invalid: %s", s_current_file_path);
        return ESP_ERR_NOT_FOUND;
    }

    file_properties_t props;
    esp_err_t hdr_err = files_read_meta(name, &props);

    s_current_offset_bytes = 0;
    s_current_file_size = (size_t)st.st_size;

    if (hdr_err == ESP_OK) {
        s_track_volume = clamp_track_volume(props.volume);
        s_volume_dirty = true;
    } else {
        // No metadata sidecar: fall back to neutral per-track gain.
        s_track_volume = 100;
        s_volume_dirty = true;
        ESP_LOGW(TAG, "Playing file without metadata sidecar: %s (size=%zu)",
                 s_current_file_path,
                 s_current_file_size);
    }

    return ESP_OK;
}

#if CONFIG_TEST_AUDIO_MOCK_BACKEND
// See notes/playback_memory_mock.md for the measured hardware footprint and
// why the timed mock playback path reserves heap in this shape.
#define MOCK_PLAYBACK_HEAP_TARGET_BYTES              (82 * 1024)
#define MOCK_PLAYBACK_HEAP_MAX_BYTES                 (192 * 1024)
#define MOCK_PLAYBACK_HEAP_MIN_FREE_BYTES            (48 * 1024)
#define MOCK_PLAYBACK_TARGET_FREE_BYTES              (76 * 1024)
#define MOCK_PLAYBACK_TARGET_LARGEST_FREE_BYTES      (96 * 1024)
#define MOCK_PLAYBACK_HEAP_BLOCK_SLOTS               32

typedef struct {
    void *blocks[MOCK_PLAYBACK_HEAP_BLOCK_SLOTS];
    size_t sizes[MOCK_PLAYBACK_HEAP_BLOCK_SLOTS];
    size_t block_count;
    size_t total_size;
} mock_playback_heap_profile_t;

static size_t mock_playback_heap_target_bytes(void)
{
    size_t reserve_target = MOCK_PLAYBACK_HEAP_TARGET_BYTES;
    size_t free8 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if (free8 > MOCK_PLAYBACK_TARGET_FREE_BYTES) {
        size_t free_target = free8 - MOCK_PLAYBACK_TARGET_FREE_BYTES;
        if (free_target > reserve_target) {
            reserve_target = free_target;
        }
    }

    size_t largest_free = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (largest_free > MOCK_PLAYBACK_TARGET_LARGEST_FREE_BYTES) {
        size_t collapse_target = largest_free - MOCK_PLAYBACK_TARGET_LARGEST_FREE_BYTES;
        if (collapse_target > reserve_target) {
            reserve_target = collapse_target;
        }
    }

    if (free8 <= MOCK_PLAYBACK_HEAP_MIN_FREE_BYTES) {
        return 0;
    }

    size_t max_reserve = free8 - MOCK_PLAYBACK_HEAP_MIN_FREE_BYTES;
    if (reserve_target > max_reserve) {
        reserve_target = max_reserve;
    }
    if (reserve_target > MOCK_PLAYBACK_HEAP_MAX_BYTES) {
        reserve_target = MOCK_PLAYBACK_HEAP_MAX_BYTES;
    }
    return reserve_target;
}

static void mock_playback_heap_profile_release(mock_playback_heap_profile_t *profile)
{
    if (!profile) {
        return;
    }

    while (profile->block_count > 0) {
        profile->block_count--;
        free(profile->blocks[profile->block_count]);
        profile->blocks[profile->block_count] = NULL;
        profile->sizes[profile->block_count] = 0;
    }
    profile->total_size = 0;
}

static size_t mock_playback_heap_profile_acquire(mock_playback_heap_profile_t *profile)
{
    static const size_t block_pattern[] = {
        4 * 1024,
        8 * 1024,
        4 * 1024,
        5 * 1024,
        3584,
        3600,
        8 * 1024,
        4 * 1024,
    };

    if (!profile) {
        return 0;
    }

    memset(profile, 0, sizeof(*profile));

    size_t target = mock_playback_heap_target_bytes();
    if (target == 0) {
        return 0;
    }

    size_t remaining = target;
    size_t largest_before = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    size_t largest_guard = 40 * 1024;
    if (largest_before > MOCK_PLAYBACK_TARGET_LARGEST_FREE_BYTES) {
        size_t collapse_target = largest_before - MOCK_PLAYBACK_TARGET_LARGEST_FREE_BYTES;
        if (collapse_target > largest_guard) {
            largest_guard = collapse_target;
        }
    }
    if (largest_guard > remaining) {
        largest_guard = remaining;
    }

    if (largest_guard > 0) {
        void *guard = heap_caps_malloc(largest_guard, MALLOC_CAP_8BIT);
        if (guard) {
            memset(guard, 0xA5, largest_guard);
            profile->blocks[profile->block_count] = guard;
            profile->sizes[profile->block_count] = largest_guard;
            profile->block_count++;
            profile->total_size += largest_guard;
            remaining -= largest_guard;
        } else {
            ESP_LOGW(TAG,
                     "Mock playback largest-block reserve failed (size=%u)",
                     (unsigned)largest_guard);
        }
    }

    size_t pattern_len = sizeof(block_pattern) / sizeof(block_pattern[0]);
    for (size_t i = 0; remaining > 0 && profile->block_count < MOCK_PLAYBACK_HEAP_BLOCK_SLOTS; i++) {
        size_t pattern_size = block_pattern[i % pattern_len];
        size_t alloc_size = remaining < pattern_size ? remaining : pattern_size;
        void *block = heap_caps_malloc(alloc_size, MALLOC_CAP_8BIT);
        if (!block) {
            ESP_LOGW(TAG,
                     "Mock playback heap reserve failed at block %u (size=%u, reserved=%u/%u)",
                     (unsigned)profile->block_count,
                     (unsigned)alloc_size,
                     (unsigned)profile->total_size,
                     (unsigned)target);
            break;
        }

        memset(block, 0xA5, alloc_size);
        profile->blocks[profile->block_count] = block;
        profile->sizes[profile->block_count] = alloc_size;
        profile->block_count++;
        profile->total_size += alloc_size;
        remaining -= alloc_size;
    }

    if (remaining > 0) {
        ESP_LOGW(TAG,
                 "Mock playback heap profile incomplete: reserved=%u target=%u blocks=%u",
                 (unsigned)profile->total_size,
                 (unsigned)target,
                 (unsigned)profile->block_count);
    } else {
        ESP_LOGI(TAG,
                 "Mock playback heap profile reserved %u bytes across %u blocks",
                 (unsigned)profile->total_size,
                 (unsigned)profile->block_count);
    }

    return profile->total_size;
}

// Parse playback duration from basename pattern "...<digits>ms...".
// Example: "test6165ms.mp3" -> 6165.
static uint32_t mock_playback_duration_from_path_ms(const char *path)
{
    if (!path || !path[0]) {
        return 0;
    }

    const char *name = strrchr(path, '/');
    name = name ? (name + 1) : path;

    const char *dot = strrchr(name, '.');
    size_t name_len = (dot && dot > name) ? (size_t)(dot - name) : strlen(name);
    uint32_t parsed_ms = 0;

    for (size_t i = 1; (i + 1) < name_len; i++) {
        if (name[i] != 'm' || name[i + 1] != 's') {
            continue;
        }

        size_t start = i;
        while (start > 0 && name[start - 1] >= '0' && name[start - 1] <= '9') {
            start--;
        }
        if (start == i) {
            continue;
        }

        uint32_t value = 0;
        for (size_t j = start; j < i; j++) {
            value = value * 10U + (uint32_t)(name[j] - '0');
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

//-------------------------------------------------------------------------
// Tasks
//-------------------------------------------------------------------------

static void play_task(void *arg)
{
    (void)arg;

#if CONFIG_TEST_AUDIO_MOCK_BACKEND
    uint32_t mock_duration_ms = mock_playback_duration_from_path_ms(s_current_file_path);
    mock_playback_heap_profile_t mock_heap = {0};
    TickType_t start_tick = xTaskGetTickCount();
    TickType_t duration_ticks = 0;
    if (mock_duration_ms > 0) {
        duration_ticks = pdMS_TO_TICKS(mock_duration_ms);
        if (duration_ticks == 0) {
            duration_ticks = 1;
        }
        mock_playback_heap_profile_acquire(&mock_heap);
    } else {
        ESP_LOGW(TAG,
                 "Mock playback duration missing in filename: %s (finishing immediately)",
                 s_current_file_path);
    }

    s_play_stop_requested = false;
    s_playing = true;

    s_current_volume_db = compute_volume_db();
    s_volume_dirty = false;
    if (playback_should_unmute()) {
        unmute_outputs();
    } else {
        mute_outputs();
    }

    while (!s_play_stop_requested) {
        if (duration_ticks == 0) {
            break;
        }
        TickType_t elapsed_ticks = xTaskGetTickCount() - start_tick;
        if (elapsed_ticks >= duration_ticks) {
            ESP_LOGI(TAG, "Playback finished (mock)");
            break;
        }

        if (s_volume_dirty) {
            s_current_volume_db = compute_volume_db();
            s_volume_dirty = false;
            if (playback_should_unmute()) {
                unmute_outputs();
            } else {
                mute_outputs();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (s_play_stop_requested) {
        mute_outputs();
    } else {
        fade_out_then_mute();
    }
    if (mock_heap.total_size > 0) {
        mock_playback_heap_profile_release(&mock_heap);
    }

    s_play_stop_requested = false;
    s_playing = false;
    s_current_file_name[0] = '\0';
    s_play_task = NULL;
    vTaskDelete(NULL);
#else

    esp_err_t err = ESP_OK;

    if (!s_playback_i2s_stream) {
        err = ESP_ERR_INVALID_STATE;
        goto exit;
    }

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
    mp3_cfg.out_rb_size = AUDIO_MP3_DECODER_OUT_RB_SIZE;
    s_decoder = mp3_decoder_init(&mp3_cfg);
    if (!s_decoder) {
        err = ESP_ERR_NO_MEM;
        goto exit;
    }

    audio_pipeline_register(s_pipeline, s_stream_reader, "file");
    audio_pipeline_register(s_pipeline, s_decoder, "mp3");
    audio_pipeline_register(s_pipeline, s_playback_i2s_stream, "i2s");

    const char *link_tag[3] = {"file", "mp3", "i2s"};
    audio_pipeline_link(s_pipeline, link_tag, 3);

    audio_element_set_uri(s_stream_reader, s_current_file_path);
    audio_element_info_t el_info = {0};
    audio_element_getinfo(s_stream_reader, &el_info);
    el_info.byte_pos = (int64_t)s_current_offset_bytes;
    el_info.total_bytes = s_current_file_size;
    audio_element_setinfo(s_stream_reader, &el_info);

    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    s_evt = audio_event_iface_init(&evt_cfg);
    if (!s_evt) {
        err = ESP_ERR_NO_MEM;
        goto exit;
    }
    audio_pipeline_set_listener(s_pipeline, s_evt);

    int active_sample_rate = 0;
    int active_bits = 0;
    int active_channels = 0;
    bool decoder_info_ready = false;

    if (prefetch_mp3_format(s_current_file_path, &active_sample_rate, &active_bits, &active_channels)) {
        audio_element_info_t prefetched = {0};
        prefetched.sample_rates = active_sample_rate;
        prefetched.bits = active_bits;
        prefetched.channels = active_channels;
        audio_element_setinfo(s_playback_i2s_stream, &prefetched);

        esp_err_t clk_err = i2s_stream_set_clk(s_playback_i2s_stream,
                                               active_sample_rate,
                                               active_bits,
                                               active_channels);
        if (clk_err == ESP_OK) {
            decoder_info_ready = true;
            ESP_LOGI(TAG, "Prefetched MP3 format: %d Hz, %d bit, %d ch",
                     active_sample_rate,
                     active_bits,
                     active_channels);
        } else {
            ESP_LOGW(TAG, "Prefetch I2S clock setup failed: %s", esp_err_to_name(clk_err));
            active_sample_rate = 0;
            active_bits = 0;
            active_channels = 0;
        }
    } else {
        ESP_LOGW(TAG, "Could not prefetch MP3 format for %s; waiting for decoder info",
                 s_current_file_path);
    }

    err = audio_pipeline_run(s_pipeline);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio_pipeline_run failed: %s", esp_err_to_name(err));
        goto exit;
    }

    s_play_stop_requested = false;
    s_playing = true;

    bool outputs_unmuted = false;
    apply_volume_to_stream();
    if (decoder_info_ready && playback_should_unmute()) {
        unmute_outputs();
        outputs_unmuted = true;
    }

    while (!s_play_stop_requested) {
        audio_event_iface_msg_t msg;
        esp_err_t res = audio_event_iface_listen(s_evt, &msg, pdMS_TO_TICKS(AUDIO_PLAY_EVENT_WAIT_MS));

        if (res == ESP_ERR_TIMEOUT) {
            if (s_volume_dirty) {
                apply_volume_to_stream();

                // Avoid startup chop: only unmute after decoder format/clock is configured.
                if (decoder_info_ready) {
                    bool should_unmute = playback_should_unmute();
                    if (should_unmute && !outputs_unmuted) {
                        unmute_outputs();
                        outputs_unmuted = true;
                    } else if (!should_unmute && outputs_unmuted) {
                        mute_outputs();
                        outputs_unmuted = false;
                    }
                }
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

            audio_element_setinfo(s_playback_i2s_stream, &music_info);
            bool need_clk_update = ((int)music_info.sample_rates != active_sample_rate ||
                                    music_info.bits != active_bits ||
                                    music_info.channels != active_channels);
            if (need_clk_update && outputs_unmuted) {
                mute_outputs();
                outputs_unmuted = false;
            }
            if (need_clk_update) {
                esp_err_t clk_err = i2s_stream_set_clk(s_playback_i2s_stream,
                                                       music_info.sample_rates,
                                                       music_info.bits,
                                                       music_info.channels);
                if (clk_err != ESP_OK) {
                    ESP_LOGW(TAG, "i2s_stream_set_clk failed: %s", esp_err_to_name(clk_err));
                } else {
                    active_sample_rate = (int)music_info.sample_rates;
                    active_bits = music_info.bits;
                    active_channels = music_info.channels;
                    decoder_info_ready = true;
                }
            } else {
                decoder_info_ready = true;
            }

            apply_volume_to_stream();
            if (decoder_info_ready && !outputs_unmuted && playback_should_unmute()) {
                unmute_outputs();
                outputs_unmuted = true;
            } else if (decoder_info_ready && outputs_unmuted && !playback_should_unmute()) {
                mute_outputs();
                outputs_unmuted = false;
            }
        }

        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT &&
            msg.cmd == AEL_MSG_CMD_REPORT_STATUS) {
            int state = (int)msg.data;
            if (msg.source == (void *)s_playback_i2s_stream &&
                (state == AEL_STATUS_STATE_FINISHED || state == AEL_STATUS_STATE_STOPPED)) {
                ESP_LOGI(TAG, "Playback finished");
                break;
            }
        }
    }

exit:
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Stopping playback due to error: %s", esp_err_to_name(err));
    }

    if (s_play_stop_requested) {
        // Fast restart path: stop immediately without tail fade.
        mute_outputs();
    } else {
        fade_out_then_mute();
    }
    destroy_active_playback_pipeline(true);

    s_play_stop_requested = false;
    s_playing = false;
    s_current_file_name[0] = '\0';
    s_play_task = NULL;
    vTaskDelete(NULL);
#endif
}

static void stop_playback_task_locked(void)
{
    if (!s_play_task) {
        return;
    }

    s_play_stop_requested = true;
    if (s_stream_reader) {
        audio_element_abort_output_ringbuf(s_stream_reader);
    }
    if (s_decoder) {
        audio_element_abort_output_ringbuf(s_decoder);
    }
    if (s_playback_i2s_stream) {
        audio_element_abort_input_ringbuf(s_playback_i2s_stream);
    }
    if (s_pipeline) {
        audio_pipeline_stop(s_pipeline);
    }
    for (int i = 0; i < 100 && s_play_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (s_play_task) {
        ESP_LOGW(TAG, "Playback task did not stop in time");
    }
}

//-------------------------------------------------------------------------
// Public
//-------------------------------------------------------------------------

static esp_err_t audio_start_file_internal(const char *name,
                                           const uint8_t *override_volume_pct,
                                           audio_playback_start_result_t *out_result,
                                           audio_playback_skip_reason_t *out_skip_reason)
{
    if (out_result) {
        *out_result = AUDIO_PLAYBACK_START_RESULT_STARTED;
    }
    if (out_skip_reason) {
        *out_skip_reason = AUDIO_PLAYBACK_SKIP_NONE;
    }
    if (!name || !name[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strchr(name, '/') || strstr(name, "..")) {
        ESP_LOGE(TAG, "Invalid filename: %s", name);
        return ESP_ERR_INVALID_ARG;
    }

    audio_lock();

    if (!s_initialized) {
        audio_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = switch_mode_locked(AUDIO_MODE_PLAYBACK);
    if (err != ESP_OK) {
        audio_unlock();
        return err;
    }

    stop_playback_task_locked();

    int full_len = snprintf(s_current_file_path,
                            sizeof(s_current_file_path),
                            "%s/%s",
                            s_base_path,
                            name);
    if (full_len < 0 || (size_t)full_len >= sizeof(s_current_file_path)) {
        ESP_LOGE(TAG,
                 "Audio path too long (len=%d, cap=%u): base=%s file=%s",
                 full_len,
                 (unsigned)sizeof(s_current_file_path),
                 s_base_path,
                 name);
        audio_unlock();
        return ESP_ERR_INVALID_SIZE;
    }

    err = load_meta_or_stat_locked(name);
    if (err != ESP_OK) {
        audio_unlock();
        return err;
    }

    if (override_volume_pct) {
        s_track_volume = clamp_percent(*override_volume_pct);
        s_volume_dirty = true;
    }

    audio_playback_skip_reason_t skip_reason = playback_skip_reason_locked();
    if (skip_reason != AUDIO_PLAYBACK_SKIP_NONE) {
        record_playback_skip_notice_locked(name, skip_reason);
        ESP_LOGW(TAG, "Skipping playback: %s (%s)", name, audio_playback_skip_reason_text(skip_reason));
        if (out_result) {
            *out_result = AUDIO_PLAYBACK_START_RESULT_SKIPPED;
        }
        if (out_skip_reason) {
            *out_skip_reason = skip_reason;
        }
        audio_unlock();
        return ESP_OK;
    }

    strlcpy(s_current_file_name, name, sizeof(s_current_file_name));
    BaseType_t res = xTaskCreate(play_task,
                                 "mp3-play",
                                 AUDIO_PLAY_TASK_STACK,
                                 NULL,
                                 AUDIO_PLAY_TASK_PRIO,
                                 &s_play_task);
    if (res != pdPASS) {
        s_play_task = NULL;
        s_current_file_name[0] = '\0';
        audio_unlock();
        return ESP_ERR_NO_MEM;
    }

    clear_playback_skip_notice_locked();
    ESP_LOGI(TAG, "Starting playback: %s", name);
    audio_unlock();
    return ESP_OK;
}

esp_err_t audio_start_file(const char *name,
                           audio_playback_start_result_t *out_result,
                           audio_playback_skip_reason_t *out_skip_reason)
{
    return audio_start_file_internal(name, NULL, out_result, out_skip_reason);
}

esp_err_t audio_start_file_with_volume(const char *name,
                                       uint8_t volume_pct,
                                       audio_playback_start_result_t *out_result,
                                       audio_playback_skip_reason_t *out_skip_reason)
{
    uint8_t clamped_volume_pct = clamp_percent(volume_pct);
    return audio_start_file_internal(name, &clamped_volume_pct, out_result, out_skip_reason);
}

bool audio_is_playing(void)
{
    return s_playing;
}

void audio_get_playback_status(bool *out_active,
                               char *out_name,
                               size_t out_name_size,
                               audio_playback_skip_reason_t *out_skip_reason,
                               bool consume_skip_notice)
{
    audio_lock();

    bool active = s_playing;
    audio_playback_skip_reason_t skip_reason = AUDIO_PLAYBACK_SKIP_NONE;
    char name[FILE_ENTRY_NAME_MAX] = {0};

    if (active) {
        strlcpy(name, s_current_file_name, sizeof(name));
    } else if (s_playback_skip_notice_reason != AUDIO_PLAYBACK_SKIP_NONE) {
        skip_reason = s_playback_skip_notice_reason;
        strlcpy(name, s_playback_skip_notice_name, sizeof(name));
        if (consume_skip_notice) {
            clear_playback_skip_notice_locked();
        }
    }

    if (out_active) {
        *out_active = active;
    }
    if (out_name && out_name_size > 0) {
        strlcpy(out_name, name, out_name_size);
    }
    if (out_skip_reason) {
        *out_skip_reason = skip_reason;
    }

    audio_unlock();
}

void audio_get_last_playback_skip_notice(uint32_t *out_seq,
                                         uint64_t *out_timestamp_ms,
                                         audio_playback_skip_reason_t *out_reason,
                                         char *out_name,
                                         size_t out_name_size)
{
    audio_lock();
    if (out_seq) {
        *out_seq = s_playback_skip_notice_seq;
    }
    if (out_timestamp_ms) {
        *out_timestamp_ms = s_playback_skip_notice_timestamp_ms;
    }
    if (out_reason) {
        *out_reason = s_playback_skip_notice_reason;
    }
    if (out_name && out_name_size > 0) {
        strlcpy(out_name, s_playback_skip_notice_name, out_name_size);
    }
    audio_unlock();
}

void audio_set_master_volume_level(uint8_t level)
{
    s_master_volume = clamp_percent(level);
    s_volume_dirty = true;
}

void audio_set_track_volume_level(uint8_t level)
{
    s_track_volume = clamp_track_volume(level);
    s_volume_dirty = true;
}
