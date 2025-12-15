#include "audio_test.h"

#include <math.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s.h"
#include "esp_log.h"
#include "esp_intr_alloc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TAG "audio_test"

// GPIO layout matches the PCB used for the previous I2S test
#define GPIO_I2S_DATA GPIO_NUM_17
#define GPIO_I2S_WS   GPIO_NUM_18
#define GPIO_I2S_BCK  GPIO_NUM_19
#define GPIO_MUTE_DAC GPIO_NUM_21  // active low
#define GPIO_MUTE_AMP GPIO_NUM_22  // active high

#define TONE_TABLE_LEN 256

static TaskHandle_t s_tone_task;
static volatile bool s_stop_requested;
static volatile float s_target_freq_hz = AUDIO_TEST_DEFAULT_HZ;
static bool s_pins_configured;

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

static void configure_mute_pins_once(void)
{
    if (s_pins_configured) {
        return;
    }
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << GPIO_MUTE_DAC) | (1ULL << GPIO_MUTE_AMP),
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = 0,
        .pull_up_en = 0,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(GPIO_MUTE_AMP, 1);
    gpio_set_level(GPIO_MUTE_DAC, 0);
    s_pins_configured = true;
}

static void unmute_outputs(void)
{
    gpio_set_level(GPIO_MUTE_AMP, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(GPIO_MUTE_DAC, 1);
}

static void mute_outputs(void)
{
    gpio_set_level(GPIO_MUTE_AMP, 1);
    gpio_set_level(GPIO_MUTE_DAC, 0);
}

static void fill_tone_block(int16_t *buffer, float *phase, float freq_hz)
{
    const float phase_step = (2.0f * (float)M_PI * freq_hz) / (float)AUDIO_TEST_SAMPLE_RATE;

    for (int i = 0; i < TONE_TABLE_LEN; i++) {
        *phase += phase_step;
        if (*phase > (2.0f * (float)M_PI)) {
            *phase -= (2.0f * (float)M_PI);
        }

        int16_t sample = (int16_t)(AUDIO_TEST_SINE_AMPLITUDE * sinf(*phase));
        buffer[2 * i] = sample;
        buffer[2 * i + 1] = sample;
    }
}

static void tone_task(void *arg)
{
    (void)arg;
    float phase = 0.0f;
    int16_t block[TONE_TABLE_LEN * 2];

    while (!s_stop_requested) {
        float freq = s_target_freq_hz;
        fill_tone_block(block, &phase, freq);

        size_t bytes_written = 0;
        esp_err_t err = i2s_write(I2S_NUM_0, block, sizeof(block), &bytes_written, pdMS_TO_TICKS(100));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s_write failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    mute_outputs();
    i2s_zero_dma_buffer(I2S_NUM_0);
    i2s_driver_uninstall(I2S_NUM_0);

    s_tone_task = NULL;
    s_stop_requested = false;
    vTaskDelete(NULL);
}

esp_err_t audio_test_start(float freq_hz)
{
    freq_hz = clamp_freq(freq_hz);
    s_target_freq_hz = freq_hz;

    if (s_stop_requested && s_tone_task) {
        for (int i = 0; i < 50 && s_tone_task; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    if (s_tone_task) {
        return ESP_OK;  // already running; frequency updated
    }

    configure_mute_pins_once();

    i2s_config_t i2s_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX,
        .sample_rate = AUDIO_TEST_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_I2S_MSB,
        .dma_buf_count = 8,
        .dma_buf_len = TONE_TABLE_LEN,
        .use_apll = false,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL2,
        .tx_desc_auto_clear = true,
    };

    esp_err_t err = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install I2S driver: %s", esp_err_to_name(err));
        return err;
    }

    i2s_pin_config_t pin_config = {
        .mck_io_num = I2S_PIN_NO_CHANGE,
        .bck_io_num = GPIO_I2S_BCK,
        .ws_io_num = GPIO_I2S_WS,
        .data_out_num = GPIO_I2S_DATA,
        .data_in_num = I2S_PIN_NO_CHANGE,
    };
    err = i2s_set_pin(I2S_NUM_0, &pin_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set I2S pins: %s", esp_err_to_name(err));
        i2s_driver_uninstall(I2S_NUM_0);
        return err;
    }

    err = i2s_set_sample_rates(I2S_NUM_0, AUDIO_TEST_SAMPLE_RATE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set I2S sample rate: %s", esp_err_to_name(err));
        i2s_driver_uninstall(I2S_NUM_0);
        return err;
    }

    s_stop_requested = false;
    if (xTaskCreate(tone_task, "audio-test", 4096, NULL, tskIDLE_PRIORITY + 4, &s_tone_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create audio test task");
        i2s_driver_uninstall(I2S_NUM_0);
        s_tone_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    unmute_outputs();
    ESP_LOGI(TAG, "Started audio test tone at %.1f Hz", (double)s_target_freq_hz);
    return ESP_OK;
}

void audio_test_stop(void)
{
    if (!s_tone_task) {
        return;
    }

    s_stop_requested = true;
    for (int i = 0; i < 50 && s_tone_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_tone_task) {
        ESP_LOGW(TAG, "Audio test task did not stop in time");
    } else {
        ESP_LOGI(TAG, "Audio test stopped");
    }
}

bool audio_test_is_running(void)
{
    return s_tone_task != NULL;
}

float audio_test_current_freq(void)
{
    return s_target_freq_hz;
}
