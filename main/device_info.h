#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

///////////////////////////////////////////////////////////////////////////////////////////////////
// Constants
///////////////////////////////////////////////////////////////////////////////////////////////////

#define DEVICE_INFO_VENDOR "TuDo Makerspace"
#define DEVICE_INFO_SOURCE_CODE_URL "https://github.com/TuDo-Makerspace/Coinbox"
#define DEVICE_INFO_LICENSE_NAME "MIT License"

///////////////////////////////////////////////////////////////////////////////////////////////////
// Interface
///////////////////////////////////////////////////////////////////////////////////////////////////

const char *device_info_firmware_version(void);
const char *device_info_hardware_version(void);
const char *device_info_vendor(void);
const char *device_info_source_code_url(void);
const char *device_info_license_name(void);
esp_err_t device_info_get_mac_string(char *out, size_t out_size);
esp_err_t device_info_get_recovery_code_string(char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
