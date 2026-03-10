#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_log.h"
#include "esp_err.h"
#include "gpio.h"
#include "audio.h"
#include "files.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"

#define TAG "gpio"

///////////////////////////////////////////////////////////////////////////////////////////////////
// Constants
///////////////////////////////////////////////////////////////////////////////////////////////////

// GPIO inputs
#define GPIO_LASER_RECEIVER 23
#define GPIO_HALL_LID_SENSOR 2
#define GPIO_INPUT_PIN_SEL ((1ULL << GPIO_LASER_RECEIVER) | (1ULL << GPIO_HALL_LID_SENSOR))
#define LASER_EVENT_TASK_STACK (6144)
#define HALL_EVENT_TASK_STACK  (6144)

///////////////////////////////////////////////////////////////////////////////////////////////////
// Structs
///////////////////////////////////////////////////////////////////////////////////////////////////

typedef struct
{
    gpio_edge_event_t events[GPIO_EVENT_BUFFER_CAPACITY];
    uint16_t capacity;
    volatile uint16_t head;
    volatile uint16_t count;
    volatile uint32_t dropped;
    volatile uint32_t rises;
    volatile uint32_t falls;
    volatile uint8_t last_level;
    volatile bool last_level_valid;
    portMUX_TYPE mux;
} gpio_history_t;


///////////////////////////////////////////////////////////////////////////////////////////////////
// Forward Declarations
///////////////////////////////////////////////////////////////////////////////////////////////////

static void gpio_laser_isr_handler(void *arg);
static void gpio_hall_isr_handler(void *arg);
static void laser_event_task(void *arg);
static void laser_handle_blocked_trigger(TickType_t trigger_tick);
static void hall_event_task(void *arg);
static void hall_handle_open_trigger(TickType_t trigger_tick);

///////////////////////////////////////////////////////////////////////////////////////////////////
// Vars
///////////////////////////////////////////////////////////////////////////////////////////////////

// IRQ history ring-buffers to build settings graphs

static gpio_history_t s_laser_history = {
    .capacity = GPIO_EVENT_BUFFER_CAPACITY,
    .head = 0,
    .count = 0,
    .dropped = 0,
    .rises = 0,
    .falls = 0,
    .last_level = 0,
    .last_level_valid = false,
    .mux = portMUX_INITIALIZER_UNLOCKED,
};

static gpio_history_t s_hall_history = {
    .capacity = GPIO_EVENT_BUFFER_CAPACITY,
    .head = 0,
    .count = 0,
    .dropped = 0,
    .rises = 0,
    .falls = 0,
    .last_level = 0,
    .last_level_valid = false,
    .mux = portMUX_INITIALIZER_UNLOCKED,
};

static volatile gpio_runtime_mode_t s_runtime_mode = GPIO_RUNTIME_BOOTSTRAP;

// Laser debouncing
static uint16_t s_debounce_time_laser_ms = GPIO_LASER_DEBOUNCE_DEFAULT_MS;
static uint16_t s_debounce_time_hall_ms = GPIO_HALL_DEBOUNCE_DEFAULT_MS;
static uint32_t s_laser_trigger_cooldown_ms = GPIO_LASER_TRIGGER_COOLDOWN_DEFAULT_MS;

// Laser task
static TaskHandle_t s_laser_task;
static TaskHandle_t s_hall_task;
static TickType_t s_laser_last_play_tick;
static TickType_t s_laser_last_trigger_tick;
static TickType_t s_hall_last_open_tick;
static TickType_t s_hall_last_close_tick;
static char s_lid_open_sound[FILE_ENTRY_NAME_MAX] = {0};
static uint8_t s_lid_open_volume_pct = 100;

#if CONFIG_TEST_GPIO_INJECTION
static volatile bool s_test_laser_override_valid;
static volatile uint8_t s_test_laser_override_level;
static volatile bool s_test_hall_override_valid;
static volatile uint8_t s_test_hall_override_level;
static TickType_t s_test_hall_synthetic_tick;
#endif

///////////////////////////////////////////////////////////////////////////////////////////////////
// Handlers
///////////////////////////////////////////////////////////////////////////////////////////////////

static inline void IRAM_ATTR gpio_input_event_push_from_isr(gpio_history_t *input, uint8_t level)
{
    const uint64_t ts_us = (uint64_t)esp_timer_get_time();
    const uint8_t normalized_level = level ? 1 : 0;

    taskENTER_CRITICAL_ISR(&input->mux);
    if (input->last_level_valid) {
        if (normalized_level != input->last_level) {
            if (normalized_level) {
                input->rises++;
            } else {
                input->falls++;
            }
        }
    } else {
        input->last_level_valid = true;
    }
    input->last_level = normalized_level;

    input->events[input->head].timestamp_us = ts_us;
    input->events[input->head].level = normalized_level;
    input->head = (uint16_t)((input->head + 1) % input->capacity);
    if (input->count < input->capacity) {
        input->count++;
    } else {
        input->dropped++;
    }
    taskEXIT_CRITICAL_ISR(&input->mux);
}

static inline void gpio_input_event_push(gpio_history_t *input, uint8_t level)
{
    const uint64_t ts_us = (uint64_t)esp_timer_get_time();
    const uint8_t normalized_level = level ? 1 : 0;

    taskENTER_CRITICAL(&input->mux);
    if (input->last_level_valid) {
        if (normalized_level != input->last_level) {
            if (normalized_level) {
                input->rises++;
            } else {
                input->falls++;
            }
        }
    } else {
        input->last_level_valid = true;
    }
    input->last_level = normalized_level;

    input->events[input->head].timestamp_us = ts_us;
    input->events[input->head].level = normalized_level;
    input->head = (uint16_t)((input->head + 1) % input->capacity);
    if (input->count < input->capacity) {
        input->count++;
    } else {
        input->dropped++;
    }
    taskEXIT_CRITICAL(&input->mux);
}

static size_t gpio_input_events_drain(gpio_history_t *input,
                                      gpio_edge_event_t *out_events,
                                      size_t max_events,
                                      uint32_t *dropped_events)
{
    if (!out_events || max_events == 0) {
        return 0;
    }

    size_t copied = 0;
    uint32_t dropped = 0;

    taskENTER_CRITICAL(&input->mux);
    uint16_t count = input->count;
    uint16_t head = input->head;
    uint16_t tail = (uint16_t)((head + input->capacity - count) % input->capacity);

    if (count > max_events) {
        uint16_t skip = (uint16_t)(count - max_events);
        tail = (uint16_t)((tail + skip) % input->capacity);
        dropped += skip;
        count = (uint16_t)max_events;
    }

    for (uint16_t i = 0; i < count; ++i) {
        uint16_t idx = (uint16_t)((tail + i) % input->capacity);
        out_events[copied++] = input->events[idx];
    }

    dropped += input->dropped;
    input->head = 0;
    input->count = 0;
    input->dropped = 0;
    taskEXIT_CRITICAL(&input->mux);

    if (dropped_events) {
        *dropped_events = dropped;
    }
    return copied;
}

static uint32_t gpio_input_get_rises(gpio_history_t *input)
{
    uint32_t rises = 0;
    taskENTER_CRITICAL(&input->mux);
    rises = input->rises;
    taskEXIT_CRITICAL(&input->mux);
    return rises;
}

static uint32_t gpio_input_get_changes(gpio_history_t *input)
{
    uint32_t rises = 0;
    uint32_t falls = 0;
    taskENTER_CRITICAL(&input->mux);
    rises = input->rises;
    falls = input->falls;
    taskEXIT_CRITICAL(&input->mux);
    return rises + falls;
}

static void IRAM_ATTR gpio_laser_isr_handler(void *arg)
{
    (void)arg; // unused for now
    const int level = gpio_get_level(GPIO_LASER_RECEIVER);

    gpio_input_event_push_from_isr(&s_laser_history, (uint8_t)(level ? 1 : 0));

    if (!s_laser_task || s_runtime_mode != GPIO_RUNTIME_MAIN_APP) {
        return;
    }

    uint32_t tick = (uint32_t)xTaskGetTickCountFromISR();
    BaseType_t hp_task_woken = pdFALSE;
    // Keep only the latest trigger value; do not enqueue a backlog.
    xTaskNotifyFromISR(s_laser_task, tick, eSetValueWithOverwrite, &hp_task_woken);
    if (hp_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void IRAM_ATTR gpio_hall_isr_handler(void *arg)
{
    (void)arg; // unused for now
    const int level = gpio_get_level(GPIO_HALL_LID_SENSOR);

    gpio_input_event_push_from_isr(&s_hall_history, (uint8_t)(level ? 1 : 0));

    if (!s_hall_task || s_runtime_mode != GPIO_RUNTIME_MAIN_APP) {
        return;
    }

    uint32_t tick = (uint32_t)xTaskGetTickCountFromISR();
    BaseType_t hp_task_woken = pdFALSE;
    xTaskNotifyFromISR(s_hall_task, tick, eSetValueWithOverwrite, &hp_task_woken);
    if (hp_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Tasks
///////////////////////////////////////////////////////////////////////////////////////////////////

static void laser_event_task(void *arg)
{
    (void)arg;

    uint32_t evt_tick = 0;
    while (true) {
        if (xTaskNotifyWait(0, UINT32_MAX, &evt_tick, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        // If multiple triggers arrived before we started processing, use the newest.
        uint32_t latest_tick = evt_tick;
        while (xTaskNotifyWait(0, UINT32_MAX, &latest_tick, 0) == pdTRUE) {
            evt_tick = latest_tick;
        }
        (void)evt_tick;

        if (s_runtime_mode != GPIO_RUNTIME_MAIN_APP) {
            continue;
        }

        if (gpio_get_laser_level() != LASER_BLOCKED) {
            continue; // only trigger playback for blocked-beam level
        }

        const TickType_t trigger_tick = evt_tick ? (TickType_t)evt_tick : xTaskGetTickCount();
        laser_handle_blocked_trigger(trigger_tick);
    }
}

static void hall_event_task(void *arg)
{
    (void)arg;

    uint32_t evt_tick = 0;
    while (true) {
        if (xTaskNotifyWait(0, UINT32_MAX, &evt_tick, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        uint32_t latest_tick = evt_tick;
        while (xTaskNotifyWait(0, UINT32_MAX, &latest_tick, 0) == pdTRUE) {
            evt_tick = latest_tick;
        }
        (void)evt_tick;

        if (s_runtime_mode != GPIO_RUNTIME_MAIN_APP) {
            continue;
        }

        const TickType_t trigger_tick = evt_tick ? (TickType_t)evt_tick : xTaskGetTickCount();
        if (gpio_get_hall_level() == HALL_LID_CLOSED) {
            s_hall_last_close_tick = trigger_tick;
            continue;
        }

        hall_handle_open_trigger(trigger_tick);
    }
}

static void laser_handle_blocked_trigger(TickType_t trigger_tick)
{
    const TickType_t debounce_ticks = s_debounce_time_laser_ms
        ? pdMS_TO_TICKS(s_debounce_time_laser_ms)
        : 0;
    const TickType_t cooldown_ticks = s_laser_trigger_cooldown_ms
        ? pdMS_TO_TICKS(s_laser_trigger_cooldown_ms)
        : 0;

    if (debounce_ticks > 0 &&
        s_laser_last_trigger_tick != 0 &&
        (trigger_tick - s_laser_last_trigger_tick) < debounce_ticks) {
        return;
    }
    s_laser_last_trigger_tick = trigger_tick;

    if (cooldown_ticks > 0) {
        if ((trigger_tick - s_laser_last_play_tick) < cooldown_ticks) {
            return;
        }
    }

    if (gpio_get_hall_level() != HALL_LID_CLOSED) {
        ESP_LOGI(TAG, "Coin detected, but lid is open; playback is disabled");
        return;
    }

    if (cooldown_ticks > 0) {
        s_laser_last_play_tick = trigger_tick;
    }

    char selected_name[FILE_ENTRY_NAME_MAX] = {0};
    uint32_t total_weight = 0;
    size_t candidates = 0;

    esp_err_t pick_err = files_pick_weighted_enabled(
        selected_name,
        sizeof(selected_name),
        &total_weight,
        &candidates);
    if (pick_err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Coin detected, but no enabled weighted sounds found (candidates=%u, total_weight=%u)",
                 (unsigned)candidates,
                 (unsigned)total_weight);
        return;
    }

    ESP_LOGI(TAG,
             "Coin detected! Starting playback of %s (candidates=%u, total_weight=%u)",
             selected_name,
             (unsigned)candidates,
             (unsigned)total_weight);

    audio_playback_start_result_t start_result = AUDIO_PLAYBACK_START_RESULT_STARTED;
    audio_playback_skip_reason_t skip_reason = AUDIO_PLAYBACK_SKIP_NONE;
    esp_err_t err = audio_start_file(selected_name, &start_result, &skip_reason);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start audio on coin detection: %s", esp_err_to_name(err));
    } else if (start_result == AUDIO_PLAYBACK_START_RESULT_STARTED) {
        ESP_LOGI(TAG, "Playback started for file: %s", selected_name);
    } else {
        ESP_LOGW(TAG,
                 "Playback skipped for file: %s (%s)",
                 selected_name,
                 audio_playback_skip_reason_text(skip_reason));
    }
}

static void hall_handle_open_trigger(TickType_t trigger_tick)
{
    const TickType_t debounce_ticks = s_debounce_time_hall_ms
        ? pdMS_TO_TICKS(s_debounce_time_hall_ms)
        : 0;

    if (debounce_ticks > 0 &&
        s_hall_last_open_tick != 0 &&
        (trigger_tick - s_hall_last_open_tick) < debounce_ticks) {
        return;
    }
    if (debounce_ticks > 0 &&
        s_hall_last_open_tick != 0 &&
        s_hall_last_close_tick != 0 &&
        s_hall_last_close_tick > s_hall_last_open_tick &&
        (trigger_tick - s_hall_last_close_tick) < debounce_ticks) {
        return;
    }
    s_hall_last_open_tick = trigger_tick;

    if (s_lid_open_sound[0] == '\0') {
        ESP_LOGI(TAG, "Lid opened, but no lid-open sound is configured");
        return;
    }

    if (s_lid_open_volume_pct == 0) {
        ESP_LOGI(TAG, "Lid opened, but lid-open volume is 0%%");
        return;
    }

    file_properties_t meta;
    if (files_read_meta(s_lid_open_sound, &meta) != ESP_OK) {
        ESP_LOGW(TAG, "Configured lid-open sound does not exist: %s", s_lid_open_sound);
        return;
    }

    ESP_LOGI(TAG, "Lid opened! Starting playback of %s", s_lid_open_sound);

    audio_playback_start_result_t start_result = AUDIO_PLAYBACK_START_RESULT_STARTED;
    audio_playback_skip_reason_t skip_reason = AUDIO_PLAYBACK_SKIP_NONE;
    esp_err_t err = audio_start_file_with_volume(s_lid_open_sound,
                                                 s_lid_open_volume_pct,
                                                 &start_result,
                                                 &skip_reason);
    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "Configured lid-open sound does not exist: %s", s_lid_open_sound);
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start lid-open audio: %s", esp_err_to_name(err));
    } else if (start_result == AUDIO_PLAYBACK_START_RESULT_STARTED) {
        ESP_LOGI(TAG, "Playback started for lid-open sound: %s", s_lid_open_sound);
    } else {
        ESP_LOGW(TAG,
                 "Lid-open playback skipped for file: %s (%s)",
                 s_lid_open_sound,
                 audio_playback_skip_reason_text(skip_reason));
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Public Interface
///////////////////////////////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------
// Init
//-------------------------------------------------------------------------

esp_err_t gpio_init(void)
{
    esp_err_t err;

    // Configure input pins with interrupts
    // Laser receiver: keep internal pull-up enabled
    gpio_config_t io_conf_laser = {};
    io_conf_laser.intr_type = GPIO_INTR_ANYEDGE; // capture all state transitions for settings graph
    io_conf_laser.mode = GPIO_MODE_INPUT;
    io_conf_laser.pin_bit_mask = (1ULL << GPIO_LASER_RECEIVER);
    io_conf_laser.pull_up_en = 1;
    io_conf_laser.pull_down_en = 0;
    err = gpio_config(&io_conf_laser);
    if (err != ESP_OK) {
        return err;
    }

    // Hall lid sensor: external pull already present, so disable internal pull-up
    gpio_config_t io_conf_hall = {};
    io_conf_hall.intr_type = GPIO_INTR_ANYEDGE;
    io_conf_hall.mode = GPIO_MODE_INPUT;
    io_conf_hall.pin_bit_mask = (1ULL << GPIO_HALL_LID_SENSOR);
    io_conf_hall.pull_up_en = 0;
    io_conf_hall.pull_down_en = 0;
    err = gpio_config(&io_conf_hall);
    if (err != ESP_OK) {
        return err;
    }

    if (!s_laser_task) {
        // Slightly larger stack; audio_start_file and logging use some stack.
        BaseType_t res = xTaskCreate(laser_event_task, "laser-events", LASER_EVENT_TASK_STACK, NULL, tskIDLE_PRIORITY + 6, &s_laser_task);
        if (res != pdPASS) {
            ESP_LOGE(TAG, "Failed to create laser event task");
            s_laser_task = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    if (!s_hall_task) {
        BaseType_t res = xTaskCreate(hall_event_task, "hall-events", HALL_EVENT_TASK_STACK, NULL, tskIDLE_PRIORITY + 5, &s_hall_task);
        if (res != pdPASS) {
            ESP_LOGE(TAG, "Failed to create hall event task");
            s_hall_task = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    // Install ISR service and attach handlers (implement gpio_input_isr_handler elsewhere)
    err = gpio_install_isr_service(0); // or ESP_INTR_FLAG_DEFAULT
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = gpio_isr_handler_add(GPIO_LASER_RECEIVER, gpio_laser_isr_handler, (void *)GPIO_LASER_RECEIVER);
    if (err != ESP_OK) {
        return err;
    }

    err = gpio_isr_handler_add(GPIO_HALL_LID_SENSOR, gpio_hall_isr_handler, (void *)GPIO_HALL_LID_SENSOR);
    if (err != ESP_OK) {
        return err;
    }

    taskENTER_CRITICAL(&s_laser_history.mux);
    s_laser_history.last_level = (uint8_t)(gpio_get_level(GPIO_LASER_RECEIVER) ? 1 : 0);
    s_laser_history.last_level_valid = true;
    taskEXIT_CRITICAL(&s_laser_history.mux);

    taskENTER_CRITICAL(&s_hall_history.mux);
    s_hall_history.last_level = (uint8_t)(gpio_get_level(GPIO_HALL_LID_SENSOR) ? 1 : 0);
    s_hall_history.last_level_valid = true;
    taskEXIT_CRITICAL(&s_hall_history.mux);

    ESP_LOGI(TAG, "ISR + timer setup complete");
    return ESP_OK;
}

void gpio_set_runtime_mode(gpio_runtime_mode_t mode)
{
    s_runtime_mode = mode;
}

gpio_runtime_mode_t gpio_get_runtime_mode(void)
{
    return s_runtime_mode;
}

//-------------------------------------------------------------------------
// Laser
//-------------------------------------------------------------------------

int gpio_get_laser_level(void)
{
#if CONFIG_TEST_GPIO_INJECTION
    if (s_test_laser_override_valid) {
        return s_test_laser_override_level ? 1 : 0;
    }
#endif
    return gpio_get_level(GPIO_LASER_RECEIVER);
}

uint32_t gpio_get_laser_changes(void)
{
    return gpio_input_get_changes(&s_laser_history);
}

uint32_t gpio_get_laser_breaks(void)
{
    return gpio_input_get_rises(&s_laser_history);
}

size_t gpio_laser_events_drain(gpio_laser_event_t *out_events, size_t max_events, uint32_t *dropped_events)
{
    return gpio_input_events_drain(&s_laser_history, out_events, max_events, dropped_events);
}

void gpio_set_laser_debounce_ms(uint16_t debounce_ms)
{
    s_debounce_time_laser_ms = debounce_ms;
}

uint16_t gpio_get_laser_debounce_ms(void)
{
    return s_debounce_time_laser_ms;
}

void gpio_set_laser_trigger_cooldown_ms(uint32_t cooldown_ms)
{
    s_laser_trigger_cooldown_ms = cooldown_ms;
}

uint32_t gpio_get_laser_trigger_cooldown_ms(void)
{
    return s_laser_trigger_cooldown_ms;
}

//-------------------------------------------------------------------------
// Hall/Lid detection
//-------------------------------------------------------------------------

int gpio_get_hall_level(void)
{
#if CONFIG_TEST_GPIO_INJECTION
    if (s_test_hall_override_valid) {
        return s_test_hall_override_level ? 1 : 0;
    }
#endif
    return gpio_get_level(GPIO_HALL_LID_SENSOR);
}


uint32_t get_hall_changes(void)
{
    return gpio_input_get_changes(&s_hall_history);
}

size_t gpio_hall_events_drain(gpio_hall_event_t *out_events, size_t max_events, uint32_t *dropped_events)
{
    return gpio_input_events_drain(&s_hall_history, out_events, max_events, dropped_events);
}

void gpio_set_hall_debounce_ms(uint16_t debounce_ms)
{
    s_debounce_time_hall_ms = debounce_ms;
}

uint16_t gpio_get_hall_debounce_ms(void)
{
    return s_debounce_time_hall_ms;
}

esp_err_t gpio_set_lid_open_sound(const char *name)
{
    if (!name || name[0] == '\0') {
        s_lid_open_sound[0] = '\0';
        return ESP_OK;
    }
    if (strchr(name, '/') || strchr(name, '\\')) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlcpy(s_lid_open_sound, name, sizeof(s_lid_open_sound)) >= sizeof(s_lid_open_sound)) {
        s_lid_open_sound[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

void gpio_set_lid_open_volume_pct(uint8_t volume_pct)
{
    s_lid_open_volume_pct = (volume_pct > 100U) ? 100U : volume_pct;
}

#if CONFIG_TEST_GPIO_INJECTION
esp_err_t gpio_test_set_laser_level(int level)
{
    if (level != 0 && level != 1) {
        return ESP_ERR_INVALID_ARG;
    }

    s_test_laser_override_level = (uint8_t)level;
    s_test_laser_override_valid = true;
    gpio_input_event_push(&s_laser_history, (uint8_t)level);

    if (level == LASER_BLOCKED && s_laser_task && s_runtime_mode == GPIO_RUNTIME_MAIN_APP) {
        uint32_t tick = (uint32_t)xTaskGetTickCount();
        xTaskNotify(s_laser_task, tick, eSetValueWithOverwrite);
    }

    return ESP_OK;
}

esp_err_t gpio_test_pulse_laser(uint32_t count, uint32_t interval_ms)
{
    if (count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const TickType_t interval_ticks = interval_ms ? pdMS_TO_TICKS(interval_ms) : 0;
    TickType_t trigger_tick = xTaskGetTickCount();
    for (uint32_t i = 0; i < count; ++i) {
        s_test_laser_override_level = 0;
        s_test_laser_override_valid = true;
        gpio_input_event_push(&s_laser_history, 0);

        s_test_laser_override_level = LASER_BLOCKED;
        s_test_laser_override_valid = true;
        gpio_input_event_push(&s_laser_history, LASER_BLOCKED);

        if (s_runtime_mode == GPIO_RUNTIME_MAIN_APP) {
            laser_handle_blocked_trigger(trigger_tick);
        }
        trigger_tick += interval_ticks;
    }

    return ESP_OK;
}

esp_err_t gpio_test_set_hall_level(int level)
{
    if (level != 0 && level != 1) {
        return ESP_ERR_INVALID_ARG;
    }

    s_test_hall_override_level = (uint8_t)level;
    s_test_hall_override_valid = true;
    gpio_input_event_push(&s_hall_history, (uint8_t)level);

    if (s_hall_task && s_runtime_mode == GPIO_RUNTIME_MAIN_APP) {
        TickType_t step = pdMS_TO_TICKS(25);
        if (step == 0) {
            step = 1;
        }
        s_test_hall_synthetic_tick += step;
        uint32_t tick = (uint32_t)s_test_hall_synthetic_tick;
        xTaskNotify(s_hall_task, tick, eSetValueWithOverwrite);
    }

    return ESP_OK;
}
#endif
