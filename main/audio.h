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

#pragma once

#include <stdbool.h>
#include <stddef.h>
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

typedef enum {
    AUDIO_PLAYBACK_START_RESULT_STARTED = 0,
    AUDIO_PLAYBACK_START_RESULT_SKIPPED,
} audio_playback_start_result_t;

typedef enum {
    AUDIO_PLAYBACK_SKIP_NONE = 0,
    AUDIO_PLAYBACK_SKIP_TRACK_VOLUME_ZERO,
} audio_playback_skip_reason_t;

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
float audio_test_current_volume_pct(void);
uint16_t audio_test_current_amplitude(void);
uint16_t audio_test_max_amplitude(void);
void audio_test_set_targets(float freq_hz, uint16_t amplitude);
void audio_test_set_volume_pct(float volume_pct);

//-------------------------------------------------------------------------
// Playback Mode
//-------------------------------------------------------------------------

esp_err_t audio_start_file(const char *name,
                           audio_playback_start_result_t *out_result,
                           audio_playback_skip_reason_t *out_skip_reason);
esp_err_t audio_start_file_with_volume(const char *name,
                                       uint8_t volume_pct,
                                       audio_playback_start_result_t *out_result,
                                       audio_playback_skip_reason_t *out_skip_reason);
bool audio_is_playing(void);
void audio_get_playback_status(bool *out_active,
                               char *out_name,
                               size_t out_name_size,
                               audio_playback_skip_reason_t *out_skip_reason,
                               bool consume_skip_notice);
const char *audio_playback_skip_reason_text(audio_playback_skip_reason_t reason);
void audio_get_last_playback_skip_notice(uint32_t *out_seq,
                                         uint64_t *out_timestamp_ms,
                                         audio_playback_skip_reason_t *out_reason,
                                         char *out_name,
                                         size_t out_name_size);

void audio_set_master_volume_level(uint8_t level);
void audio_set_track_volume_level(uint8_t level);
