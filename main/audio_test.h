#pragma once

#include <stdbool.h>
#include "esp_err.h"

#define AUDIO_TEST_SAMPLE_RATE    44100
#define AUDIO_TEST_SINE_AMPLITUDE 0x0030  // ~-40 dBFS, safe test level

#define AUDIO_TEST_MIN_HZ      50.0f
#define AUDIO_TEST_MAX_HZ    8000.0f
#define AUDIO_TEST_DEFAULT_HZ 440.0f

esp_err_t audio_test_start(float freq_hz);
void audio_test_stop(void);
bool audio_test_is_running(void);
float audio_test_current_freq(void);
