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

/*
 * Minimal board definition for Coinbox hardware.
 */

#ifndef _AUDIO_BOARD_DEFINITION_H_
#define _AUDIO_BOARD_DEFINITION_H_

///////////////////////////////////////////////////////////////////////////////////////////////////
// Includes
///////////////////////////////////////////////////////////////////////////////////////////////////

#include <stdbool.h>

#include "driver/i2s_types.h"

///////////////////////////////////////////////////////////////////////////////////////////////////
// Board Capabilities
///////////////////////////////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------
// SDCard (not present)
//-------------------------------------------------------------------------

/*
 * SDCARD: not present on this board.
 */
#define FUNC_SDCARD_EN            (0)
#define SDCARD_OPEN_FILE_NUM_MAX  5
#define SDCARD_INTR_GPIO          (-1)

#define ESP_SD_PIN_CLK            (-1)
#define ESP_SD_PIN_CMD            (-1)
#define ESP_SD_PIN_D0             (-1)
#define ESP_SD_PIN_D1             (-1)
#define ESP_SD_PIN_D2             (-1)
#define ESP_SD_PIN_D3             (-1)
#define ESP_SD_PIN_D4             (-1)
#define ESP_SD_PIN_D5             (-1)
#define ESP_SD_PIN_D6             (-1)
#define ESP_SD_PIN_D7             (-1)
#define ESP_SD_PIN_CD             (-1)
#define ESP_SD_PIN_WP             (-1)

//-------------------------------------------------------------------------
// LEDs (not present)
//-------------------------------------------------------------------------

/*
 * LEDs: not present.
 */
#define FUNC_SYS_LEN_EN           (0)
#define GREEN_LED_GPIO            (-1)
#define BLUE_LED_GPIO             (-1)

//-------------------------------------------------------------------------
// Audio Path
//-------------------------------------------------------------------------

/*
 * Audio path:
 * - ESP32 I2S -> external DAC
 * - Separate external power amp
 */
#define FUNC_AUDIO_CODEC_EN       (1)
#define AUXIN_DETECT_GPIO         (-1)
#define HEADPHONE_DETECT          (-1)
#define PA_ENABLE_GPIO            (-1)
#define DAC_MUTE_GPIO             (21)  /* Active low: 0=mute, 1=unmute */
#define AMP_MUTE_GPIO             (22)  /* Active high: 1=mute, 0=unmute */
#define DAC_MUTE_LEVEL            (0)
#define AMP_MUTE_LEVEL            (1)
#define CODEC_ADC_I2S_PORT        ((i2s_port_t)0)
#define CODEC_ADC_BITS_PER_SAMPLE ((i2s_data_bit_width_t)16)
#define CODEC_ADC_SAMPLE_RATE     (48000)
#define RECORD_HARDWARE_AEC       (false)
#define BOARD_PA_GAIN             (0)

//-------------------------------------------------------------------------
// Algorithm Defaults
//-------------------------------------------------------------------------

/*
 * Algorithm stream defaults.
 */
#define AUDIO_ADC_INPUT_CH_FORMAT "N"

//-------------------------------------------------------------------------
// Codec Defaults
//-------------------------------------------------------------------------

extern audio_hal_func_t AUDIO_CODEC_ES7148_DEFAULT_HANDLE;
#define AUDIO_CODEC_DEFAULT_CONFIG() {                  \
    .adc_input  = AUDIO_HAL_ADC_INPUT_ALL,              \
    .dac_output = AUDIO_HAL_DAC_OUTPUT_ALL,             \
    .codec_mode = AUDIO_HAL_CODEC_MODE_BOTH,            \
    .i2s_iface = {                                      \
        .mode = AUDIO_HAL_MODE_SLAVE,                   \
        .fmt = AUDIO_HAL_I2S_NORMAL,                    \
        .samples = AUDIO_HAL_48K_SAMPLES,               \
        .bits = AUDIO_HAL_BIT_LENGTH_16BITS,            \
    },                                                  \
}

//-------------------------------------------------------------------------
// Buttons (not present)
//-------------------------------------------------------------------------

/*
 * Buttons: not present.
 */
#define FUNC_BUTTON_EN            (0)
#define INPUT_KEY_NUM             0
#define BUTTON_REC_ID             (-1)
#define BUTTON_MODE_ID            (-1)
#define BUTTON_SET_ID             (-1)
#define BUTTON_PLAY_ID            (-1)
#define BUTTON_VOLUP_ID           (-1)
#define BUTTON_VOLDOWN_ID         (-1)
#define BUTTON_COLOR_ID           (-1)

#define INPUT_KEY_DEFAULT_INFO() {}

#endif
