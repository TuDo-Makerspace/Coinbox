#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define AUDIO_TEST_FILENAME      "test.mp3"
#define AUDIO_TEST_QUIET_DB      (-40)

esp_err_t audio_init(const char *base_path);
esp_err_t audio_start_file(const char *name);
void audio_stop(void);
bool audio_is_playing(void);

void set_master_volume_level(uint8_t level);
void set_track_volume_level(uint8_t level);
void set_lid_open_volume_level(uint8_t level);
void set_lid_level(bool lid_open);

TaskHandle_t create_play_audio_task(void);
