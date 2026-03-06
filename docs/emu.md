# Coinbox on QEMU (ESP32 + OpenETH)

This document explains what had to change to run Coinbox reliably in QEMU, and why those changes are needed.

## 1) How to launch the project in QEMU

### Prerequisites

- ESP-IDF `v5.5.x` environment installed and exported.
- QEMU Xtensa tool installed:

```bash
python "$IDF_PATH/tools/idf_tools.py" install qemu-xtensa
```

### Launch from terminal

```bash
idf.py -B build_qemu \
  -D SDKCONFIG=sdkconfig.qemu \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.qemu" \
  qemu --qemu-extra-args "-nic user,model=open_eth,hostfwd=tcp::8080-:80" monitor
```

- UI is then reachable at `http://127.0.0.1:8080`.
- QEMU build uses `partitions.csv` (not `partitions_4mb.csv`) via:
  - `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"` in `sdkconfig.defaults` and `sdkconfig.qemu`.

### Launch from VS Code task

- Task: `ESP-IDF: QEMU Monitor (OpenETH)` in [tasks.json](/home/patrick/dev/coinbox/.vscode/tasks.json:5)
- It runs the same `idf.py ... qemu ... monitor` flow and OpenETH host forwarding.

## 2) Network stack changes for OpenETH

### QEMU profile config

The QEMU profile disables Wi-Fi and enables OpenETH:

- [sdkconfig.defaults.qemu](/home/patrick/dev/coinbox/sdkconfig.defaults.qemu:1)
  - `CONFIG_NETWORK_WIFI_AP=n`
  - `CONFIG_NETWORK_WIFI_STA=n`
  - `CONFIG_NETWORK_ETH_OPENETH=y`
  - `CONFIG_FREERTOS_UNICORE=y`
- [sdkconfig.qemu](/home/patrick/dev/coinbox/sdkconfig.qemu:443), [sdkconfig.qemu](/home/patrick/dev/coinbox/sdkconfig.qemu:932), [sdkconfig.qemu](/home/patrick/dev/coinbox/sdkconfig.qemu:1424)
  - `CONFIG_NETWORK_ETH_OPENETH=y`
  - `CONFIG_ETH_USE_OPENETH=y`
  - `CONFIG_FREERTOS_UNICORE=y`

### OpenETH initialization in firmware

OpenETH is initialized in [network.c](/home/patrick/dev/coinbox/main/network.c:756):

- `esp_eth_mac_new_openeth(...)` creates the QEMU OpenCores MAC.
- `esp_eth_phy_new_dp83848(...)` creates the PHY object expected by the ETH stack.
- `esp_eth_driver_install(...)`, `esp_netif_attach(...)`, and `esp_eth_start(...)` bring link up.
- `init_wifi()` routes through OpenETH path when `CONFIG_NETWORK_ETH_OPENETH=y`:
  - [network.c](/home/patrick/dev/coinbox/main/network.c:995)

### Why this was needed

QEMU does not provide a practical ESP32 Wi-Fi simulation for this app. OpenETH gives a stable virtual Ethernet interface, and `hostfwd=tcp::8080-:80` exposes the device HTTP UI to the host.

## 3) Reboot changes required for QEMU stability

### What changed

For OpenETH builds only, restart now uses `esp_restart_noos_dig()`:

- Settings/UI restart path:
  - [mainapp.c](/home/patrick/dev/coinbox/main/mainapp.c:1556)
- OTA reboot path:
  - [ota.c](/home/patrick/dev/coinbox/main/ota.c:266)

Non-OpenETH builds still use regular `esp_restart()`.

### Why this was needed

With QEMU + OpenETH, plain software restart (`esp_restart()`) could leave emulator/peripheral state in a bad condition. That showed up as reboot-time panics (`InstrFetchProhibited` / invalid PC) and reboot loops.

`esp_restart_noos_dig()` performs a lower-level reset path that avoids that stale-state behavior in the emulator.

Additionally, QEMU is configured as unicore (`CONFIG_FREERTOS_UNICORE=y`) to reduce SMP timing/race issues during reset and early boot.

## 4) Quick validation checklist

After starting QEMU:

1. Confirm app boots and HTTP server starts.
2. Open `http://127.0.0.1:8080`.
3. Trigger restart from Settings.
4. Confirm reboot completes and app returns without panic loop.
