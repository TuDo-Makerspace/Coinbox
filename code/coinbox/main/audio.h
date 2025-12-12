#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gpio.h"

uint8_t master_volume_level = 100;

uint8_t lid_open_volume_level = 30;


uint8_t actual_lid_open_volume_level = 0;
uint8_t actual_master_volume_level = 0;
uint8_t track_volume_level = 100;

void audio_init();
void set_master_volume_level(uint8_t level);

void set_lid_level(bool lid_open);
void set_lid_open_volume_level(uint8_t level);
void set_track_volume_level(uint8_t level);

void update_volume_level();

TaskHandle_t create_play_audio_task(void);
static void play_audio_task(void *arg);

extern TaskHandle_t s_worker_task;