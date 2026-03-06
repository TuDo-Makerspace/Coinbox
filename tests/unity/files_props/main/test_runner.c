#include "unity.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    UNITY_END();

    while (true) {
        vTaskDelay(portMAX_DELAY);
    }
}
