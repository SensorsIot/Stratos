# Stratos

![License](https://img.shields.io/github/license/SensorsIot/Stratos)
![Platform](https://img.shields.io/badge/platform-ESP32-blue)
![Framework](https://img.shields.io/badge/framework-ESP--IDF%20v5.5-red)
![Hardware](https://img.shields.io/badge/board-LILYGO%20T3%20V1.6-orange)
![Status](https://img.shields.io/badge/status-v0%20draft-yellow)

> **Status:** v0 — under active development. Specification at [docs/fsd/Stratos FSD.md](docs/fsd/Stratos%20FSD.md). Companion app: [BalloonHunter](https://github.com/SensorsIot/BalloonHunter).

## 🎈 The Problem

Every day, weather services launch hundreds of small radiosondes — single-use sensor packages dangling under helium balloons. Each one transmits its GPS position on 433 MHz as it ascends, drifts, bursts, and falls. To find one after it lands, you need a receiver: a box that listens to the right frequency, decodes the position, and feeds it to a phone you can carry into the field.

A good field receiver is small, runs on a battery, configures over a phone, and shows live data — local on its own screen, and remote on the phone's map.

## 🛰 The Solution

Stratos is open-source firmware for a ~€20 LILYGO T3 V1.6 (TTGO LoRa32 V2.1_1.6) board. It turns that board into a self-contained, pocket-sized receiver for Vaisala **RS41** radiosondes and feeds the decoded position to **[BalloonHunter](https://github.com/SensorsIot/BalloonHunter)**, the companion iOS/Android app, over Bluetooth Low Energy.

Buy the board, flash this firmware, attach a 433 MHz antenna, open BalloonHunter — done.

> **Compatibility note.** Stratos's BLE payload format is wire-compatible with the MySondyGo API v3.0 ASCII protocol, so other MySondyGo-aware apps connect without modification. This is a side effect of using a stable, documented protocol — not the project's reason for being.

## ✨ What It Does

- Tunes a single 433 MHz channel and decodes Vaisala **RS41** radiosondes.
- Streams the decoded telemetry — name, latitude, longitude, altitude, horizontal/vertical velocity, signal strength — over Bluetooth Low Energy to BalloonHunter (or any MySondyGo-aware app).
- Shows the same data locally on the on-board OLED.
- Exposes a configuration web page on its own WiFi access point. The receiver never joins a home network — it is its own AP.
- Saves all settings in NVS; recovers gracefully from a wiped or corrupted config.
- Updates its own firmware over WiFi by uploading a `.bin` file from the browser (with automatic rollback if the new image fails to boot).

**Limitations (v1, by design):**

- One frequency at a time — no scanner or auto-search.
- RS41 sondes only. Hooks exist for M10, M20, DFM, PILOT, but no decoders are wired up yet.
- AP-only — no STA mode, no GitHub-pull OTA. Updates are operator-driven from a phone or laptop on the AP.
- No deep-sleep / battery optimisation.
- No outbound uploaders (no SondeHub, APRS-IS, MQTT).

## 🧭 How It Works

```
   ╭───────────╮
   │  balloon  │           ╭────────────────────────────╮
   │   ╱╲╱╲    │           │  LILYGO T3 V1.6            │
   │  ▽    ◯  RS41         │  ESP32 + SX1276 + OLED     │
   ╰───┊───────╯           │                            │
       │ 433 MHz          ╭┤ SX1276 (FSK RX)            │
       │ ────────────────▶│                            │
                          │└─▶ RS41 decoder            │
                          │     │                      │
                          │     ▼                      │
                          │  state ─┬──▶ BLE NUS ──┐   │
                          │         │   (Stratos   │   │
                          │         │    protocol) │   │
                          │         └──▶ HTTP UI  ─┼─╮ │
                          │             OLED       │ │ │
                          ╰────────────────────────┴─┴─╯
                                                   │ │
                                       ┌───────────┘ │
                                       │             │
                                       ▼             ▼
                                ┌───────────┐  ┌──────────┐
                                │   phone   │  │ browser  │
                                │ (Balloon- │  │ (config) │
                                │  Hunter)  │  │          │
                                └───────────┘  └──────────┘
```

The radio chip on the board listens to the sonde's transmission. Software on the ESP32 turns the bits into GPS coordinates. Those coordinates are notified over Bluetooth in the format BalloonHunter expects. The OLED on the board shows the same data locally; a web page on the same WiFi network is there for configuration.

## 🔧 Hardware

| Item | Notes |
|---|---|
| **LILYGO T3 V1.6** (TTGO LoRa32 V2.1_1.6) | 433 MHz variant. Identifiable by the silkscreen `LoRa32 V2.1 1.6`. ~€20 from LilyGO direct or major resellers. The board includes the ESP32, SX1276 radio, 0.96″ SSD1306 OLED, USB-C, and 18650 battery holder. Pinout reference: [LilyGO `utilities.h`](https://github.com/Xinyuan-LilyGO/LilyGo-LoRa-Series/blob/master/examples/ArduinoLoRa/LoRaSender/utilities.h). |
| **433 MHz antenna** | λ/4 (~17 cm whip) or λ/2, SMA connector. Any cheap dipole works for nearby sondes; a tuned antenna helps at distance. |
| **USB-C cable** | For flashing and primary power. |
| **18650 Li-Ion cell** *(optional)* | Slots into the on-board holder for portable use. |

## 🚀 Quick Start

### Prerequisites

- **ESP-IDF v5.5** or newer ([install guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/))
- A serial port to the board (`/dev/ttyUSB0`, `/dev/ttyACM0`, or similar)

### Build & Flash

```bash
. $IDF_PATH/export.sh
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

### First Boot

On power-up the device opens an open WiFi access point named **`Stratos-XXXX`** (last 4 hex of its MAC) on `192.168.4.1`. Connect a phone or laptop, browse to **<http://192.168.4.1>** (the captive portal redirects there automatically), and set the listen frequency, sonde type, and battery calibration. Save → done.

The receiver does not join any other WiFi network.

## ⚙️ Configuration

After provisioning, configuration is reachable two ways:

- **Web page** at `http://192.168.4.1` after joining the device's AP from your phone or laptop.
- **BalloonHunter app** — connect via Bluetooth and use the app's frequency/sonde-type controls. The device honours the API v3.0 commands documented in `docs/fsd/Stratos FSD.md` §6.

Long-press the on-board button (≥ 5 s) to factory-reset and re-enter provisioning mode.

## 🆙 Firmware Updates

OTA is **operator-driven and AP-only**:

1. Power up the receiver and join its AP.
2. Browse to `http://192.168.4.1`.
3. Click **Upload .bin**, pick the new `stratos.bin`, wait for the green checkmark.
4. The device verifies the image, swaps OTA slots, reboots, and rolls back automatically if the new firmware fails to boot.

There is no STA mode, no GitHub-pull update path, and no auto-update.

## 🩺 Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| OLED stays blank, log shows `no SSD1306 ACK` | I²C bus issue (loose pin, wrong board variant) | Check SDA=21, SCL=22 are connected; confirm the board is a T3 V1.6 (this firmware does not support older T3 V1.3 pinout). |
| `rst:0x1 (POWERON_RESET)` reboot loop right after boot | Board profile lists a non-existent OLED reset pin | Confirm `oled_rst` is `GPIO_NUM_NC` in `components/board_profile/src/board_profile.c`. T3 V1.6 has no OLED reset wire. |
| BalloonHunter sees the device but no telemetry | Frequency mis-set; antenna disconnected; no sonde in range | Open the web UI, verify frequency matches a known active sonde launch. RSSI on the OLED status screen confirms signal. |
| Browser upload of `.bin` fails | Image too large, or wrong target | The OTA slot is 1.5 MB. Confirm you're uploading the `stratos.bin` for ESP32 (not a different target). |

## 📂 Files

```
.
├── CMakeLists.txt              top-level ESP-IDF project (project name: stratos)
├── partitions.csv              4 MB layout: nvs, phy_init, otadata, ota_0, ota_1
├── sdkconfig.defaults          NimBLE on, custom partition, 60s task WDT
├── main/
│   ├── app_main.c              boot sequence, component wiring
│   └── CMakeLists.txt
├── components/
│   ├── board_profile/          per-board pin/feature table
│   ├── config_store/           NVS-backed configuration
│   ├── platform_common/        version, watchdog, reboot, heap monitor
│   ├── wifi_manager/           SoftAP setup
│   ├── http_ui/                web UI, REST API, OTA
│   ├── ota/                    OTA receive + verify
│   ├── oled_ui/                SSD1306 driver (i2c_master) + render loop
│   ├── button/                 short/long press handler
│   ├── battery/                ADC calibration, percentage curves
│   ├── ble_nus/                NimBLE host, NUS service
│   ├── stratos_codec/          BLE wire-format codec (MySondyGo v3.0 wire-compatible)
│   ├── rf_sx1276/              FSK RX driver for Semtech SX1276
│   ├── decoder_core/           per-sonde-type decoder vtable
│   ├── decoder_rs41/           Vaisala RS41 frame decoder
│   ├── sonde_state/            decoded-frame state machine
│   └── sonde_types/            shared types and event base
├── docs/fsd/Stratos FSD.md     canonical specification
├── LICENSE                     GPL-2.0
└── README.md
```

## 🛠️ Under the Hood

| Concern | Choice |
|---|---|
| RTOS / framework | **ESP-IDF v5.5** (FreeRTOS). No Arduino-core compatibility shim. |
| Radio | **Semtech SX1276** in raw FSK continuous-RX mode (not LoRa). Custom SPI driver, sync-word matching, FIFO drain on `DIO0` interrupt. |
| Decoder | RS41 algorithms ported from **[`rs1729/RS`](https://github.com/rs1729/RS)** (GPL-2.0) — the de-facto reference for radiosonde decoding. |
| Bluetooth | **NimBLE** host stack (built into ESP-IDF). Exposes the Nordic UART Service (NUS); Stratos's MySondyGo-v3.0-compatible ASCII protocol rides on top. |
| OLED | SSD1306 driver via the IDF `i2c_master` API (page-addressing mode). |
| Web | ESP-IDF `esp_http_server`. Single-page HTML embedded in firmware via `EMBED_TXTFILES` — no SPIFFS / LittleFS partition. |
| Persistence | ESP-IDF **NVS** (`nvs_flash`). |
| Updates | Browser-uploaded `.bin` → `esp_ota_*` + dual A/B partitions, automatic rollback on bad firmware. |

## 🌐 Network & Ports

| Port | Service | Notes |
|---|---|---|
| `192.168.4.1:80` | HTTP UI | Captive portal, REST API, OTA upload |
| `192.168.4.1:53/udp` | Captive DNS | Redirects all DNS lookups to the device |
| BLE NUS | `6E400001-…-…` | RX `…0002` (write), TX `…0003` (notify) |

## 🔌 Wire Protocol

Stratos's BLE payload format is identical to the **MySondyGo API v3.0 ASCII protocol**, so any MySondyGo-aware app (BalloonHunter included) parses the frames natively. The frame types `0/`, `1/`, `2/`, `3/` and inbound `o{...}o` command grammar are documented in [Stratos FSD §6](docs/fsd/Stratos%20FSD.md).

## 🙏 References & Credits

- **[BalloonHunter](https://github.com/SensorsIot/BalloonHunter)** — companion iOS/Android app (Andreas Spiess, HB9BLA).
- **[`rs1729/RS`](https://github.com/rs1729/RS)** — upstream radiosonde decoder reference (GPL-2.0). RS41 decoder algorithms here are derived from this work.
- **[`dl9rdz/rdz_ttgo_sonde`](https://github.com/dl9rdz/rdz_ttgo_sonde)** — sister project, cross-referenced for hardware quirks (not a code dependency).
- **[SondeHub](https://sondehub.org)** — global sonde tracking network.
- **[Mirko Dalmonte (IZ4PNN)](https://download.farenight.it/MySondyGoAPI_V3.pdf)** — author of the MySondyGo API v3.0 specification, which Stratos's wire format implements.

## 📜 License

GPL-2.0 — required because the RS41 decoder is a port of `rs1729/RS` (GPL-2.0). See [LICENSE](LICENSE).
