#include <string.h>

#include "audio_error.h"
#include "board.h"
#include "esp_log.h"

///////////////////////////////////////////////////////////////////////////////////////////////////
// Constants
///////////////////////////////////////////////////////////////////////////////////////////////////

#define TAG "COINBOX_BOARD"

#define COINBOX_I2S_MCLK      (-1)
#define COINBOX_I2S_BCK       (GPIO_NUM_19)
#define COINBOX_I2S_WS        (GPIO_NUM_18)
#define COINBOX_I2S_DATA_OUT  (GPIO_NUM_17)
#define COINBOX_I2S_DATA_IN   (-1)

///////////////////////////////////////////////////////////////////////////////////////////////////
// Pin Mapping
///////////////////////////////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------
// I2C / I2S / SPI
//-------------------------------------------------------------------------

esp_err_t get_i2c_pins(i2c_port_t port, i2c_config_t *i2c_config)
{
    AUDIO_NULL_CHECK(TAG, i2c_config, return ESP_FAIL);
    (void)port;

    i2c_config->sda_io_num = -1;
    i2c_config->scl_io_num = -1;
    return ESP_FAIL;
}

esp_err_t get_i2s_pins(int port, board_i2s_pin_t *i2s_config)
{
    AUDIO_NULL_CHECK(TAG, i2s_config, return ESP_FAIL);

    if (port == 0 || port == 1) {
        i2s_config->mck_io_num = COINBOX_I2S_MCLK;
        i2s_config->bck_io_num = COINBOX_I2S_BCK;
        i2s_config->ws_io_num = COINBOX_I2S_WS;
        i2s_config->data_out_num = COINBOX_I2S_DATA_OUT;
        i2s_config->data_in_num = COINBOX_I2S_DATA_IN;
        return ESP_OK;
    }

    memset(i2s_config, -1, sizeof(*i2s_config));
    ESP_LOGE(TAG, "i2s port %d is not supported", port);
    return ESP_FAIL;
}

esp_err_t get_spi_pins(spi_bus_config_t *spi_config,
                       spi_device_interface_config_t *spi_device_interface_config)
{
    AUDIO_NULL_CHECK(TAG, spi_config, return ESP_FAIL);
    AUDIO_NULL_CHECK(TAG, spi_device_interface_config, return ESP_FAIL);

    spi_config->mosi_io_num = -1;
    spi_config->miso_io_num = -1;
    spi_config->sclk_io_num = -1;
    spi_config->quadwp_io_num = -1;
    spi_config->quadhd_io_num = -1;

    spi_device_interface_config->spics_io_num = -1;
    return ESP_OK;
}

//-------------------------------------------------------------------------
// SDCard
//-------------------------------------------------------------------------

int8_t get_sdcard_intr_gpio(void)
{
    return SDCARD_INTR_GPIO;
}

int8_t get_sdcard_open_file_num_max(void)
{
    return SDCARD_OPEN_FILE_NUM_MAX;
}

int8_t get_sdcard_power_ctrl_gpio(void)
{
    return -1;
}

//-------------------------------------------------------------------------
// Audio Detect / PA
//-------------------------------------------------------------------------

int8_t get_auxin_detect_gpio(void)
{
    return AUXIN_DETECT_GPIO;
}

int8_t get_headphone_detect_gpio(void)
{
    return HEADPHONE_DETECT;
}

int8_t get_pa_enable_gpio(void)
{
    return PA_ENABLE_GPIO;
}

int8_t get_adc_detect_gpio(void)
{
    return -1;
}

int8_t get_es7243_mclk_gpio(void)
{
    return -1;
}

//-------------------------------------------------------------------------
// Buttons
//-------------------------------------------------------------------------

int8_t get_input_rec_id(void)
{
    return BUTTON_REC_ID;
}

int8_t get_input_mode_id(void)
{
    return BUTTON_MODE_ID;
}

int8_t get_input_color_id(void)
{
    return BUTTON_COLOR_ID;
}

int8_t get_input_set_id(void)
{
    return BUTTON_SET_ID;
}

int8_t get_input_play_id(void)
{
    return BUTTON_PLAY_ID;
}

int8_t get_input_volup_id(void)
{
    return BUTTON_VOLUP_ID;
}

int8_t get_input_voldown_id(void)
{
    return BUTTON_VOLDOWN_ID;
}

//-------------------------------------------------------------------------
// Reset / LEDs
//-------------------------------------------------------------------------

int8_t get_reset_codec_gpio(void)
{
    return -1;
}

int8_t get_reset_board_gpio(void)
{
    return -1;
}

int8_t get_green_led_gpio(void)
{
    return GREEN_LED_GPIO;
}

int8_t get_blue_led_gpio(void)
{
    return BLUE_LED_GPIO;
}
