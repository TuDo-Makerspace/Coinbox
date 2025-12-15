#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"

void configure_gpio();

void mute_output(bool mute);
esp_err_t set_amp_muted(bool mute);
esp_err_t toggle_amp_muted(bool *muted_out);
bool is_amp_muted(void);

extern volatile uint32_t laser_isr_count;
extern volatile uint32_t hall_isr_count;

extern bool laser_detection_enabled;
extern TimerHandle_t s_isr_timer;
extern TaskHandle_t  s_worker_task;

int gpio_get_laser_level(void);
int gpio_get_hall_level(void);
bool gpio_is_laser_beam_blocked(void);
bool gpio_is_lid_open(void);
