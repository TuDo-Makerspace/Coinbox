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
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
#include <string.h>
#include "esp_err.h"
#include "sdkconfig.h"

///////////////////////////////////////////////////////////////////////////////////////////////////
// Constants
///////////////////////////////////////////////////////////////////////////////////////////////////

#define LASER_BLOCKED 1
#define HALL_LID_CLOSED 0
#define GPIO_EVENT_BUFFER_CAPACITY 128
#define GPIO_LASER_DEBOUNCE_DEFAULT_MS 30U
#define GPIO_HALL_DEBOUNCE_DEFAULT_MS 1000U
#define GPIO_LASER_TRIGGER_COOLDOWN_DEFAULT_MS 0U

typedef struct
{
    uint64_t timestamp_us;
    uint8_t level;
} gpio_edge_event_t;

typedef gpio_edge_event_t gpio_laser_event_t;
typedef gpio_edge_event_t gpio_hall_event_t;

typedef enum
{
    GPIO_RUNTIME_BOOTSTRAP = 0,
    GPIO_RUNTIME_MAIN_APP = 1,
} gpio_runtime_mode_t;

///////////////////////////////////////////////////////////////////////////////////////////////////
// Interface
///////////////////////////////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------
// Init
//-------------------------------------------------------------------------

esp_err_t gpio_init(void);
void gpio_set_runtime_mode(gpio_runtime_mode_t mode);
gpio_runtime_mode_t gpio_get_runtime_mode(void);

//-------------------------------------------------------------------------
// Laser
//-------------------------------------------------------------------------

int gpio_get_laser_level(void);
uint32_t gpio_get_laser_changes(void);
uint32_t gpio_get_laser_breaks(void);
size_t gpio_laser_events_drain(gpio_laser_event_t *out_events, size_t max_events, uint32_t *dropped_events);
void gpio_set_laser_debounce_ms(uint16_t debounce_ms);
uint16_t gpio_get_laser_debounce_ms(void);
void gpio_set_laser_trigger_cooldown_ms(uint32_t cooldown_ms);
uint32_t gpio_get_laser_trigger_cooldown_ms(void);

//-------------------------------------------------------------------------
// Hall/Lid detecion
//-------------------------------------------------------------------------

int gpio_get_hall_level(void);
uint32_t get_hall_changes(void);
size_t gpio_hall_events_drain(gpio_hall_event_t *out_events, size_t max_events, uint32_t *dropped_events);
void gpio_set_hall_debounce_ms(uint16_t debounce_ms);
uint16_t gpio_get_hall_debounce_ms(void);
esp_err_t gpio_set_lid_open_sound(const char *name);
void gpio_set_lid_open_volume_pct(uint8_t volume_pct);

#if CONFIG_TEST_GPIO_INJECTION
esp_err_t gpio_test_set_laser_level(int level);
esp_err_t gpio_test_pulse_laser(uint32_t count, uint32_t interval_ms);
esp_err_t gpio_test_set_hall_level(int level);
#endif
