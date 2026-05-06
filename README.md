# Balloon-Receiver

![License](https://img.shields.io/github/license/SensorsIot/Balloon-Receiver)
![Platform](https://img.shields.io/badge/platform-ESP32-blue)
![Framework](https://img.shields.io/badge/framework-ESP--IDF%20v5.x-red)
![Hardware](https://img.shields.io/badge/board-TTGO%20LoRa32%20V2.1__1.6-orange)
![Status](https://img.shields.io/badge/status-v0%20draft-yellow)

> **Status:** v0 — specification only, no firmware yet. The full functional specification lives at [docs/fsd/Balloon-Receiver FSD.md](docs/fsd/Balloon-Receiver%20FSD.md). When code lands, this README is the starting point for users.

## The Problem

Every day around the world, weather services launch hundreds of small radiosondes — single-use sensor packages dangling under helium balloons. Each one transmits its GPS position on 433 MHz as it ascends, drifts, bursts, and falls. Hobbyists who chase and recover these sondes have two choices for a portable receiver:

- **Closed-source apps** like *MySondy Go* — work great, pair with a cheap ESP32 dev board over Bluetooth, but the firmware is proprietary.
- **Open firmware** like `rdz_ttgo_sonde` — fully open, but bundles many features beyond a focused, pocketable field receiver.

There is no minimal, open, modern-toolchain receiver that pairs cleanly with the popular MySondy Go phone app.

## The Solution

Balloon-Receiver is open-source firmware for a ~€20 ESP32 dev board (the LILYGO TTGO LoRa32 V2.1_1.6) that turns it into a portable RS41 radiosonde receiver. It speaks the same Bluetooth protocol the MySondy Go Android app already understands, so the app works against this firmware **without any modification**. A built-in web page handles configuration if you don't want to touch the phone.

Buy the board, flash this firmware, attach a 433 MHz antenna, open the MySondy Go app — done.

## What It Does

- Tunes a single 433 MHz channel and decodes Vaisala **RS41** radiosondes.
- Streams the decoded sonde data — name, latitude, longitude, altitude, horizontal/vertical velocity, signal strength — to a paired phone over Bluetooth Low Energy.
- Speaks the **MySondy Go API v3.0** ASCII protocol, so the official MySondy Go Android app sees it as a regular MySondy Go device.
- Serves a configuration web page on its **own WiFi access point** for setting frequency, callsign, and Bluetooth on/off. The receiver never joins a home network — it is its own AP.
- Saves all settings in flash; recovers gracefully from a wiped or corrupted config.
- Updates its own firmware over WiFi (OTA) with automatic rollback if the new image fails to boot.

**Limitations (v1, by design):**

- One frequency at a time — no scanner or auto-search.
- RS41 sondes only. Hooks exist in the code for M10, M20, DFM, and PILOT, but no decoders are wired up yet.
- No deep-sleep / battery optimisation.
- No outbound uploaders (no SondeHub, APRS-IS, MQTT).

## How It Works

```
   ╭───────────╮
   │  balloon  │           ╭────────────────────────────╮
   │   ╱╲╱╲    │           │  TTGO LoRa32 V2.1_1.6      │
   │  ▽    ◯  RS41         │  ESP32 + SX1276 + OLED     │
   ╰───┊───────╯           │                            │
       │ 433 MHz          ╭┤ SX1276 (FSK RX)            │
       │ ────────────────▶│                            │
                          │└─▶ RS41 decoder (rs1729)   │
                          │     │                      │
                          │     ▼                      │
                          │  state ─┬──▶ BLE NUS ──┐   │
                          │         │   (MySondy   │   │
                          │         │    Go ASCII) │   │
                          │         └──▶ HTTP UI  ─┼─╮ │
                          │             OLED       │ │ │
                          ╰────────────────────────┴─┴─╯
                                                   │ │
                                       ┌───────────┘ │
                                       │             │
                                       ▼             ▼
                                ┌───────────┐  ┌──────────┐
                                │  phone    │  │ browser  │
                                │ (MySondy  │  │ (config) │
                                │  Go app)  │  │          │
                                └───────────┘  └──────────┘
```

The radio chip on the board listens to the sonde's transmission. Software on the ESP32 turns the bits into GPS coordinates. Those coordinates fly out over Bluetooth in a format the existing MySondy Go phone app already understands. The OLED on the board shows the same data locally; a web page on the same WiFi network is there for configuration.

## Hardware

| Item | Notes |
|---|---|
| **LILYGO TTGO LoRa32 V2.1_1.6** | 433 MHz variant. Identifiable by the silkscreen `LoRa32 V2.1 1.6`. ~€20 from LilyGO direct or major resellers. The board includes the ESP32, SX1276 radio, OLED, USB-C, and 18650 battery holder. |
| **433 MHz antenna** | λ/4 (~17 cm whip) or λ/2, SMA connector. Any cheap dipole or whip works for nearby sondes; a tuned antenna helps at distance. |
| **USB-C cable** | For flashing and primary power. |
| **18650 Li-Ion cell** *(optional)* | Slots into the on-board holder for portable use. |

## Building & Flashing

Requires **ESP-IDF v5.1** or newer. Set up per [Espressif's getting started guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/).

```bash
# Activate ESP-IDF environment
. $IDF_PATH/export.sh

# Build and flash
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

On power-up the device opens an open WiFi access point named `MySondyGo-XXXX` (last 4 hex of its MAC). Connect a phone or laptop, browse to **http://192.168.4.1** (the captive portal redirects there automatically), and set the listen frequency, sonde type, and battery calibration. Save → done. The receiver does not join any other WiFi network.

## Configuration

After provisioning, configuration is reachable two ways:

- **Web page** at `http://192.168.4.1` after joining the device's AP from your phone.
- **MySondy Go Android app** — connect via Bluetooth and use the app's frequency/sonde-type controls. The device honours the API v3.0 commands documented at <https://download.farenight.it/MySondyGoAPI_V3.pdf>.

Long-press the on-board button (≥ 5 s) to factory-reset and re-enter provisioning mode.

## Files

```
.
├── CMakeLists.txt              top-level ESP-IDF project
├── sdkconfig.defaults          NimBLE on, two-OTA partition, 60s WDT
├── main/
│   ├── app_main.c              boot stub (Phase 1 placeholder)
│   ├── CMakeLists.txt
│   └── idf_component.yml       requires IDF >= 5.1
├── docs/fsd/
│   └── Balloon-Receiver FSD.md the canonical specification
├── LICENSE                     GPL-2.0
└── README.md
```

The full component layout (RF driver, decoder, BLE codec, web UI, NVS, OTA, OLED, button, watchdog) is described in [§2.3 of the FSD](docs/fsd/Balloon-Receiver%20FSD.md).

## Under the Hood

| Concern | Choice |
|---|---|
| RTOS / framework | **ESP-IDF** (FreeRTOS), latest stable. No Arduino-core compatibility shim. |
| Radio | **Semtech SX1276** in raw FSK continuous-RX mode (not LoRa). Custom SPI driver, sync-word matching, FIFO drain on `DIO0` interrupt. |
| Decoder | Algorithms ported from **[`rs1729/RS`](https://github.com/rs1729/RS)** (GPL-2.0) — the de-facto reference for radiosonde decoding. |
| Bluetooth | **NimBLE** host stack (built into ESP-IDF). Exposes the Nordic UART Service (NUS); MySondy Go ASCII protocol rides on top of it. |
| Web | ESP-IDF's `esp_http_server`. Single-page HTML embedded in firmware via `EMBED_TXTFILES` — no SPIFFS / LittleFS partition. |
| Persistence | ESP-IDF **NVS** (`nvs_flash`). |
| Updates | ESP-IDF `esp_https_ota` + dual A/B partitions, automatic rollback on bad firmware. |

## References

- **MySondy Go API v3.0** — protocol the device emulates: <https://download.farenight.it/MySondyGoAPI_V3.pdf>
- **rs1729/RS** — upstream decoder reference: <https://github.com/rs1729/RS>
- **dl9rdz/rdz_ttgo_sonde** — sister project, cross-referenced for hardware quirks (not a code dependency): <https://github.com/dl9rdz/rdz_ttgo_sonde>
- **SondeHub** — global sonde tracking network: <https://sondehub.org>

## Credits

- **Mirko Dalmonte (IZ4PNN)** — author of MySondy Go and its API. This project is protocol-compatible with his work by emulation.
- **rs1729** — author of the upstream radiosonde decoder reference implementation. The decoder algorithms here are derived from his code.
- **DL9RDZ** — author of `rdz_ttgo_sonde`, cross-referenced for hardware-level details.

## License

GPL-2.0 — required because the decoder is a port of `rs1729/RS` (GPL-2.0). See [LICENSE](LICENSE).
