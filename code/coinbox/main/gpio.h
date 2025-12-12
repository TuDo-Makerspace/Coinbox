#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>

volatile uint32_t laser_isr_count = 0;
volatile uint32_t hall_isr_count = 0;

bool laser_detection_enabled = true;
static TimerHandle_t s_isr_timer = NULL;
static TaskHandle_t  s_worker_task = NULL;

void configure_gpio();

void mute_output(bool mute);

static void IRAM_ATTR gpio_laser_isr_handler(void *arg);
static void IRAM_ATTR gpio_hall_isr_handler(void *arg);

\