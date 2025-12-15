#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdkconfig.h"

#define AUDIO_TEST_SAMPLE_RATE    44100

#ifndef CONFIG_AUDIO_MAX_AMPLITUDE
#define CONFIG_AUDIO_MAX_AMPLITUDE 0x0030
#endif
#define AUDIO_TEST_SINE_AMPLITUDE CONFIG_AUDIO_MAX_AMPLITUDE

#define AUDIO_TEST_MIN_HZ      50.0f
#define AUDIO_TEST_MAX_HZ    8000.0f
#define AUDIO_TEST_DEFAULT_HZ 440.0f

esp_err_t audio_test_start(float freq_hz, uint16_t amplitude);
void audio_test_stop(void);
bool audio_test_is_running(void);
float audio_test_current_freq(void);
uint16_t audio_test_current_amplitude(void);
uint16_t audio_test_max_amplitude(void);
