#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"

///////////////////////////////////////////////////////////////////////////////////////////////////
// Constants
///////////////////////////////////////////////////////////////////////////////////////////////////

#define AUDIO_TEST_FILENAME      "test6165ms.mp3"
#define AUDIO_TEST_QUIET_DB      (-40)

#define AUDIO_TEST_SAMPLE_RATE    44100

#ifndef CONFIG_AUDIO_MAX_AMPLITUDE
#define CONFIG_AUDIO_MAX_AMPLITUDE 0x0030
#endif
#define AUDIO_TEST_SINE_AMPLITUDE CONFIG_AUDIO_MAX_AMPLITUDE

#define AUDIO_TEST_MIN_HZ      50.0f
#define AUDIO_TEST_MAX_HZ    8000.0f
#define AUDIO_TEST_DEFAULT_HZ 440.0f
#define AUDIO_TEST_DEFAULT_VOLUME_PCT 25U
#define AUDIO_TEST_DEFAULT_AMPLITUDE \
    ((uint16_t)((AUDIO_TEST_SINE_AMPLITUDE * AUDIO_TEST_DEFAULT_VOLUME_PCT) / 100U))

///////////////////////////////////////////////////////////////////////////////////////////////////
// Enums
///////////////////////////////////////////////////////////////////////////////////////////////////

typedef enum {
    AUDIO_MODE_UNINITIALIZED = 0,
    AUDIO_MODE_PLAYBACK,
    AUDIO_MODE_TEST,
} audio_mode_t;

///////////////////////////////////////////////////////////////////////////////////////////////////
// Interface
///////////////////////////////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------
// Common
//-------------------------------------------------------------------------

esp_err_t audio_init(void);
audio_mode_t audio_mode(void);
void audio_stop(void);

//-------------------------------------------------------------------------
// Test Mode
//-------------------------------------------------------------------------

esp_err_t audio_test_start(float freq_hz, uint16_t amplitude);
esp_err_t audio_test_start_sweep(void);
bool audio_test_is_running(void);
bool audio_test_sweep_is_running(void);
float audio_test_current_freq(void);
uint16_t audio_test_current_amplitude(void);
uint16_t audio_test_max_amplitude(void);
void audio_test_set_targets(float freq_hz, uint16_t amplitude);

//-------------------------------------------------------------------------
// Playback Mode
//-------------------------------------------------------------------------

esp_err_t audio_start_file(const char *name);
bool audio_is_playing(void);

void audio_set_master_volume_level(uint8_t level);
void audio_set_track_volume_level(uint8_t level);
void audio_set_lid_open_volume_level(uint8_t level);
void audio_set_lid_level(bool lid_open);
