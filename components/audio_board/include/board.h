/*
 * Minimal audio board API for Coinbox.
 */

#ifndef _AUDIO_BOARD_H_
#define _AUDIO_BOARD_H_

#include "audio_hal.h"
#include "board_def.h"
#include "board_pins_config.h"
#include "display_service.h"
#include "esp_peripherals.h"
#include "periph_sdcard.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

///////////////////////////////////////////////////////////////////////////////////////////////////
// Types
///////////////////////////////////////////////////////////////////////////////////////////////////

struct audio_board_handle {
    audio_hal_handle_t audio_hal;
};

typedef struct audio_board_handle *audio_board_handle_t;

///////////////////////////////////////////////////////////////////////////////////////////////////
// Board Lifecycle
///////////////////////////////////////////////////////////////////////////////////////////////////

audio_board_handle_t audio_board_init(void);
audio_board_handle_t audio_board_get_handle(void);
esp_err_t audio_board_deinit(audio_board_handle_t audio_board);

///////////////////////////////////////////////////////////////////////////////////////////////////
// Codec / Optional Peripherals
///////////////////////////////////////////////////////////////////////////////////////////////////

audio_hal_handle_t audio_board_codec_init(void);
display_service_handle_t audio_board_led_init(void);
esp_err_t audio_board_key_init(esp_periph_set_handle_t set);
esp_err_t audio_board_sdcard_init(esp_periph_set_handle_t set, periph_sdcard_mode_t mode);

///////////////////////////////////////////////////////////////////////////////////////////////////
// Output Control
///////////////////////////////////////////////////////////////////////////////////////////////////

/* Board-level output control (DAC + power amp mute pins). */
esp_err_t audio_board_outputs_init(void);
void audio_board_outputs_mute(void);
void audio_board_outputs_unmute(void);

#if CONFIG_TEST_GPIO_INJECTION
int audio_board_test_get_amp_mute_level(void);
int audio_board_test_get_dac_mute_level(void);
esp_err_t audio_board_test_set_amp_mute_level(int level);
esp_err_t audio_board_test_set_dac_mute_level(int level);
#endif

#ifdef __cplusplus
}
#endif

#endif
