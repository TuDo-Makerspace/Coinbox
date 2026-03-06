#include "board.h"

#include "audio_mem.h"
#include "driver/gpio.h"
#include "es7148.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

///////////////////////////////////////////////////////////////////////////////////////////////////
// Constants
///////////////////////////////////////////////////////////////////////////////////////////////////

static const char *TAG = "AUDIO_BOARD";

///////////////////////////////////////////////////////////////////////////////////////////////////
// Static Vars
///////////////////////////////////////////////////////////////////////////////////////////////////

static audio_board_handle_t s_board_handle;
static bool s_outputs_initialized;

#if CONFIG_TEST_GPIO_INJECTION
static volatile int s_test_amp_mute_level = -1;
static volatile int s_test_dac_mute_level = -1;
#endif

///////////////////////////////////////////////////////////////////////////////////////////////////
// Board Lifecycle
///////////////////////////////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------
// Board Init
//-------------------------------------------------------------------------

audio_board_handle_t audio_board_init(void)
{
    if (s_board_handle) {
        return s_board_handle;
    }

    s_board_handle = (audio_board_handle_t)audio_calloc(1, sizeof(struct audio_board_handle));
    AUDIO_MEM_CHECK(TAG, s_board_handle, return NULL);

    s_board_handle->audio_hal = audio_board_codec_init();
    return s_board_handle;
}

//-------------------------------------------------------------------------
// Codec Init
//-------------------------------------------------------------------------

audio_hal_handle_t audio_board_codec_init(void)
{
#if FUNC_AUDIO_CODEC_EN
    audio_hal_codec_config_t cfg = AUDIO_CODEC_DEFAULT_CONFIG();
    audio_hal_handle_t hal = audio_hal_init(&cfg, &AUDIO_CODEC_ES7148_DEFAULT_HANDLE);
    AUDIO_NULL_CHECK(TAG, hal, return NULL);
    return hal;
#else
    return NULL;
#endif
}

//-------------------------------------------------------------------------
// Optional Peripherals
//-------------------------------------------------------------------------

display_service_handle_t audio_board_led_init(void)
{
    ESP_LOGW(TAG, "LED service not supported on this board");
    return NULL;
}

esp_err_t audio_board_key_init(esp_periph_set_handle_t set)
{
    (void)set;
    return ESP_OK;
}

esp_err_t audio_board_sdcard_init(esp_periph_set_handle_t set, periph_sdcard_mode_t mode)
{
    (void)set;
    (void)mode;
    return ESP_ERR_NOT_SUPPORTED;
}

audio_board_handle_t audio_board_get_handle(void)
{
    return s_board_handle;
}

//-------------------------------------------------------------------------
// Deinit
//-------------------------------------------------------------------------

esp_err_t audio_board_deinit(audio_board_handle_t audio_board)
{
    esp_err_t ret = ESP_OK;

    if (!audio_board) {
        return ESP_OK;
    }

    if (audio_board->audio_hal) {
        ret = audio_hal_deinit(audio_board->audio_hal);
        audio_board->audio_hal = NULL;
    }

    audio_free(audio_board);
    if (audio_board == s_board_handle) {
        s_board_handle = NULL;
    }
    return ret;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Output Control (DAC + AMP Mute)
///////////////////////////////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------
// Outputs Init
//-------------------------------------------------------------------------

esp_err_t audio_board_outputs_init(void)
{
    if (s_outputs_initialized) {
        return ESP_OK;
    }

    uint64_t output_mask = 0;
    if (AMP_MUTE_GPIO >= 0) {
        output_mask |= (1ULL << AMP_MUTE_GPIO);
    }
    if (DAC_MUTE_GPIO >= 0) {
        output_mask |= (1ULL << DAC_MUTE_GPIO);
    }

    if (output_mask != 0) {
        gpio_config_t io_conf = {
            .intr_type = GPIO_INTR_DISABLE,
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = output_mask,
            .pull_down_en = 0,
            .pull_up_en = 0,
        };

        esp_err_t err = gpio_config(&io_conf);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to configure output pins: %s", esp_err_to_name(err));
            return err;
        }
    }

    audio_board_outputs_mute();
    s_outputs_initialized = true;
    return ESP_OK;
}

//-------------------------------------------------------------------------
// Mute / Unmute
//-------------------------------------------------------------------------

static esp_err_t audio_board_write_amp_mute_level(int level)
{
    if (AMP_MUTE_GPIO < 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    esp_err_t err = gpio_set_level(AMP_MUTE_GPIO, level ? 1 : 0);
#if CONFIG_TEST_GPIO_INJECTION
    if (err == ESP_OK) {
        s_test_amp_mute_level = level ? 1 : 0;
    }
#endif
    return err;
}

static esp_err_t audio_board_write_dac_mute_level(int level)
{
    if (DAC_MUTE_GPIO < 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    esp_err_t err = gpio_set_level(DAC_MUTE_GPIO, level ? 1 : 0);
#if CONFIG_TEST_GPIO_INJECTION
    if (err == ESP_OK) {
        s_test_dac_mute_level = level ? 1 : 0;
    }
#endif
    return err;
}

void audio_board_outputs_mute(void)
{
    if (AMP_MUTE_GPIO >= 0) {
        if (audio_board_write_amp_mute_level(AMP_MUTE_LEVEL) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to set AMP mute GPIO");
        }
    }
    if (DAC_MUTE_GPIO >= 0) {
        if (audio_board_write_dac_mute_level(DAC_MUTE_LEVEL) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to set DAC mute GPIO");
        }
    }
}

void audio_board_outputs_unmute(void)
{
    if (AMP_MUTE_GPIO >= 0) {
        if (audio_board_write_amp_mute_level(!AMP_MUTE_LEVEL) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to set AMP mute GPIO");
        }
    }
    if (DAC_MUTE_GPIO >= 0) {
        if (audio_board_write_dac_mute_level(!DAC_MUTE_LEVEL) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to set DAC mute GPIO");
        }
    }
}

#if CONFIG_TEST_GPIO_INJECTION
int audio_board_test_get_amp_mute_level(void)
{
    return s_test_amp_mute_level;
}

int audio_board_test_get_dac_mute_level(void)
{
    return s_test_dac_mute_level;
}

esp_err_t audio_board_test_set_amp_mute_level(int level)
{
    if (level != 0 && level != 1) {
        return ESP_ERR_INVALID_ARG;
    }
    return audio_board_write_amp_mute_level(level);
}

esp_err_t audio_board_test_set_dac_mute_level(int level)
{
    if (level != 0 && level != 1) {
        return ESP_ERR_INVALID_ARG;
    }
    return audio_board_write_dac_mute_level(level);
}
#endif
