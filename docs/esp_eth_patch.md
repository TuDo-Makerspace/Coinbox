# Local `esp_eth` Patch for QEMU

This repository contains a local override of ESP-IDF's `esp_eth` component under:

- `components/esp_eth/CMakeLists.txt`
- `components/esp_eth/src/openeth/esp_eth_mac_openeth.c`

The override exists only to patch the QEMU/OpenETH path used by integration tests. It is not intended to change behavior on real hardware.

Some integration tests load pages such as `/sounds/` that read metadata from LittleFS. While LittleFS is reading from flash, the ESP32 temporarily disables the flash cache. During that window, the QEMU OpenETH interrupt handler could fire and hit this path from the upstream driver:

- `OPENETH_INT_BUSY`
- `ESP_EARLY_LOGW(TAG, "%s: RX frame dropped ...")`

That log call is unsafe in this situation because the log tag and format string live in flash. If the interrupt tries to use them while the cache is disabled, the firmware can crash with a panic such as:

- `Cache disabled but cached memory region accessed`

Patch: For `OPENETH_INT_BUSY`, we no longer logs from inside the IRAM ISR.

```diff
 if (status & OPENETH_INT_BUSY) {
-    ESP_EARLY_LOGW(TAG, "%s: RX frame dropped (0x%" PRIx32 ")", __func__, status);
+    // This IRAM ISR can run while LittleFS disables the SPI flash cache.
+    // Avoid flash-backed logging here to prevent cache-disabled panics in QEMU.
 }
```