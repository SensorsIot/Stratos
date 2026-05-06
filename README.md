# Balloon-Receiver

Open-source ESP-IDF firmware for a single-channel **RS41 radiosonde receiver** on the **LILYGO® TTGO LoRa32 V2.1_1.6** (ESP32 + Semtech SX1276, 433 MHz). The receiver exposes decoded telemetry to a phone over Bluetooth Low Energy using the **MySondy Go API v3.0** ASCII protocol, so the existing MySondy Go Android app works without modification. A built-in HTML page over WiFi handles configuration.

> **Status:** v0 — specification drafted, no firmware yet. See [docs/fsd/MySondyGo-IDF FSD.md](docs/fsd/MySondyGo-IDF%20FSD.md).

## Why this exists

The closed-source MySondy Go firmware is feature-appropriate for a portable sonde chaser but proprietary. `dl9rdz/rdz_ttgo_sonde` is open but bundles many features beyond a focused field receiver. This project sits between them: minimal, open, ESP-IDF-native, and protocol-compatible with the MySondy Go app.

## What it does

- Decodes a single Vaisala **RS41** radiosonde on a user-configured 433 MHz frequency.
- Streams telemetry (frequency, signal, name, lat/lon/alt, velocities, AFC, battery) to a phone over BLE — Nordic UART Service carrying MySondy Go v3.0 ASCII frames.
- Serves a web page over WiFi to set frequency, sonde type, MYCALL, WiFi credentials, and BLE on/off.
- Persists configuration in NVS; recovers gracefully from corruption or empty-NVS first boot.
- OTA firmware updates over WiFi.

**Not included (v1):** scanner / auto-search, M10/M20/DFM/PILOT decoders (architectural hooks only), RS92, MRZ, iMet, deep-sleep, SondeHub/APRS/MQTT uploaders.

## Hardware

- **Board**: LILYGO® TTGO LoRa32 V2.1_1.6 (433 MHz variant) — ESP32-PICO-D4, SX1276, on-board SSD1306 OLED, 18650 holder, USB-C.
- **Antenna**: External 433 MHz λ/4 or λ/2 (user-supplied).
- **Power**: USB-C primary; 18650 supported as power source (no power optimisation in v1).

## Building (once firmware lands)

```bash
# ESP-IDF v5.x environment must be active
. $IDF_PATH/export.sh

idf.py set-target esp32
idf.py menuconfig          # optional — captive portal can also provision WiFi
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

First boot with empty NVS → device starts an open WiFi AP `MySondyGo-XXXX` on `192.168.4.1` for provisioning.

## Documentation

- **FSD** (full specification): [docs/fsd/MySondyGo-IDF FSD.md](docs/fsd/MySondyGo-IDF%20FSD.md)
- **MySondy Go API v3.0**: <https://download.farenight.it/MySondyGoAPI_V3.pdf> (external)

## License

GPL-2.0. Required because the decoder is a port of [`rs1729/RS`](https://github.com/rs1729/RS) (GPL-2.0).

## Credits

- **Mirko Dalmonte (IZ4PNN)** — author of MySondy Go and its API; this project is protocol-compatible by emulation.
- **rs1729** — author of the upstream radiosonde decoder reference implementation.
- **DL9RDZ** — author of `rdz_ttgo_sonde`, cross-referenced for hardware quirks.
