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

//-------------------------------------------------------------------------
// Hall/Lid detecion
//-------------------------------------------------------------------------

int gpio_get_hall_level(void);
uint32_t get_hall_changes(void);
size_t gpio_hall_events_drain(gpio_hall_event_t *out_events, size_t max_events, uint32_t *dropped_events);

#if CONFIG_TEST_GPIO_INJECTION
esp_err_t gpio_test_set_laser_level(int level);
esp_err_t gpio_test_set_hall_level(int level);
#endif
