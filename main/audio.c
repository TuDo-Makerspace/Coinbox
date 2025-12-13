#include "audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <stdbool.h>
#include "esp_log.h"

#define TAG "audio"

uint8_t master_volume_level = 100;

uint8_t lid_open_volume_level = 30;

uint8_t actual_lid_open_volume_level = 0;
uint8_t actual_master_volume_level = 0;
uint8_t track_volume_level = 100;

static pthread_mutex_t volume_mutex = PTHREAD_MUTEX_INITIALIZER;

void audio_init()
{
    // Initialize audio hardware here
    // This is a placeholder for actual audio initialization code
    printf("Audio system initialized.\n");
}

void set_master_volume_level(uint8_t level)
{
    if (level > 100)
    {
        level = 100;
    }
    
    pthread_mutex_lock(&volume_mutex);
    master_volume_level = level;
    pthread_mutex_unlock(&volume_mutex);
}

void set_lid_open_volume_level(uint8_t level)
{
    if (level > 100)
    {
        level = 100;
    }
    pthread_mutex_lock(&volume_mutex);
    lid_open_volume_level = level;
    pthread_mutex_unlock(&volume_mutex);
    update_volume_level();
}

void set_lid_level(bool lid_open)
{
    pthread_mutex_lock(&volume_mutex);
    if (lid_open)
    {
        actual_lid_open_volume_level = lid_open_volume_level;
    }
    else
    {
        actual_lid_open_volume_level = 100;
    }
    pthread_mutex_unlock(&volume_mutex);
    update_volume_level();
}

void set_track_volume_level(uint8_t level)
{
    if (level > 100)
    {
        level = 100;
    }
    pthread_mutex_lock(&volume_mutex);
    track_volume_level = level;
    pthread_mutex_unlock(&volume_mutex);
    update_volume_level();
}

void update_volume_level()
{
    pthread_mutex_lock(&volume_mutex);
    actual_master_volume_level = master_volume_level * (track_volume_level / 100) * (actual_lid_open_volume_level / 100);
    pthread_mutex_unlock(&volume_mutex);

    ESP_LOGI(TAG, "Volume updated: Actual-Master=%d, Master=%d, Track=%d, Lid Open=%d", actual_master_volume_level, master_volume_level, track_volume_level, actual_lid_open_volume_level);
}

TaskHandle_t create_play_audio_task(void)
{
    TaskHandle_t handle = NULL;

    BaseType_t res = xTaskCreate(
        play_audio_task,   // task function
        "isr_worker",      // name
        2048,              // stack size
        NULL,              // arg
        5,                 // priority
        &handle            // out handle
    );

    if (res != pdPASS) {
        printf("Timer: failed to create worker task!\n");
        return NULL;
    }

    printf("Timer: created new worker task (%p)\n", (void *)handle);
    return handle;
}

static void play_audio_task(void *arg)
{
    // TODO Add actual audio playback logic here


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
