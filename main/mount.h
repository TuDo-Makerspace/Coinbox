#include "esp_err.h"

#ifndef MOUNT_H
#define MOUNT_H

esp_err_t mount_storage(const char* base_path);
esp_err_t format_storage(const char* base_path);

#endif // MOUNT_H
