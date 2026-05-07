# Stratos

ESP-IDF firmware that turns a LILYGO TTGO LoRa32 V2.1_1.6 (T3 V1.6) into a portable weather-balloon (radiosonde) receiver. BLE companion to [BalloonHunter](https://github.com/SensorsIot/BalloonHunter); BLE wire format is MySondyGo API v3.0–compatible so other MySondyGo-aware apps also connect.

## Tech Stack

- **Framework**: ESP-IDF (latest, `release/v5.5`)
- **Language**: C / C++
- **Toolchain**: CMake + Ninja, `idf.py` wrapper
- **Container**: Debian devcontainer with ESP-IDF at `/opt/esp-idf`, tools at `/opt/esp-idf-tools`

## Devcontainer

- Workspace: `/workspaces/Balloon-Receiver` *(directory rename to `/workspaces/Stratos` pending devcontainer rebuild)*
- SSH port (host): `2634` → container `2222`
- USB passthrough: `/dev` is bind-mounted (privileged) for ESP32 flashing
- ESP-IDF auto-sourced in `~/.bashrc`

## Common Commands

```bash
idf.py set-target esp32
idf.py build
idf.py flash monitor
```

## Conventions

- Public-symbol prefix: `st_*` (e.g. `st_oled_init`, `st_config_t`).
- IDF project name: `stratos`. Output binary: `stratos.bin` at `0x20000`.
- Default device name (BLE adv + WiFi SSID): `Stratos-XXXX` (last 4 hex of MAC).

## Related

- [[BalloonHunter]] — companion iOS/Android app: <https://github.com/SensorsIot/BalloonHunter>
- [[kytrack]] — Pi 4 APRS gateway, possible counterpart for balloon tracking
