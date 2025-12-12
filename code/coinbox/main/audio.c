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
