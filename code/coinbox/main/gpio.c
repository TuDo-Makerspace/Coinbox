#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include "esp_log.h"
#include "gpio.h"
#include "audio.h"

#define TAG "gpio"

// GPIO definitions
// GPIO outputs
#define GPIO_MUTE_DAC 21
#define GPIO_MUTE_AMP 22
#define GPIO_OUTPUT_PIN_SEL ((1ULL << GPIO_MUTE_DAC) | (1ULL << GPIO_MUTE_AMP))

// GPIO inputs
#define GPIO_LASER_RECEIVER 23
#define GPIO_HALL_LID_SENSOR 2
#define GPIO_INPUT_PIN_SEL ((1ULL << GPIO_LASER_RECEIVER) | (1ULL << GPIO_HALL_LID_SENSOR))

void configure_gpio()
{
    // Create a one-shot software timer with period = 1 tick
    s_isr_timer = xTimerCreate(
        "isr_timer",
        pdMS_TO_TICKS(1), // 1 tick; adjust if you want a bit more delay
        pdFALSE,          // one-shot
        NULL,
        isr_timer_callback);

    if (s_isr_timer == NULL)
    {
        printf("Failed to create ISR timer!\n");
        return;
    }

    // zero-initialize the config structure.
    gpio_config_t io_conf = {};
    // disable interrupt
    io_conf.intr_type = GPIO_INTR_DISABLE;
    // set as output mode
    io_conf.mode = GPIO_MODE_OUTPUT;
    // bit mask of the pins that you want to set,e.g.GPIO18/19
    io_conf.pin_bit_mask = GPIO_OUTPUT_PIN_SEL;
    // disable pull-down mode
    io_conf.pull_down_en = 0;
    // disable pull-up mode
    io_conf.pull_up_en = 0;
    // configure GPIO with the given settings
    gpio_config(&io_conf);

    // Configure input pins with interrupts
    gpio_config_t io_conf_in = {};
    io_conf_in.intr_type = GPIO_INTR_ANYEDGE; // enable interrupts on any edge (change to NEG/POSEDGE if needed)
    io_conf_in.mode = GPIO_MODE_INPUT;
    io_conf_in.pin_bit_mask = GPIO_INPUT_PIN_SEL;
    io_conf_in.pull_up_en = 1; // enable pull-up if required by your sensors
    io_conf_in.pull_down_en = 0;
    gpio_config(&io_conf_in);

    // Install ISR service and attach handlers (implement gpio_input_isr_handler elsewhere)
    gpio_install_isr_service(0); // or ESP_INTR_FLAG_DEFAULT
    gpio_isr_handler_add(GPIO_LASER_RECEIVER, gpio_laser_isr_handler, (void *)GPIO_LASER_RECEIVER);
    gpio_isr_handler_add(GPIO_HALL_LID_SENSOR, gpio_hall_isr_handler, (void *)GPIO_HALL_LID_SENSOR);

    printf("ISR + timer setup complete\n");
}

void mute_output(bool mute)
{
    if (mute)
    {
        ESP_LOGI(TAG, "Muting output");
        gpio_set_level(GPIO_MUTE_AMP, 1);
        gpio_set_level(GPIO_MUTE_DAC, 0);
    }
    else
    {
        ESP_LOGI(TAG, "Unmuting output");
        gpio_set_level(GPIO_MUTE_AMP, 0);
        vTaskDelay(configTICK_RATE_HZ / 20); // delay dac 50ms to give amp some time to turn on before soft unmute
        gpio_set_level(GPIO_MUTE_DAC, 1);
    }
}

// lid open, dont detect coins
// lid open, volume to minimal level to prevent laud noises
uint16_t debaunce_time_laser_ms = 50;
uint16_t debaunce_time_hall_ms = 3000;

static void IRAM_ATTR gpio_laser_isr_handler(void *arg)
{
    // GPIO is high when laser detects a coin

    uint32_t gpio_num = (uint32_t)arg;
    int level = gpio_get_level(gpio_num);

    static TickType_t last_tick = 0;
    const TickType_t debounce_ticks = pdMS_TO_TICKS(debaunce_time_laser_ms);

    TickType_t now = xTaskGetTickCountFromISR();
    if (now - last_tick < debounce_ticks)
    {
        // Bounce: ignore
        return;
    }
    last_tick = now;

    if (level == 1 && laser_detection_enabled)
    {
        // Coin detected
        laser_isr_count++;

        BaseType_t xHigherPriorityTaskWoken = pdFALSE;

        // One-shot timer with period = 1 tick. Resetting it here
        // means "fire 1 tick after the last interrupt".
        if (xTimerResetFromISR(s_isr_timer, &xHigherPriorityTaskWoken) != pdPASS)
        {
            // Could not queue command to timer task (rare, but you can log later)
        }

        if (xHigherPriorityTaskWoken)
        {
            portYIELD_FROM_ISR();
        }
    }
}

static void IRAM_ATTR gpio_hall_isr_handler(void *arg)
{
    // GPIO is low when hall sensor detects lid closed, high when lid open

    uint32_t gpio_num = (uint32_t)arg;
    int level = gpio_get_level(gpio_num);

    static TickType_t last_tick = 0;
    const TickType_t debounce_ticks = pdMS_TO_TICKS(debaunce_time_hall_ms);

    TickType_t now = xTaskGetTickCountFromISR();
    if (now - last_tick < debounce_ticks)
    {
        // Bounce: ignore
        return;
    }
    last_tick = now;

    if (level == 0)
    {
        // Lid is closed, turn up volume, enable coin detection
        set_lid_level(false);
        laser_detection_enabled = true;
    }
    else
    {
        // Lid is open, turn down volume, disable coin detection
        hall_isr_count++;
        set_lid_level(true);
        laser_detection_enabled = false;
    }
}

static void isr_timer_callback(TimerHandle_t xTimer)
{
    (void)xTimer;

    // If a worker task is still "considered alive", kill it
    if (s_worker_task != NULL) {
        printf("Timer: previous worker (%p) still alive, deleting it\n", (void *)s_worker_task);
        vTaskDelete(s_worker_task);
        s_worker_task = NULL;
    }

    // Create a new worker task
    BaseType_t res = xTaskCreate(
        play_audio_task,
        "isr_worker",
        2048,          // stack size
        NULL,          // arg
        5,             // priority
        &s_worker_task // out handle
    );

    if (res != pdPASS) {
        printf("Timer: failed to create worker task!\n");
        s_worker_task = NULL;
    } else {
        printf("Timer: created new worker task (%p)\n", (void *)s_worker_task);
    }
}

static void play_audio_task(void *arg)
{
    // Just print something; you can add more logic here
    printf("Worker task started (handle=%p)\n", (void *)xTaskGetCurrentTaskHandle());

    // Simulate a bit of work
    vTaskDelay(pdMS_TO_TICKS(10));

    printf("Worker task exiting (handle=%p)\n", (void *)xTaskGetCurrentTaskHandle());

    // Clear global handle before self-delete (best-effort)
    s_worker_task = NULL;

    // Kill this task
    vTaskDelete(NULL);
}
