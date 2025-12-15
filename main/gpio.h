#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void configure_gpio();

void mute_output(bool mute);

static void IRAM_ATTR gpio_laser_isr_handler(void *arg);
static void IRAM_ATTR gpio_hall_isr_handler(void *arg);

static void isr_timer_callback(TimerHandle_t xTimer);

extern volatile uint32_t laser_isr_count;
extern volatile uint32_t hall_isr_count;

extern bool laser_detection_enabled;
extern TimerHandle_t s_isr_timer;
extern TaskHandle_t  s_worker_task;

int gpio_get_laser_level(void);
int gpio_get_hall_level(void);
bool gpio_is_laser_beam_blocked(void);
bool gpio_is_lid_open(void);
