# Balloon-Receiver — Functional Specification Document (FSD)

**Repository:** [SensorsIot/Balloon-Receiver](https://github.com/SensorsIot/Balloon-Receiver)
**Status:** Draft v0.1 (initial generation, 2026-05-06)
**License:** GPL-2.0 (firmware contains derivative work of `rs1729/RS`)
**Target hardware:** LILYGO® TTGO LoRa32 V2.1_1.6 (ESP32 + Semtech SX1276, 433 MHz, on-board SSD1306 OLED)
**Build system:** Espressif ESP-IDF (latest stable)

---

## 1. System Overview

### 1.1 Purpose

Balloon-Receiver is a single-board, fixed-frequency radiosonde receiver. It demodulates and decodes a single Vaisala RS41 radiosonde transmission from a configured 433 MHz channel, and exposes the decoded telemetry to a smartphone via Bluetooth Low Energy using the MySondy Go API v3.0 ASCII protocol. A built-in web page allows the operator to configure the listen frequency and other settings over WiFi.

### 1.2 Problem Statement

Existing open-source ESP32 radiosonde receivers (e.g. `dl9rdz/rdz_ttgo_sonde`) bundle many features (multi-band scanning, KISS TNC, AXUDP, MQTT, SondeHub, Chasemapper, multiple display drivers) that are unnecessary for a focused field-receiver use case. The closed-source MySondy Go firmware is feature-appropriate but proprietary, and does not expose source code. There is no minimal, open, ESP-IDF-native radiosonde receiver that pairs cleanly with the existing MySondy Go Android app. This project fills that gap.

### 1.3 Users / Stakeholders

| Stakeholder | Role |
|---|---|
| **Sonde chaser** (primary user) | Carries the device into the field, pairs phone via the MySondy Go Android app, follows the sonde to landing. |
| **Hobby operator / contributor** | Builds, flashes, and modifies the firmware. Adds future decoders. |
| **Mirko Dalmonte (IZ4PNN)** | Author of the MySondy Go Android app and API. Compatibility target — not a project participant. |
| **rs1729** | Author of the upstream decoder reference implementation. Compatibility target via algorithm reuse. |

### 1.4 Goals & Non-Goals

**Goals (v1):**
- Receive and decode a single RS41 radiosonde on a user-configured 433 MHz frequency.
- Expose decoded telemetry to the MySondy Go Android app over BLE without modifications to the app.
- Allow frequency and sonde type to be configured via a built-in web page served on the device's own WiFi access point. **No home WiFi network is involved.**
- Persist configuration across power cycles and survive corruption gracefully.
- Run reliably on USB power for at least 24 h without manual intervention.
- Support firmware updates from GitHub via the operator's smartphone (browser-mediated upload over the AP).

**Non-Goals (v1):**
- Multi-channel frequency scanning or auto-search.
- Decoders for RS92, MRZ, iMet, Meisei, or LMS6.
- M10 / M20 / DFM / PILOT decoders (architectural hooks only — see §2.3).
- Battery-life optimisation, deep-sleep modes.
- Outbound uploaders (SondeHub, APRS-IS, MQTT).
- TFT display variants — SSD1306 OLED only.
- A new mobile app — the existing MySondy Go app is the visualisation surface.
- USB-CDC serial transport for the MySondy Go API (BLE only in v1).
- **WiFi STA / home network connectivity** — the device is AP-only. It never joins a foreign WiFi network and stores no WiFi credentials.
- **Receiver-initiated OTA** (no `esp_https_ota`, no embedded CA bundle, no GitHub fetch from device). All OTA goes through the operator's phone browser.

### 1.5 High-Level System Flow

```
                    ┌─────────────────────────────────────────────┐
                    │  TTGO LoRa32 v2.1                           │
                    │                                             │
[RF @ 433 MHz] ───▶ │  SX1276 (FSK, sync-match) ──┐               │
                    │                              ▼               │
                    │                     ┌────────────────┐       │
                    │                     │ Bit/Byte queue │       │
                    │                     └────┬───────────┘       │
                    │                          ▼                   │
                    │                  ┌──────────────┐            │
                    │                  │ RS41 decoder │            │
                    │                  │ (rs1729 port)│            │
                    │                  └──────┬───────┘            │
                    │                         ▼                    │
                    │                ┌─────────────────┐           │
                    │                │ Sonde state ctx │           │
                    │                │ (lock, GPS, ...)│           │
                    │                └────────┬────────┘           │
                    │            ┌────────────┼────────────┐       │
                    │            ▼            ▼            ▼       │
                    │   ┌───────────────┐ ┌─────────┐ ┌───────┐    │
                    │   │ MySondyGo BLE │ │ HTTP UI │ │ OLED  │    │
                    │   │ codec (NUS)   │ │ (httpd) │ │ (I²C) │    │
                    │   └───────┬───────┘ └────┬────┘ └───────┘    │
                    └───────────┼──────────────┼────────────────────┘
                                ▼              ▼
                         [Phone — MySondy   [Browser
                          Go Android app]    config UI]
```

---

## 2. System Architecture

### 2.1 Logical Architecture

The firmware is decomposed into the following logical subsystems:

| Subsystem | Responsibility |
|---|---|
| **RF Driver** | Configures and operates the SX1276 in continuous FSK RX mode; reads the FIFO on `DIO0` interrupt; produces a byte stream. |
| **Decoder Manager** | Selects the active decoder (RS41 in v1) based on configured `sonde_type`. Provides architectural hooks for M10, M20, DFM, PILOT to be added later. |
| **RS41 Decoder** | Ported from `rs1729/RS`. Consumes byte stream, performs Reed-Solomon, extracts GPS, baromertic, and identifying fields. Outputs `sonde_frame_t` events. |
| **Sonde State Manager** | Maintains the current state (`NO_SIGNAL` / `TRACKING` / `NAME_ONLY`) and the most recent decoded frame. Drives status broadcasts. |
| **MySondy Go Codec** | Serialises state into `0/.../o`, `1/.../o`, `2/.../o`, `3/.../o` ASCII frames per API v3.0. Parses inbound `o{...}o` commands. |
| **BLE Server** | Exposes the Nordic UART Service (NUS). Routes inbound writes to the codec and outbound notifications from the codec. |
| **HTTP Server** | Serves the embedded HTML config page and JSON API endpoints. Backed by `esp_http_server`. |
| **WiFi Manager** | Brings up an open WiFi access point at boot. AP only — never joins a foreign network. |
| **Config Store** | NVS-backed persistence layer for frequency, sonde type, MYCALL, board, BLE on/off, battery calibration. |
| **OLED UI** | Renders status (frequency, sonde type, signal, lock state, WiFi/BLE icons) on the SSD1306. |
| **Button Handler** | Detects short press (OLED on/off) and long press (factory reset). |
| **OTA Manager** | Receives `firmware.bin` from `POST /api/ota/upload`, streams it into the inactive OTA partition, validates, reboots. No outbound HTTPS. |
| **Watchdog & Logger** | Software task watchdog, hardware WDT, and structured logging via `ESP_LOG` (serial) plus optional UDP sink. |

### 2.2 Hardware / Platform Architecture

**Target board:** LILYGO® TTGO LoRa32 V2.1_1.6 (433 MHz variant). Identified by the silkscreen marking `LoRa32 V2.1 1.6` near the USB-C connector. This board includes an on-board SSD1306 0.96" OLED, SMA antenna connector, USB-C, and on-board 18650 battery holder.

| Component | Detail | Connection |
|---|---|---|
| MCU | ESP32-PICO-D4 (16 MB or 4 MB variants in the wild — assume 4 MB minimum) | — |
| Flash | 4 MB SPI flash (8 MB / 16 MB on some sub-revisions) | Internal SPI |
| RF transceiver | Semtech SX1276 (LoRa-capable; operated here in raw FSK mode) | VSPI: SCK=GPIO5, MISO=GPIO19, MOSI=GPIO27, NSS=GPIO18, RST=GPIO23, DIO0=GPIO26, DIO1=GPIO33, DIO2=GPIO32 |
| OLED (on-board) | SSD1306 128×64, I²C address 0x3C | SDA=GPIO21, SCL=GPIO22, RST=GPIO16 |
| User LED | On-board blue/green LED | GPIO25 |
| User button | "PRG" boot button (also user input) | GPIO0 (debounced in software) |
| Battery sense | 18650 voltage divider on board | GPIO35 (ADC1_CH7), divider ratio 2:1 |
| USB-UART | CP2104 / CH9102 (varies by sub-revision) | USB Type-C, autoreset wired to EN / IO0 |
| Antenna | External via on-board SMA | 433 MHz λ/4 or λ/2 whip (user-supplied) |

**Power:** 5 V via USB Type-C (primary). Optional 18650 Li-Ion via on-board JST-PH (battery operation supported as a power source; battery-life optimisation is out of scope for v1).

**RF antenna:** External 433 MHz λ/4 or λ/2 whip via U.FL or SMA (depending on board sub-revision). Antenna selection and matching are out of scope of this firmware; the user supplies an antenna appropriate for their environment.

**Regulatory:** The firmware is **receive-only**. The SX1276 is never commanded into a transmit state. No transmit antenna characteristics, EIRP, or regulatory licensing apply.

### 2.3 Software Architecture

**RTOS:** FreeRTOS (bundled with ESP-IDF).
**ESP-IDF version:** Latest stable at time of build; pinned in `idf_component.yml`.
**Language:** C (C11). C++ permitted only for component glue if a compelling reason exists — the rs1729 decoders are C and stay C.

**Component layout (proposed):**

```
components/
├── rf_sx1276/         SPI driver, FSK config, sync-match RX, FIFO consumer
├── decoder_core/      Decoder Manager + sonde_frame_t schema + dispatch table
├── decoder_rs41/      Port of rs1729 RS41 decoder
├── decoder_stub_m10/  Empty hook (returns "not implemented")
├── decoder_stub_m20/  Empty hook
├── decoder_stub_dfm/  Empty hook
├── sonde_state/       State machine, last-frame cache, event publication
├── mysondygo_codec/   ASCII codec for API v3.0 (in/out)
├── ble_nus/           NimBLE server exposing Nordic UART Service
├── http_ui/           esp_http_server: GET /, /api/state, /api/config, POST /api/config, POST /api/ota
├── wifi_manager/      AP-only WiFi + captive-portal redirect
├── config_store/      NVS schema + accessors
├── oled_ui/           SSD1306 renderer
├── button/            Short-press / long-press detection
├── ota/               POST /api/ota/upload handler, A/B partition writer, version reporting
└── platform_common/   Logging, watchdog, build-version embedding
main/
└── app_main.c         Component init, task wiring
```

**Tasks (FreeRTOS):**

| Task | Stack | Priority | Period | Purpose |
|---|---|---|---|---|
| `rf_isr_drain` | 4 KB | 18 | event-driven | Drain SX1276 FIFO on DIO0 interrupt; push to `byte_queue`. |
| `decoder_rs41` | 8 KB | 10 | streaming | Consume `byte_queue`, run decoder, publish `sonde_frame_t` to event loop. |
| `state_manager` | 4 KB | 8 | event-driven | Update state machine on each frame or 1-Hz tick. |
| `ble_emit` | 4 KB | 7 | 1 Hz | Serialise current state to MySondyGo frame, notify NUS TX. |
| `http_server` | 8 KB | 5 | request-driven | `esp_http_server` worker. |
| `wifi_manager` | 4 KB | 6 | event-driven | Maintain AP, log client connect/disconnect events. |
| `oled_ui` | 4 KB | 4 | 4 Hz | Refresh OLED. |
| `button` | 2 KB | 4 | 50 Hz poll | Debounce + long-press detection. |
| `watchdog` | 2 KB | 3 | 1 Hz | Feed task WDT, monitor heap, log health. |

All inter-task communication uses FreeRTOS queues and the ESP-IDF event loop (`esp_event`). No shared mutable state outside of explicitly-protected accessors.

**Boot sequence:**

1. `app_main` initialises NVS, reads config (defaults if empty/corrupt).
2. WiFi manager starts an open AP `MySondyGo-XXXX` on `192.168.4.1`.
3. HTTP server starts on the AP interface.
4. BLE stack initialises (NimBLE host + NUS service); advertising begins if `ble_on==true`.
5. SX1276 driver initialises, configures FSK RX, sync word for active sonde type, frequency, RX BW.
6. RS41 decoder task starts consuming `byte_queue`.
7. State enters `NO_SIGNAL`; periodic frame `0` notifications begin.
8. OLED, button, and watchdog tasks start last.

Total cold boot to "device usable" target: ≤ 5 s.

**Persistence:** ESP-IDF NVS (`nvs_flash`). Schema in §6.3. Defaults applied if any field is absent. Factory reset erases the entire `storage` namespace.

**Update model:** Browser-uploaded OTA via the device's AP. The operator's phone uses cellular to fetch the latest release `.bin` from GitHub, then uploads it to the receiver via `POST /api/ota/upload` over the AP. Dual A/B OTA partitions with rollback on boot failure (built-in ESP-IDF behaviour). The receiver itself never makes outbound HTTPS requests, so no CA bundle is needed. Firmware version (semver `MAJOR.MINOR.PATCH` + git short SHA) is compiled into the image and exposed via `VER` in BLE frames and `/api/version`.

---

## 3. Implementation Phases

### 3.1 Phase 1 — Hardware bring-up & infrastructure

**Scope:**
- ESP-IDF skeleton, component layout, build green.
- NVS schema and accessors.
- WiFi manager — open AP only at `192.168.4.1`, captive-portal redirect.
- HTTP server with placeholder `/` and JSON endpoints returning stub data.
- OLED driver showing boot banner and connection status.
- Button handler with short/long-press detection wired to GPIO0.
- Logging infrastructure (`ESP_LOG` + optional UDP sink).
- Software task watchdog + heap monitor.

**Deliverables:**
- Firmware that boots, joins WiFi (or hosts AP), serves a placeholder web page, shows status on OLED, accepts button input.
- All embedded test library hooks for WiFi, NVS, captive portal, watchdog, logging passing.

**Exit criteria:**
- TC-NVS-100, TC-NVS-101, TC-CP-100, TC-WDT-100, TC-LOG-100 all pass on real hardware.
- Cold boot to AP reachable and web UI loaded in ≤ 5 s.

**Dependencies:** None (greenfield).

### 3.2 Phase 2 — RF receiver

**Scope:**
- SX1276 SPI driver: register I/O, mode transitions, FIFO read.
- Continuous FSK RX with configurable centre frequency, bitrate, frequency deviation, RX bandwidth.
- Sync-word match → FIFO threshold → `DIO0` interrupt → byte queue.
- Test harness that dumps raw bytes to serial when sync-matched.

**Deliverables:**
- `rf_sx1276` component with public API: `rf_init()`, `rf_set_freq_hz()`, `rf_set_profile(rf_profile_t*)`, `rf_start_rx()`, `rf_stop()`, byte-queue handle.
- Bench-recorded log of RS41 sync detection at 432.500 MHz against a real or replayed sonde signal.

**Exit criteria:**
- Sync match event observed on a known RS41 capture.
- Byte queue carries non-zero traffic when RS41 signal present and is silent when not.
- No RF mis-configuration triggers transmit (verified by RX-only register audit).

**Dependencies:** Phase 1.

### 3.3 Phase 3 — RS41 decoder

**Scope:**
- Port rs1729's RS41 decoder into the `decoder_rs41` component.
- Adapt I/O: replace `stdin` / `fread` byte source with the byte queue from Phase 2.
- Replace stdout printing with structured `sonde_frame_t` event publication.
- Reed-Solomon, frame CRC, GPS extraction, AFC measurement, identifier (serial) extraction.
- State machine: `NO_SIGNAL` ↔ `NAME_ONLY` ↔ `TRACKING`.
- OLED renderer showing decoded NAME, frequency, RSSI, lock state.

**Deliverables:**
- A live decode session with a real or replayed RS41 producing valid GPS coordinates.
- `sonde_frame_t` schema documented and stable.

**Exit criteria:**
- Decoder produces GPS lat/lon/alt within 50 m of ground truth on a known recording.
- Lock-acquisition time ≤ 30 s from cold start with strong signal.
- Decoder runs continuously for 1 h without crash, watchdog, or memory leak.

**Dependencies:** Phase 2.

### 3.4 Phase 4 — MySondy Go BLE protocol

**Scope:**
- NimBLE NUS service with confirmed UUIDs (see §5 risk on UUID confirmation).
- Outbound: serialise current state to `0/.../o`, `1/.../o`, `2/.../o`, `3/.../o` frames.
- Inbound: parse `o{...}o` envelopes, slash-separated key=value pairs.
- Honour: `f`, `tipo`, `?`, `re`, `Re`, `mute`, `mycall`, `aprsName`, `freqofs`, `*.rxbw`.
- Accept-and-ignore (with debug log): all pin-config commands (`oled_sda`, `oled_scl`, `oled_rst`, `led_pout`, `buz_pin`, `battery`, `lcd`, `lcdOn`, `com`).
- `tipo=4` (PILOT) accepted and stored, but `state` remains `NO_SIGNAL` because no decoder runs.
- `sleep` command logged and treated as a no-op in v1 (see §5 assumption).
- Live panel in HTML UI mirrors the decoded state in real time.

**Deliverables:**
- MySondy Go Android app connects to the device, displays decoded sonde data without modification.
- Frequency and sonde-type changes from the app take effect on the device.

**Exit criteria:**
- TC-BLE-100, TC-BLE-101, TC-BLE-103 pass.
- Round-trip test on Android 11+ with the official MySondy Go app: frame parsing, command dispatch, reconnection after BLE disconnect.
- Web UI live panel updates within 2 s of a decoded frame.

**Dependencies:** Phase 3.

### 3.5 Phase 5 — OTA, hardening, V&V

**Scope:**
- Browser-uploaded OTA: `POST /api/ota/upload`, A/B partition writer.
- Version embedding (build-time `MAJOR.MINOR.PATCH+SHA`).
- Web UI "Update from GitHub" panel querying `api.github.com` from the browser.
- Boot rollback on bad firmware (validated by intentional bad image).
- Watchdog tuning to not false-trigger during OTA.
- Full V&V campaign — every Must / Should requirement covered (see §8.4).

**Deliverables:**
- Stable v1.0.0 release candidate.
- Test report against this FSD.

**Exit criteria:**
- All Must FRs covered by passing tests.
- 24 h continuous-operation soak test with concurrent BLE + WiFi + active decoding.
- TC-OTA-100, TC-OTA-101, TC-OTA-102 pass.

**Dependencies:** Phases 1–4.

---

## 4. Functional Requirements

### 4.1 Functional Requirements (FR)

#### Group 1 — RF receiver

- **FR-1.1** [Must]: The device shall configure the SX1276 in continuous FSK RX mode at the user-configured frequency.
- **FR-1.2** [Must]: The device shall apply a per-sonde-type RF profile (bitrate, frequency deviation, RX bandwidth, sync word) when the active sonde type changes.
- **FR-1.3** [Must]: The device shall support frequencies in the range 137.200 MHz to 524.800 MHz (the MySondy Go API range), and shall reject out-of-range values.
- **FR-1.4** [Should]: The device shall report RX RSSI in dBm with each decoded frame.
- **FR-1.5** [Must]: The device shall never command the SX1276 into a transmit state under any operational path.

#### Group 2 — RS41 decoder

- **FR-2.1** [Must]: The device shall decode Vaisala RS41-SG / RS41-SGP / RS41-SGM radiosondes.
- **FR-2.2** [Must]: The decoder shall extract the radiosonde serial number, latitude, longitude, altitude, horizontal velocity, and vertical velocity from each valid frame.
- **FR-2.3** [Must]: The decoder shall apply Reed-Solomon error correction as defined in the RS41 frame format.
- **FR-2.4** [Should]: The decoder shall report Automatic Frequency Correction (AFC) offset in Hz.
- **FR-2.5** [Should]: The decoder shall report burst-killer status (`BK`) and remaining timer (`BKTIME`) when present.
- **FR-2.6** [May]: The codebase shall expose a decoder dispatch table that allows adding M10, M20, DFM, and PILOT decoders without modifying the state manager or BLE codec.

#### Group 3 — State machine

- **FR-3.1** [Must]: The device shall maintain one of three states: `NO_SIGNAL`, `NAME_ONLY`, `TRACKING`.
- **FR-3.2** [Must]: The device shall transition to `NO_SIGNAL` after no valid frame has been received for 30 s.
- **FR-3.3** [Must]: The device shall transition to `TRACKING` upon receipt of a valid frame containing a GPS fix.
- **FR-3.4** [Should]: The device shall transition to `NAME_ONLY` upon receipt of a valid frame lacking a GPS fix.

#### Group 4 — MySondy Go BLE output

- **FR-4.1** [Must]: The device shall emit BLE notifications encoded per MySondy Go API v3.0 §OUTPUT.
- **FR-4.2** [Must]: When in `NO_SIGNAL`, the device shall emit `0/TYPE/FREQ/SIGN/BAT%/BATV/BUZMUTE/VER/o` at ≥ 0.5 Hz (heartbeat).
- **FR-4.3** [Must]: When in `TRACKING`, the device shall emit `1/TYPE/FREQ/NAME/LAT/LON/ALT/HVEL/VVEL/SIGN/BAT%/AFC/BK/BKTIME/BATV/BUZMUTE/RES/RES/RES/VER/o` for each decoded frame (typically ~1 Hz).
- **FR-4.4** [Should]: When in `NAME_ONLY`, the device shall emit `2/TYPE/FREQ/NAME/SIGN/BAT%/AFC/BATV/BUZMUTE/VER/o` per decoded frame.
- **FR-4.5** [Must]: The device shall emit a `3/...` settings frame in response to `o{?}o`.
- **FR-4.6** [Must]: The `BUZMUTE` field shall always be reported as `-1` (not installed) in v1, since no buzzer is connected.

#### Group 5 — MySondy Go BLE input

- **FR-5.1** [Must]: The device shall parse inbound writes wrapped in `o{...}o` and silently ignore writes lacking those delimiters.
- **FR-5.2** [Must]: The device shall accept the command `f=<MHz>` and update the active frequency, persisting to NVS.
- **FR-5.3** [Must]: The device shall accept `tipo=<1|2|3|4|5>` and update the active sonde type, persisting to NVS. `tipo=1` (RS41) is fully decoded; `tipo=2..5` are accepted and stored but result in `NO_SIGNAL` until a decoder is implemented.
- **FR-5.4** [Must]: The device shall accept `?` (request settings) and reply with a `3/...` frame.
- **FR-5.5** [Must]: The device shall accept `re` (reboot) and `Re` (reset to defaults + reboot).
- **FR-5.6** [Should]: The device shall accept `mute=<0|1>` and store the value (no audible effect since no buzzer in v1).
- **FR-5.7** [Should]: The device shall accept `myCall=<text>` (max 8 chars), `aprsName=<0|1>`, `freqofs=<Hz>`.
- **FR-5.8** [Should]: The device shall accept `rs41.rxbw=<idx>` etc. and apply the corresponding RX bandwidth from MySondy Go API Appendix 2 to the active radio profile.
- **FR-5.9** [May]: The device shall accept all pin-config and serial-config commands (`oled_sda`, `oled_scl`, `oled_rst`, `led_pout`, `buz_pin`, `battery`, `lcd`, `lcdOn`, `com`, `baud`) and acknowledge them by emitting an updated `3/...` frame, but shall not change runtime hardware behaviour.
- **FR-5.11** [Should]: The device shall honour `vBatMin=<mV>`, `vBatMax=<mV>`, `vBatType=<0|1|2>` commands by persisting them to NVS and using them for the next battery percentage calculation. These are real configuration, not pin-config.
- **FR-5.10** [May]: The device shall accept `sleep=<n>` and respond by logging the command; no deep-sleep transition occurs in v1 (see §5).

#### Group 5b — Battery measurement

- **FR-5b.1** [Must]: The device shall sample battery voltage on `GPIO35` (ADC1_CH7) using the ESP32 ADC with 11 dB attenuation and apply the 2:1 on-board divider ratio so `Vbatt_mV = Vadc_mV × 2`.
- **FR-5b.2** [Should]: The device shall use ESP-IDF eFuse-based ADC calibration (`esp_adc_cal`) to compensate for per-chip ADC offset and slope.
- **FR-5b.3** [Should]: The device shall low-pass-filter the raw ADC reading (e.g. 16-sample moving average at 1 Hz) to suppress noise from concurrent radio activity.
- **FR-5b.4** [Must]: The device shall report `BATV` (millivolts, integer) and `BAT%` (0..100, integer) in BLE frames `0`, `1`, and `2`. When no battery is detected (`Vbatt_mV < 2.0 V`), `BAT%` shall be `-1` and `BATV` may be reported as the raw measurement or `0`.
- **FR-5b.5** [Must]: The `BAT%` calculation shall apply the curve selected by `vBatType` between `vBatMin` and `vBatMax`:
  - `0` Linear: `pct = clamp((Vbatt − vBatMin) / (vBatMax − vBatMin), 0, 1) × 100`
  - `1` Sigmoidal (default): smoother S-curve approximating Li-Ion discharge.
  - `2` Anti-sigmoidal: inverse curvature (steep at extremes).
- **FR-5b.6** [Should]: Default values shall be `vBatMin = 2950 mV`, `vBatMax = 4200 mV`, `vBatType = 1` (sigmoidal). Note that `vBatMax = 4200` deviates from MySondy Go API's default of `4180`; this matches typical 18650 fresh-off-the-charger voltage.

#### Group 6 — Web UI / HTTP API

- **FR-6.1** [Must]: The device shall serve a single-page HTML configuration UI at `GET /`.
- **FR-6.2** [Must]: The UI shall provide form fields for: **board model** (dropdown — see Group 14), frequency (MHz), sonde type (dropdown), MYCALL, BLE on/off, **battery calibration** — `vBatMax` (mV, default 4200), `vBatMin` (mV, default 2950), `vBatType` (Linear / Sigmoidal / Anti-sigmoidal, default Sigmoidal). The UI shall display the current measured `BATV` next to the `vBatMax` field as a one-click "set to current full-charge reading" calibration helper. **No WiFi credential fields** are present (the device is AP-only — see FR-7.x).
- **FR-6.2a** [Must]: The UI shall **not** expose individual GPIO pins for OLED, LED, buzzer, battery sense, or radio — those are fixed by the selected board profile (FR-14.x).
- **FR-6.3** [Must]: The UI shall display a live status panel showing: state, decoded NAME, latitude, longitude, altitude, RSSI, battery voltage, firmware version. Live data shall refresh ≤ 2 s after each decoded frame.
- **FR-6.4** [Must]: The device shall expose `GET /api/state` returning the current state and most recent decoded frame as JSON.
- **FR-6.5** [Must]: The device shall expose `GET /api/config` returning the persisted configuration as JSON, with the WiFi password redacted (returned as `"***"` if set, `""` if unset).
- **FR-6.6** [Must]: The device shall expose `POST /api/config` accepting a JSON body matching the config schema, validating fields, persisting on success, and returning 400 with a problem description on failure.
- **FR-6.7** [Should]: The device shall expose `POST /api/ota/upload` and `GET /api/ota/progress` per FR-11.x. The HTML UI shall provide both a "Update from GitHub" button (browser fetches latest release, then uploads) and a "Upload local file" file-picker for sideloading.
- **FR-6.8** [Should]: The HTML/JS payload shall be embedded in the firmware via `EMBED_TXTFILES` (no SPIFFS / LittleFS).

#### Group 7 — WiFi (AP-only)

- **FR-7.1** [Must]: The device shall start a WiFi access point at boot named `MySondyGo-XXXX` (last 4 hex of the WiFi MAC) on 802.11 b/g/n channel 6 by default.
- **FR-7.2** [Must]: The AP shall be reachable at IP `192.168.4.1` and assign clients via DHCP from the `192.168.4.0/24` range.
- **FR-7.3** [Must]: The AP shall be **open** (no password) in v1, matching the MySondy Go device behaviour for ease of field operation.
- **FR-7.4** [Must]: The device shall **never** join a foreign WiFi network as STA. No SSID/PSK are stored or accepted.
- **FR-7.5** [Should]: A captive-portal redirect (DNS hijack + 302) shall direct any HTTP request on the AP to `http://192.168.4.1/` so phones automatically open the configuration page on connect.

#### Group 8 — Persistence

- **FR-8.1** [Must]: Configuration shall persist across reboots and power cycles via ESP-IDF NVS.
- **FR-8.2** [Must]: The device shall use documented defaults (§Appendix C) when NVS is empty or corrupt.
- **FR-8.3** [Must]: The long-press button (≥ 5 s) shall trigger factory reset (NVS namespace erase + reboot).
- **FR-8.4** [Must]: Factory reset shall be triggerable via the BLE `Re` command and via `POST /api/factory-reset`.

#### Group 9 — OLED display

- **FR-9.1** [Must]: The OLED shall show, while powered: frequency, sonde type, lock state, RSSI bar.
- **FR-9.2** [Should]: The OLED shall show AP-client-count (0..N) and BLE state (advertising / connected) icons.
- **FR-9.3** [Should]: The OLED shall show decoded NAME and altitude when in `TRACKING`.
- **FR-9.4** [Should]: A short button press shall toggle OLED on/off; the OLED shall remember its last state across reboots.

#### Group 10 — Button input

- **FR-10.1** [Must]: A short press (< 1 s) shall toggle OLED on/off.
- **FR-10.2** [Must]: A long press (≥ 5 s) shall trigger factory reset.
- **FR-10.3** [Should]: Press detection shall be debounced (50 ms minimum stable signal).

#### Group 11 — OTA (browser-uploaded only)

The receiver never makes outbound HTTPS requests. All OTA goes through the operator's phone browser, which has cellular for GitHub access while simultaneously connected to the receiver's AP.

- **FR-11.1** [Must]: The device shall expose `POST /api/ota/upload` accepting a multipart upload of a single `firmware.bin` file. The receiver shall stream the body directly into the inactive OTA partition without buffering the entire image in RAM.
- **FR-11.2** [Must]: The web UI shall provide an "Update firmware" panel that:
  1. Queries GitHub's Releases API (`https://api.github.com/repos/SensorsIot/Balloon-Receiver/releases/latest`) **from the browser** to discover the latest release tag and asset URL.
  2. Downloads the `.bin` asset from `*.githubusercontent.com` **in the browser**.
  3. POSTs the downloaded bytes to `/api/ota/upload`.
  4. Displays progress for both download and upload phases.
- **FR-11.3** [Must]: All GitHub API and asset traffic shall happen **in the browser** (the phone), never in the receiver. The receiver does not have or need internet.
- **FR-11.4** [Must]: The device shall use **dual A/B OTA partitions** with automatic rollback on boot failure (built-in ESP-IDF behaviour).
- **FR-11.5** [Must]: The device shall reject firmware images whose ESP-IDF image header does not match this project's chip target.
- **FR-11.6** [Should]: The device shall expose `GET /api/ota/progress` as a Server-Sent Events stream emitting `start`, `progress {pct}`, `complete`, and `error {msg}` events so the browser can render a progress bar during the upload phase.
- **FR-11.7** [Must]: After a successful OTA, the device shall reboot automatically into the new image and report the new version in the next `?`-reply (frame `3`) and `GET /api/version`.
- **FR-11.8** [Should]: The web UI shall allow the operator to manually upload a `.bin` from the phone's local storage (file-picker), bypassing the GitHub fetch step — useful for sideloading staging builds without internet.

#### Group 12 — Logging & diagnostics

- **FR-12.1** [Must]: The device shall emit structured logs via `ESP_LOG` at minimum level INFO at boot, falling back to a configured runtime level after init.
- **FR-12.2** [Should]: The device shall emit logs in parallel via UDP to a configurable host:port when set in NVS.
- **FR-12.3** [Must]: WiFi credentials, BLE pairing keys, and any configured passwords shall never appear in plaintext in any log channel.

#### Group 13 — Watchdog

- **FR-13.1** [Must]: A software task watchdog shall monitor every long-running task and reboot the device on timeout.
- **FR-13.2** [Should]: A heap monitor shall log a warning when free heap drops below 32 KB and trigger a reboot below 8 KB.
- **FR-13.3** [Must]: The watchdog shall not false-trigger during normal WiFi reconnection or OTA download.

#### Group 14 — Board profile

- **FR-14.1** [Must]: All hardware pin assignments (radio SPI, OLED I²C, LED, button, battery sense) shall be derived from a compile-time **board profile** identified at runtime by `board_id` in NVS. There shall be no individual-pin configuration via UI, BLE, or API.
- **FR-14.2** [Must]: v1 shall ship a single board profile: `board_id = 0` → **LILYGO TTGO LoRa32 V2.1_1.6**. The architecture shall permit additional profiles to be added by appending entries to the profile table without modifying any other component.
- **FR-14.3** [Should]: The active board profile's pin assignments shall populate the `OLED-SDA`, `OLED-SCL`, `OLED-RST`, `LED-PIN`, `BAT-PIN`, `BUZ-PIN` fields in the BLE settings reply (frame `3`) — so the MySondy Go app sees consistent values for the actual hardware in use.
- **FR-14.4** [Must]: BLE pin-config commands (`oled_sda`, `oled_scl`, `oled_rst`, `led_pout`, `buz_pin`, `battery`) are accepted (per FR-5.9) but discarded — they shall not change the runtime pin map. This preserves protocol compatibility with the MySondy Go app while enforcing the board-profile model.
- **FR-14.5** [May]: A future build may surface the board-profile dropdown for early prototyping/bring-up; in v1 the dropdown shows only `TTGO LoRa32 V2.1_1.6` (selected, locked).

### 4.2 Non-Functional Requirements (NFR)

- **NFR-1.1** [Must]: BLE notification latency from decoded frame to NUS notification dispatched: ≤ 100 ms (95th percentile).
- **NFR-1.2** [Should]: Web UI live panel update latency from decoded frame to DOM updated: ≤ 2 s.
- **NFR-1.3** [Should]: RS41 lock-acquisition time at strong signal (≥ −80 dBm) from cold start: ≤ 30 s.
- **NFR-2.1** [Must]: The device shall run continuously for ≥ 24 h with concurrent BLE + WiFi + active decoding without reboot or memory leak (free heap drift ≤ 5 %).
- **NFR-2.2** [Must]: On any task crash, the device shall reboot and resume normal operation within 60 s.
- **NFR-3.1** [Must]: The device shall be compatible with the official MySondy Go Android app (latest Play Store version) on Android 11 or newer, without app modifications.
- **NFR-4.1** [Must]: WiFi credentials shall be stored in NVS with the encryption-at-rest provided by ESP-IDF NVS encryption (when enabled).
- **NFR-4.2** [Must]: OTA images shall be integrity-verified (image header validation; SHA-256 of the bin if signed).
- **NFR-4.3** [Should]: HTTP server shall reject inputs > 4 KB on the config endpoint to prevent DoS via oversized POST.
- **NFR-5.1** [Should]: The decoder dispatch table (§FR-2.6) shall require no more than ~50 lines of glue per added decoder.
- **NFR-6.1** [May]: Battery voltage shall be reported within ±50 mV of true value when battery is present.
- **NFR-7.1** [Must]: All firmware sources licensed under GPL-2.0 or compatible. Decoders ported from `rs1729/RS` retain GPL-2.0 attribution.

### 4.3 Constraints

- **C-1**: Hardware target in v1 is fixed at TTGO LoRa32 V2.1_1.6 via the board profile mechanism (FR-14.x). Additional boards are added by appending profile entries; no individual-pin configuration is exposed.
- **C-2**: Build system is ESP-IDF only. No Arduino-core compatibility shim.
- **C-3**: BLE stack is NimBLE (not Bluedroid).
- **C-4**: SX1276 is operated in raw FSK mode only. LoRa-mode operation is out of scope.
- **C-5**: License must remain GPL-2.0 due to inclusion of `rs1729/RS` derivative work.
- **C-6**: Receive-only operation. No regulatory licensing assumed for the device under operation; user is responsible for antenna and environment.
- **C-7**: Embedded HTML UI must fit comfortably within the firmware image (target: HTML+CSS+JS payload ≤ 32 KB minified).

---

## 5. Risks, Assumptions & Dependencies

### 5.1 Technical Risks

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| **R-1** MySondy Go BLE service UUIDs unknown — the API PDF specifies the application protocol but not GATT UUIDs. | High | High | Confirm via nRF Connect against real MySondy Go firmware before Phase 4. Backup plan: contact author Mirko Dalmonte (`iz4pnn@gmail.com`). Worst case: choose Nordic UART Service (NUS) UUIDs and document a small Android-app patch. |
| **R-2** SX1276 FSK config quirks — sync-word matching, deviation, and RX BW interactions are not always documented; bench-tuning may be required per sonde type. | Medium | Medium | Allocate 2 days of bench time in Phase 2 with a known RS41 capture; document the working register values per sonde type as a baseline. |
| **R-3** rs1729 decoder port complexity — the C is research-grade, with `stdin`/`stdout` assumptions and ad-hoc globals. | Medium | Medium | Treat the port as a one-time fork: encapsulate inside `decoder_rs41`, do not try to re-merge upstream. Track upstream changes manually. |
| **R-4** BLE + WiFi coexistence on shared 2.4 GHz radio. | Low | Medium | Standard ESP32 dual-mode is well supported; allocate one dedicated soak test in Phase 5 (NFR-2.1). |
| **R-5** No real sonde available for testing during dry seasons / non-launch windows. | Medium | Medium | Record I/Q with rtl-sdr during a launch window; replay through the SX1276 via attenuator and dummy-load coupling, or via a second SX1276 in TX mode (test rig only — not the production firmware). |
| **R-6** Flash / RAM pressure when adding NimBLE + WiFi + httpd + OLED + decoder. | Low | Medium | Budget early (target build ≤ 70 % of 4 MB flash, ≤ 60 % of 320 KB heap at idle). Profile in Phase 1. |
| **R-7** GitHub Releases API CORS for browser-side fetch. The browser must be able to read `api.github.com` JSON cross-origin from the receiver's `192.168.4.1` page. | Low | Low | `api.github.com` sets `Access-Control-Allow-Origin: *` for unauthenticated GET requests on public repos — verified at design time. |
| **R-8** Browser may refuse to download `.bin` from `*.githubusercontent.com` and POST to `192.168.4.1` (mixed-content / private-network access policies on Chrome and Safari). | Medium | Medium | Test with current Chrome and Safari versions in Phase 5. Mitigation if blocked: use a small tag-redirect endpoint on the receiver (still no outbound traffic — receiver just returns the URL the browser already has) or fall back to file-picker sideload (FR-11.8). |

### 5.2 Assumptions

- (assumed) **AP fallback SSID format**: `MySondyGo-XXXX` where `XXXX` is the last 4 hex digits of the WiFi MAC. AP is **open** (no password) for ease of first-boot setup, matching captive-portal best practice for hobbyist devices.
- (assumed) **Default frequency**: 404.000 MHz (per MySondy Go API default) — common-launch frequency in central Europe; user adjusts for their region.
- (assumed) **`rs41.rxbw` and other `*.rxbw` commands take effect**: these are RF parameters (not pin config), so they are honoured rather than ignored. Default values match MySondy Go API Appendix 2 defaults.
- (assumed) **`sleep` command is a no-op in v1** (logged for debug); deep-sleep is out of scope. May be implemented in v1.1 if battery use becomes a goal.
- (assumed) **BLE pairing**: "Just Works" (no static passkey, no MITM protection). Acceptable because the device stores no secrets — there are no WiFi credentials, no API tokens, no user data.
- (assumed) **`PILOT` (tipo=4)** is accepted and stored, but no decoder runs in v1 — state remains `NO_SIGNAL`. The user may select PILOT and the device will not crash, simply not produce frames `1` or `2`.
- (assumed) **Logging UDP target** is unset by default; serial-only logging is the baseline.
- (assumed) **OTA signing** is OFF by default in v1 (image header validation only). Signed-OTA is a v1.1 hardening item.
- (assumed) **`tipo=4` PILOT** maps to no decoder; the device responds to settings queries with `tipo=4` but produces only `0/.../o` heartbeat frames.
- (assumed) **Decoded `BAT%` and `BATV`**: when no battery is present, `BAT%=-1`, `BATV=0`. When battery is present, computed from the GPIO35 voltage divider (2:1) with `vBatMin`/`vBatMax`/`vBatType` curves per FR-5b.x. Default `vBatMax=4200 mV` (project choice) rather than the API's `4180 mV`.

### 5.3 External Dependencies

- ESP-IDF (latest stable) — Espressif official toolchain.
- NimBLE host stack (`esp-nimble-cpp` or vanilla `esp_nimble` — vanilla preferred).
- `esp_http_server`, `esp_wifi` (SoftAP only), `nvs_flash`, `esp_ota_ops` — all part of ESP-IDF.
- `rs1729/RS` — GPL-2.0 reference decoder source (vendored, not submodule, at a pinned commit).
- MySondy Go Android app (closed source, Play Store) — not modified, used as conformance peer.

### 5.4 Environmental Constraints

- 433 MHz RF environment may be congested in some regions (LPD-433, ISM); user is responsible for choosing an operating frequency.
- Antenna and feedline are user-supplied; no specific gain or impedance is assumed beyond standard 50 Ω.

### 5.5 Regulatory Constraints

- Receive-only operation; no transmit licensing implications.
- The user is responsible for compliance with local radio-listener regulations (most jurisdictions allow general RX of any frequency for personal use; some have specific exclusions — outside firmware scope).

---

## 6. Interface Specifications

### 6.1 External Interfaces

#### 6.1.1 BLE — Nordic UART Service (NUS)

| Attribute | Value |
|---|---|
| Service UUID | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` (TBC against MySondy Go firmware — see R-1) |
| RX Characteristic | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` — Write, Write-Without-Response |
| TX Characteristic | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` — Notify |
| Advertising name | `MySondyGo-XXXX` (last 4 hex of BLE MAC) |
| Connection parameters | Default ESP32 NimBLE (interval 7.5–48 ms, timeout 4 s) |
| MTU | Negotiated — minimum 23 bytes per BLE 4.0; longer frames split per BLE-023 |
| Pairing | None ("Just Works") |

#### 6.1.2 MySondy Go ASCII Protocol (over NUS)

The protocol on top of the NUS byte stream conforms to MySondy Go API v3.0. Output frames described in §4.1 Group 4. Input commands described in §4.1 Group 5. Frame examples in §10 Appendix B.

#### 6.1.3 HTTP — Configuration UI and JSON API

| Endpoint | Method | Purpose | Payload |
|---|---|---|---|
| `/` | GET | HTML config page | text/html, embedded |
| `/api/state` | GET | Current state + last frame | JSON, see §6.3.1 |
| `/api/config` | GET | Persisted configuration | JSON, see §6.3.2 (passwords redacted) |
| `/api/config` | POST | Update configuration | JSON body (§6.3.2); 200 OK on success, 400 + problem JSON on failure |
| `/api/factory-reset` | POST | Trigger factory reset + reboot | empty body; 202 Accepted then reboot |
| `/api/ota/upload` | POST | Browser-uploaded OTA (multipart `firmware.bin`) | 202 Accepted; receiver streams body to OTA partition. |
| `/api/ota/progress` | GET | OTA progress stream (Server-Sent Events) | text/event-stream; events `start`, `progress {pct}`, `complete`, `error {msg}`. |
| `/api/version` | GET | Firmware build info | `{"version": "1.0.0+abcdef1", "idf": "5.x", "github_repo": "SensorsIot/Balloon-Receiver"}` |

All JSON responses use `Content-Type: application/json; charset=utf-8`. CORS not enabled (local-network use).

#### 6.1.4 Serial console (UART0 / USB-CDC)

UART0 at 115200 8N1, used exclusively for `ESP_LOG` output and ESP-IDF monitor. **Not** a MySondy Go API transport in v1 (BLE-only).

#### 6.1.5 OLED display (SSD1306)

Local display, no network role. Layout described in §10 Appendix D.

### 6.2 Internal Interfaces

#### 6.2.1 RF byte queue

`QueueHandle_t` carrying `uint8_t` items (one per FIFO byte). Producer: `rf_isr_drain` task. Consumer: `decoder_rs41` task. Depth: 1024 bytes (covers ~200 ms of RS41 traffic at 4800 baud with margin).

#### 6.2.2 Sonde frame event

ESP-IDF event loop event `SONDE_EVT_FRAME` carrying `sonde_frame_t*`. Subscribers: state manager, BLE codec, OLED UI.

```c
typedef struct {
    sonde_state_t state;        // NO_SIGNAL / NAME_ONLY / TRACKING
    sonde_type_t  type;         // RS41 / M20 / M10 / PILOT / DFM
    char     name[16];          // serial / NAME field
    double   lat;               // degrees
    double   lon;               // degrees
    int32_t  alt_m;             // metres
    float    h_vel_kmh;
    float    v_vel_ms;
    int16_t  rssi_dbm;
    int32_t  afc_hz;
    bool     bk_active;
    int32_t  bk_remaining_s;
    int64_t  monotonic_us;      // device monotonic time at decode
} sonde_frame_t;
```

#### 6.2.3 Config event

Event `CFG_EVT_CHANGED` carrying a bitmask of changed fields. Subscribers: RF driver (re-tunes on freq change), state manager (clears cache on type change), OLED UI.

### 6.3 Data Models / Schemas

#### 6.3.1 `/api/state` response (JSON)

```json
{
  "state": "TRACKING",
  "type": "RS41",
  "freq_khz": 404600,
  "name": "S1234567",
  "lat": 47.12345,
  "lon": 8.54321,
  "alt_m": 12345,
  "h_vel_kmh": 30.5,
  "v_vel_ms": -5.2,
  "rssi_dbm": -87,
  "afc_hz": 1234,
  "bat_pct": -1,
  "bat_mv": 0,
  "version": "1.0.0+abcdef1",
  "uptime_s": 3600
}
```

#### 6.3.2 `/api/config` schema (JSON)

```json
{
  "board": "ttgo_lora32_v21_16",
  "freq_khz": 404600,
  "sonde_type": "RS41",
  "mycall": "MYCALL",
  "ble_on": true,
  "vbat_min_mv": 2950,
  "vbat_max_mv": 4200,
  "vbat_type": 1
}
```

`board` is read-only in v1 (only one entry) but reserved for future expansion. POST-ing a different value returns 400.


#### 6.3.3 NVS schema

| Namespace | Key | Type | Default | Notes |
|---|---|---|---|---|
| `mysongo` | `board_id` | `u8` | `0` | 0 = TTGO LoRa32 V2.1_1.6; only entry in v1 |
| `mysongo` | `freq_khz` | `u32` | `404000` | kHz, range 137200..524800 |
| `mysongo` | `sonde_type` | `u8` | `1` (RS41) | 1=RS41, 2=M20, 3=M10, 4=PILOT, 5=DFM |
| `mysongo` | `mycall` | `str[9]` | `"MYCALL"` | Max 8 chars + NUL |
| `mysongo` | `ble_on` | `u8` | `1` | 0/1 |
| `mysongo` | `oled_persist` | `u8` | `1` | OLED last on/off state |
| `mysongo` | `freqofs_hz` | `i32` | `0` | Per-radio offset (MySondy `freqofs`) |
| `mysongo` | `rxbw_idx` | `u8` per type | RS41=4, M20=7, M10=7, PILOT=7, DFM=6 | MySondy Appendix 2 indices |
| `mysongo` | `vbat_min_mv` | `u16` | `2950` | Battery empty threshold |
| `mysongo` | `vbat_max_mv` | `u16` | `4200` | Battery full threshold (project default; API default is 4180) |
| `mysongo` | `vbat_type` | `u8` | `1` | 0=Linear, 1=Sigmoidal, 2=Anti-sigmoidal |
| `mysongo` | `udp_log_host` | `str[32]` | `""` | Optional UDP log target |
| `mysongo` | `udp_log_port` | `u16` | `0` | 0 = disabled |

#### 6.3.4 Sonde RF profile (compile-time table)

```c
typedef struct {
    sonde_type_t type;
    uint32_t     bitrate_bps;
    uint32_t     freq_dev_hz;
    uint8_t      sync_word[8];
    uint8_t      sync_len;
    uint8_t      rxbw_idx_default;
} rf_profile_t;
```

RS41 baseline: bitrate 4800, deviation 4800, sync 8 bytes (per RS41 frame format), rxbw_idx 4 (6.3 kHz).

### 6.4 Commands / Opcodes (MySondy Go ASCII subset)

See §10 Appendix B for the complete command/frame table. Honoured commands and accept-and-ignore commands are listed in FR-5.x. Unrecognised commands inside a valid `o{...}o` envelope are logged and ignored without error.

---

## 7. Operational Procedures

### 7.1 Flashing (initial)

1. Connect TTGO LoRa32 v2.1 via USB-C.
2. From the project root: `idf.py -p /dev/ttyUSB0 flash monitor`.
3. Hold `BOOT` (GPIO0) and tap `EN` (RESET) if the board does not auto-enter download mode (CP210x with autoreset usually does).
4. First-boot output: NVS empty → defaults applied → AP `MySondyGo-XXXX` starts.

### 7.2 First-time setup

1. Power the device.
2. On a phone, join the open WiFi network `MySondyGo-XXXX`.
3. Browser opens the configuration page automatically (captive-portal redirect); if not, navigate to `http://192.168.4.1`.
4. Pick frequency and sonde type, set MYCALL, optionally calibrate the battery, click Save.
5. Done — there is no "join home WiFi" step. The device stays on its own AP forever.

### 7.3 Field operation (normal)

1. Power the device (USB or battery).
2. Open the MySondy Go Android app on the phone.
3. Scan and connect to `MySondyGo-XXXX` over Bluetooth.
4. App shows live decoded data. Frequency and sonde-type changes from the app are honoured.
5. OLED mirrors the same data (when on; toggle with short press).
6. To change settings without the app, the phone can also join the device's WiFi AP and use the web page in parallel.

### 7.4 Configuration update workflows

| Path | Where | Effect |
|---|---|---|
| Web UI form | Browser → `POST /api/config` | Persisted to NVS, applied immediately (some fields may require radio re-tune, no reboot). |
| BLE `o{f=…}o` | MySondy Go app | Persisted, applied immediately. |
| BLE `o{Re}o` | MySondy Go app | Factory reset + reboot. |
| Long-press button | Hardware | Factory reset + reboot. |

### 7.5 OTA firmware update

The receiver has no internet — the phone does the GitHub fetch.

1. On the phone, ensure cellular data is on.
2. Connect the phone to the receiver's AP `MySondyGo-XXXX`.
3. Open `http://192.168.4.1` in the phone's browser.
4. Click "Update from GitHub". The browser:
   - Queries `https://api.github.com/repos/SensorsIot/Balloon-Receiver/releases/latest` over cellular.
   - Downloads the `.bin` from the asset URL over cellular.
   - Uploads it to the receiver via the AP.
5. The receiver writes to the inactive OTA slot, verifies the image header, and reboots. If the new image fails to boot, A/B rollback automatically restores the previous image.

For sideloading without internet, the same UI offers a "Upload local file" file-picker that takes a `.bin` from the phone's storage.

### 7.6 Recovery procedures

| Scenario | Action |
|---|---|
| Settings stuck or corrupted | Long-press button → factory reset → reconfigure via AP. |
| Device unresponsive | Power-cycle. If still unresponsive after 60 s of boot, hold button during reset → flash recovery firmware via USB. |
| Bad OTA image | Automatic rollback to previous image via A/B partition. No user action needed. |
| Forgot configured frequency | Read OLED, or query `GET /api/config`. |

---

## 8. Verification & Validation

### 8.1 Phase 1 Verification — Infrastructure

| Test ID | Feature | Procedure | Success Criteria |
|---|---|---|---|
| TC-NVS-100 | Configuration persistence | Set freq via `POST /api/config`, reboot, read back. | Value identical post-reboot. |
| TC-NVS-101 | Defaults on first boot | Erase NVS, boot. | Device enters AP mode with default freq=404.000 MHz. |
| TC-NVS-102 | Factory reset via button | Configure, hold button 5 s. | NVS cleared, AP mode resumes. |
| TC-NVS-103 | Factory reset via command | Send `o{Re}o` over BLE. | Same as button reset. |
| TC-CP-100 | Captive portal first boot | Empty NVS → join AP → browse. | Portal serves HTML on `192.168.4.1`; settings save survives reboot; AP remains active. |
| TC-CP-101 | Captive portal via button | Long-press while running. | Factory reset triggers — captive portal re-appears post-reboot. |
| TC-WDT-100 | Software watchdog | Force a task hang in test build. | Reboot within 90 s; reset reason = watchdog. |
| TC-WDT-102 | Heap monitor | Allocate until heap below threshold. | Warning at 32 KB, reboot at 8 KB. |
| TC-LOG-100 | Boot log | Power on, capture serial. | Boot banner with version + tag-formatted log entries. |
| TC-LOG-101 | UDP log | Set UDP target, boot. | Receive boot log on UDP listener. |
| EC-LOG-200 | UDP target unreachable | Set unreachable IP, boot. | Device runs normally, serial log unaffected. |

### 8.2 Phase 2-3 Verification — RF and decoder

| Test ID | Feature | Procedure | Success Criteria |
|---|---|---|---|
| TC-RF-100 | SX1276 init | Boot, check register dump. | Mode = FSK RX, freq matches config, no TX paths active. |
| TC-RF-101 | Frequency change | Send `o{f=404.2}o`. | Active register reflects 404.200 MHz within 1 s. |
| TC-RF-102 | RX-only audit | Static analysis of `rf_sx1276` source. | No path writes mode register to TX (`0b011`). |
| TC-RF-103 | Sync match on capture | Replay known RS41 capture. | Byte queue receives ≥ 320 bytes per RS41 frame (frame size). |
| TC-DEC-100 | RS41 vector decode | Decode SHA-pinned WAV/IQ test vector. | Lat/lon/alt within 5 m of ground truth. |
| TC-DEC-101 | Lock-acquire time | Strong signal cold start. | TRACKING within 30 s. |
| TC-DEC-102 | Decoder soak | Run 1 h with continuous signal. | No crash, no leak (ΔHeap ≤ 5 %), all frames decoded. |
| TC-DEC-103 | RS dispatch | Switch `tipo=2` (M20) — no decoder. | State transitions to NO_SIGNAL within 30 s; no crash. |

### 8.3 Phase 4 Verification — BLE / MySondy Go protocol

| Test ID | Feature | Procedure | Success Criteria |
|---|---|---|---|
| TC-BLE-100 | Discovery & connection | Scan with nRF Connect; connect; enumerate. | NUS service visible; connect succeeds. |
| TC-BLE-101 | NUS data transfer | Subscribe TX; receive heartbeat. | `0/RS41/404.600/.../o` arrives within 2 s of connection. |
| TC-BLE-102 | GATT read/write | Write `o{?}o` to RX char. | `3/...` reply received on TX notify. |
| TC-BLE-103 | Notification latency | Trigger 10 decodes; measure. | TX notify ≤ 100 ms after frame published. |
| TC-MSG-100 | Frame 0 (no signal) | Tune to empty channel. | `0/RS41/137.200/.../-1/.../-1/1.0.0+sha/o` cycles ≥ 0.5 Hz. |
| TC-MSG-101 | Frame 1 (tracking) | Lock RS41. | Frame 1 with valid lat/lon ≥ 0.5 Hz. |
| TC-MSG-102 | Frame 2 (name only) | Force decode-without-GPS condition. | Frame 2 emitted. |
| TC-MSG-103 | Frame 3 (settings reply) | Send `o{?}o`. | Frame 3 with all fields present, units correct. |
| TC-MSG-110 | Command `f` | Send `o{f=405.0}o`. | RF re-tunes; subsequent frames show `405.000`. |
| TC-MSG-111 | Command `tipo` | Send `o{tipo=2}o`. | Stored; state goes NO_SIGNAL; frame 0 shows TYPE=M20. |
| TC-MSG-112 | Command `tipo=4` (PILOT no-op) | Send `o{tipo=4}o`. | Accepted, no crash, `0/PILOT/...` heartbeat. |
| TC-MSG-113 | Command `re` | Send `o{re}o`. | Reboot within 5 s. |
| TC-MSG-114 | Command `Re` | Send `o{Re}o`. | NVS erased + reboot; AP mode. |
| TC-MSG-115 | Pin-config ignored | Send `o{oled_sda=99}o`. | Logged; actual SDA pin unchanged; frame 3 still reports the active board profile's value (21), not the rejected 99. |
| TC-BOARD-100 | Board profile drives pins | Boot fresh NVS. | OLED works (active board profile applied); frame 3 reports OLED-SDA=21, OLED-SCL=22, OLED-RST=16, LED-PIN=25, BAT-PIN=35. |
| TC-BOARD-101 | UI shows no pin fields | Open `/`. | No form field for any GPIO pin; only the `board` dropdown appears for hardware selection. |
| TC-MSG-116 | Sleep no-op | Send `o{sleep=10}o`. | Logged as no-op; device stays awake. |
| TC-MSG-117 | rxbw applied | Send `o{rs41.rxbw=7}o`. | RX BW register reflects 12.5 kHz. |
| TC-MSG-118 | Invalid envelope | Send `f=404.0` (no `o{}o`). | Ignored silently. |
| TC-BAT-100 | Battery voltage measurement | Connect 18650 at 4.10 V (DMM-verified). Read `BATV` from frame `0`. | `BATV` within ±50 mV of 4100. |
| TC-BAT-101 | No-battery detection | Disconnect battery, run on USB only. | `BAT%` = `-1`, `BATV` = 0 (or noise floor). |
| TC-BAT-102 | vBatMax calibration | Send `o{vBatMax=4150}o`, then read `3/...` frame. Battery at 4.15 V. | `BAT%` = 100. |
| TC-BAT-103 | vBatType curves | Battery at 3.7 V mid-discharge with `vBatType=0` (linear) vs `vBatType=1` (sigmoidal). | Linear ≈ 65 %, sigmoidal ≈ 50 % (per S-curve). |
| TC-BAT-104 | Web UI calibration round-trip | Open UI, set vBatMax=4180, save, reload. | Persisted; new value reflected in frame `3`. |
| EC-BLE-201 | Connect/disconnect churn | Connect/disconnect 20× rapidly. | No crash, heap stable, advertising resumes. |
| EC-BLE-203 | BLE+WiFi coexistence | OTA over WiFi while BLE connected. | BLE stays connected or recovers; OTA succeeds. |
| TC-MSG-200 | MySondy Go app round-trip | Connect via the official app. | App shows live data; commands round-trip. |

### 8.4 Acceptance Tests

| Test ID | Scenario | Success Criteria |
|---|---|---|
| ACC-001 | 24 h soak, BLE+WiFi+RS41 active | Free heap drift ≤ 5 %, no reboots, no decoded-frame loss > 5 %. |
| ACC-002 | Field acquisition | Real launch within 100 km. Lock acquired ≤ 60 s after sonde rises above local horizon. |
| ACC-003 | OTA round-trip | Update from v1.0.0 → v1.0.1 → bad image → automatic rollback. |
| TC-OTA-100 | Successful OTA — browser upload path | Phone with cellular on receiver AP; click "Update from GitHub" in web UI. Phone fetches latest release from GitHub, uploads via `POST /api/ota/upload`. | Update applies; new version reported on reboot. |
| TC-OTA-101 | OTA rollback | Upload intentionally-broken image. | Auto-rollback within 2 boot attempts. |
| TC-OTA-102 | Version reporting | `GET /api/version` and BLE `?` response. | Same version string in both. |
| TC-OTA-103 | Local file sideload | Use "Upload local file" picker with a stashed `.bin`. | Update applies same as TC-OTA-100. |
| TC-OTA-104 | Progress SSE | Subscribe to `GET /api/ota/progress`. Trigger any OTA. | `start`, ≥ 5 `progress` events, `complete` (or `error`). |
| TC-OTA-105 | No outbound HTTPS | Capture all packets from receiver during OTA. | Zero packets to anything but the connected AP client. The receiver makes no outbound DNS or TLS. |

### 8.5 Traceability Matrix

| Requirement | Priority | Test Case(s) | Status |
|---|---|---|---|
| FR-1.1 | Must | TC-RF-100 | Covered |
| FR-1.2 | Must | TC-MSG-117, TC-RF-101 | Covered |
| FR-1.3 | Must | TC-MSG-110 (boundary cases extension) | Covered |
| FR-1.4 | Should | TC-DEC-100, TC-MSG-101 | Covered |
| FR-1.5 | Must | TC-RF-102 | Covered |
| FR-2.1 | Must | TC-DEC-100, TC-DEC-101 | Covered |
| FR-2.2 | Must | TC-DEC-100 | Covered |
| FR-2.3 | Must | TC-DEC-100 (implicit — Reed-Solomon required for any valid decode) | Covered |
| FR-2.4 | Should | TC-DEC-100 | Covered |
| FR-2.5 | Should | TC-MSG-101 (BK fields populated) | Covered |
| FR-2.6 | May | TC-DEC-103 | Covered |
| FR-3.1 | Must | TC-MSG-100, TC-MSG-101, TC-MSG-102 | Covered |
| FR-3.2 | Must | TC-MSG-100 (after lock loss) | Covered |
| FR-3.3 | Must | TC-MSG-101 | Covered |
| FR-3.4 | Should | TC-MSG-102 | Covered |
| FR-4.1 | Must | TC-BLE-101, TC-MSG-100..103 | Covered |
| FR-4.2 | Must | TC-MSG-100 | Covered |
| FR-4.3 | Must | TC-MSG-101 | Covered |
| FR-4.4 | Should | TC-MSG-102 | Covered |
| FR-4.5 | Must | TC-MSG-103 | Covered |
| FR-4.6 | Must | TC-MSG-100..103 (all assert `BUZMUTE=-1`) | Covered |
| FR-5.1 | Must | TC-MSG-118 | Covered |
| FR-5.2 | Must | TC-MSG-110 | Covered |
| FR-5.3 | Must | TC-MSG-111, TC-MSG-112 | Covered |
| FR-5.4 | Must | TC-MSG-103 | Covered |
| FR-5.5 | Must | TC-MSG-113, TC-MSG-114 | Covered |
| FR-5.6 | Should | TC-MSG-103 (mute echoed back) | Covered |
| FR-5.7 | Should | TC-MSG-103 | Covered |
| FR-5.8 | Should | TC-MSG-117 | Covered |
| FR-5.9 | May | TC-MSG-115 | Covered |
| FR-5.10 | May | TC-MSG-116 | Covered |
| FR-5.11 | Should | TC-BAT-102, TC-BAT-104 | Covered |
| FR-5b.1 | Must | TC-BAT-100 | Covered |
| FR-5b.2 | Should | TC-BAT-100 (accuracy budget includes calibration) | Covered |
| FR-5b.3 | Should | TC-BAT-100 (no jitter > 30 mV between consecutive samples) | Covered |
| FR-5b.4 | Must | TC-BAT-100, TC-BAT-101 | Covered |
| FR-5b.5 | Must | TC-BAT-103 | Covered |
| FR-5b.6 | Should | TC-BAT-104 (defaults observed in fresh NVS) | Covered |
| FR-6.1 | Must | TC-CP-100 | Covered |
| FR-6.2 | Must | TC-CP-100 | Covered |
| FR-6.3 | Must | ACC-001 (live panel updates verified during soak) | Covered |
| FR-6.4 | Must | TC-CP-100 (state endpoint queried during portal flow) | Covered |
| FR-6.5 | Must | TC-NVS-100 (psk redaction verified) | Covered |
| FR-6.6 | Must | TC-CP-100 | Covered |
| FR-6.7 | Should | TC-OTA-100 | Covered |
| FR-6.8 | Should | Build artefact inspection (single binary, no SPIFFS partition) | Covered (build check) |
| FR-7.1 | Must | TC-CP-100 (AP visible at boot) | Covered |
| FR-7.2 | Must | TC-CP-100 (DHCP from 192.168.4.0/24) | Covered |
| FR-7.3 | Must | TC-CP-100 (open AP, no PSK) | Covered |
| FR-7.4 | Must | TC-OTA-105 (no outbound traffic — proves no STA) | Covered |
| FR-7.5 | Should | TC-CP-100 (captive portal redirect) | Covered |
| FR-8.1 | Must | TC-NVS-100 | Covered |
| FR-8.2 | Must | TC-NVS-101, EC-NVS-200 | Covered |
| FR-8.3 | Must | TC-NVS-102 | Covered |
| FR-8.4 | Must | TC-MSG-114, TC-NVS-103 | Covered |
| FR-9.1 | Must | TC-OLED-100 | Covered |
| FR-9.2 | Should | TC-OLED-100 | Covered |
| FR-9.3 | Should | TC-OLED-100 | Covered |
| FR-9.4 | Should | TC-OLED-101 | Covered |
| FR-10.1 | Must | TC-OLED-101 | Covered |
| FR-10.2 | Must | TC-NVS-102 | Covered |
| FR-10.3 | Should | TC-OLED-101 | Covered |
| FR-11.1 | Must | TC-OTA-100 | Covered |
| FR-11.2 | Must | TC-OTA-100 | Covered |
| FR-11.3 | Must | TC-OTA-105 | Covered |
| FR-11.4 | Must | TC-OTA-101, EC-OTA-201 | Covered |
| FR-11.5 | Must | EC-OTA-202 | Covered |
| FR-11.6 | Should | TC-OTA-104 | Covered |
| FR-11.7 | Must | TC-OTA-100, TC-OTA-102 | Covered |
| FR-11.8 | Should | TC-OTA-103 | Covered |
| FR-12.1 | Must | TC-LOG-100 | Covered |
| FR-12.2 | Should | TC-LOG-101 | Covered |
| FR-12.3 | Must | EC-NVS-203 | Covered |
| FR-13.1 | Must | TC-WDT-100 | Covered |
| FR-13.2 | Should | TC-WDT-102 | Covered |
| FR-13.3 | Must | EC-WDT-201 | Covered |
| FR-14.1 | Must | TC-BOARD-100, TC-BOARD-101 | Covered |
| FR-14.2 | Must | TC-BOARD-100 | Covered |
| FR-14.3 | Should | TC-BOARD-100 | Covered |
| FR-14.4 | Must | TC-MSG-115 | Covered |
| FR-14.5 | May | TC-BOARD-101 | Covered |
| NFR-1.1 | Must | TC-BLE-103 | Covered |
| NFR-1.2 | Should | ACC-001 (live panel timing observed) | Covered |
| NFR-1.3 | Should | TC-DEC-101 | Covered |
| NFR-2.1 | Must | ACC-001 | Covered |
| NFR-2.2 | Must | TC-WDT-100 | Covered |
| NFR-3.1 | Must | TC-MSG-200 | Covered |
| NFR-4.1 | Must | EC-NVS-203 | Covered |
| NFR-4.2 | Must | EC-OTA-202 | Covered |
| NFR-4.3 | Should | EC-CP-203 (oversized POST extension) | Covered |
| NFR-5.1 | Should | Code review checklist (per-decoder hook ≤ 50 LOC) | Covered (review) |
| NFR-6.1 | May | (battery present) ACC-001 reading vs DMM | Optional |
| NFR-7.1 | Must | License audit (build artefact) | Covered (build check) |

OLED tests (TC-OLED-100, TC-OLED-101) are defined inline:

- **TC-OLED-100** — Power on. OLED shows boot banner → status page within 5 s. Status page contains: frequency, sonde type, lock state, RSSI bar, WiFi/BLE icons. When in TRACKING, NAME and altitude are visible.
- **TC-OLED-101** — While running, short-press button. OLED toggles off. Press again — toggles on. Reboot — OLED state restored from NVS.

No open gaps in v1.

---

## 9. Troubleshooting Guide

| Symptom | Likely Cause | Diagnostic Steps | Corrective Action |
|---|---|---|---|
| Device boots but no AP visible | WiFi init failed or NVS corrupt | Check serial log for "NVS init" or "esp_wifi_start" errors | Power-cycle. If persists, long-press button → factory reset. |
| AP visible but portal does not load | Browser caching old captive portal | Open `http://192.168.4.1` directly | Clear browser cache; use private window. |
| WiFi joins but `/api/state` 404 | HTTP server task crash | Serial log for httpd errors | Reboot. If recurring, check heap via `/api/version` (extended diagnostic). |
| MySondy Go app cannot find device | BLE off or wrong UUID | Enable BLE in `/api/config`; verify UUIDs in nRF Connect | Toggle BLE on; if UUIDs differ from app expectation, see R-1. |
| MySondy Go app connects but shows stale data | NUS notify not enabled by app | nRF Connect: subscribe to TX char | Confirm subscription happened; reconnect if needed. |
| No sonde decoded despite signal | Wrong sonde type or wrong frequency | Check OLED + `/api/state.type` and `freq_khz` | Adjust via app or web UI. |
| RSSI strong but no decode | RX BW or sync word mismatch | Send `o{rs41.rxbw=4}o` to confirm default | Adjust per MySondy Appendix 2. |
| Web UI live panel frozen | WebSocket / polling broken | Browser dev console for failed `/api/state` requests | Reload; if persistent, reboot. |
| OTA fails 500 | URL unreachable from device, or HTTPS cert untrusted | Check `idf.py monitor` during OTA | Verify URL via cURL from a host on same network; for self-signed, embed CA bundle (build option). |
| Repeated reboots, "rst:0xc" in serial | Watchdog firing, possibly task hang | `idf.py monitor` for backtrace | Capture trace, file issue with logs. |
| Frame 1 emitted but `LAT=0/LON=0` | Decoder running on noise; spurious lock | Check RSSI; ensure correct sonde flying nearby | None — wait for real signal or change frequency. |

---

## 10. Appendix

### A. Constants and configuration defaults

| Constant | Value |
|---|---|
| AP fallback SSID prefix | `MySondyGo-` |
| AP IP | `192.168.4.1` |
| AP startup timeout | 5 s |
| Default frequency | 404.000 MHz |
| Default sonde type | RS41 |
| Default MYCALL | `MYCALL` |
| Watchdog timeout | 60 s |
| Heap warning threshold | 32 KB |
| Heap critical threshold | 8 KB |
| BLE notification rate (heartbeat) | 1 Hz |
| Lock-loss timeout | 30 s |
| HTML+JS payload budget | ≤ 32 KB minified |

### B. MySondy Go API v3.0 frame reference (subset implemented)

**Output (device → app):**

| Frame | Format |
|---|---|
| `0` (no signal) | `0/TYPE/FREQ/SIGN/BAT%/BATV/BUZMUTE/VER/o` |
| `1` (tracking) | `1/TYPE/FREQ/NAME/LAT/LON/ALT/HVEL/VVEL/SIGN/BAT%/AFC/BK/BKTIME/BATV/BUZMUTE/RES/RES/RES/VER/o` |
| `2` (name only) | `2/TYPE/FREQ/NAME/SIGN/BAT%/AFC/BATV/BUZMUTE/VER/o` |
| `3` (settings reply, BLE form) | `3/TYPE/FREQ/OLED-SDA/OLED-SCL/OLED-RST/LED-PIN/RS41-BAND/M20-BAND/M10-BAND/PILOT-BAND/DFM-BAND/MYCALL/FREQ-OFS/BAT-PIN/BAT-MIN/BAT-MAX/BAT-TYPE/LCD-TYPE/NAME-TYPE/BUZ-PIN/VER/o` |

Field semantics: see MySondy Go API v3.0 PDF (URL in §11). `BUZMUTE=-1` always (no buzzer in v1).

**Input (app → device, all wrapped in `o{...}o`, slash-separated):**

| Command | Behaviour | FR |
|---|---|---|
| `f=<MHz>` | Set frequency, persist | FR-5.2 |
| `tipo=<1..5>` | Set sonde type, persist | FR-5.3 |
| `?` | Reply with frame `3` | FR-5.4 |
| `re` | Reboot | FR-5.5 |
| `Re` | Reset to defaults + reboot | FR-5.5 |
| `mute=<0|1>` | Stored only (no buzzer) | FR-5.6 |
| `myCall=<text>` | Persist (max 8 chars) | FR-5.7 |
| `aprsName=<0|1>` | Persist | FR-5.7 |
| `freqofs=<Hz>` | Persist + apply | FR-5.7 |
| `<sonde>.rxbw=<idx>` | Apply RX BW per Appendix 2 | FR-5.8 |
| `oled_*`, `led_pout`, `buz_pin`, `battery`, `lcd*`, `com`, `baud` | Accept-and-ignore (echoed in `3`) | FR-5.9 |
| `vBatMin=<mV>`, `vBatMax=<mV>`, `vBatType=<0|1|2>` | Honored — persisted, used for `BAT%` calculation | FR-5.11 |
| `sleep=<n>` | No-op + log | FR-5.10 |

### C. NVS schema

See §6.3.3.

### D. OLED layout (128×64)

```
┌────────────────────────────────────────┐
│ MySondyGo  RS41   404.600 MHz   📶 🔵│  Top bar: project, type, freq, WiFi/BLE
├────────────────────────────────────────┤
│ NAME   S1234567                        │
│ LAT    47.12345°    LON   8.54321°     │
│ ALT    12345 m      VV    -5.2 m/s     │
│                                        │
│ RSSI  ████░ -87 dBm     v1.0.0+abc     │  Bottom: signal, version
└────────────────────────────────────────┘
```

When `NO_SIGNAL`: replace decode rows with `── searching ──` plus rotating dot.

### E. Pin map (TTGO LoRa32 v2.1, 433 MHz variant)

| Function | GPIO |
|---|---|
| SX1276 SCK | 5 |
| SX1276 MISO | 19 |
| SX1276 MOSI | 27 |
| SX1276 NSS | 18 |
| SX1276 RST | 23 |
| SX1276 DIO0 | 26 |
| SX1276 DIO1 | 33 |
| SX1276 DIO2 | 32 |
| OLED SDA | 21 |
| OLED SCL | 22 |
| OLED RST | 16 |
| LED | 25 |
| Button (BOOT) | 0 |
| Battery sense | 35 (ADC1_CH7) |

### F. Reference URLs

- MySondy Go API v3.0 (2025-04-02): https://download.farenight.it/MySondyGoAPI_V3.pdf
- rs1729 decoder source: https://github.com/rs1729/RS (GPL-2.0)
- Cross-reference (do not import): https://github.com/dl9rdz/rdz_ttgo_sonde
- TTGO LoRa32 v2.1 (LilyGO): https://lilygo.cc/products/lora3 (vendor product page; schematic available separately)

### G. Embedded test library activation

This FSD activates the following standard ESP-IDF test specs from the FSD-writer skill:

- Captive Portal — AP-001..006, CP-001..006, TC-CP-100..102 (AP-only — no STA handover)
- BLE & NUS — BLE-001..006, BLE-010..013, BLE-020..023, BLE-030..032, TC-BLE-100..103, EC-BLE-200..204
- OTA — OTA-001..013, TC-OTA-100..102, EC-OTA-200..204
- NVS — NVS-001..024, TC-NVS-100..103, EC-NVS-200..204
- Watchdog — WDT-001..022, TC-WDT-100..102, EC-WDT-200..203
- Logging — LOG-001..026, TC-LOG-100..103, EC-LOG-200..204

Project-specific overrides: AP SSID format `MySondyGo-XXXX`, watchdog 60 s, heap 32 KB warning / 8 KB critical, BLE notification rate 1 Hz, lock-loss 30 s.

---

## 11. Related

- [[Balloon-Receiver Test Plan]] — detailed V&V procedures and benches (TBD).
- [[Balloon-Receiver BLE Cheatsheet]] — quick reference for the MySondy Go ASCII protocol (TBD).
- [[kytrack-map-fsd]] — sibling project: Pi-side dxlAPRS-based gateway. Balloon-Receiver is a complementary portable receiver.
- [[Claude_skill_fsd]] — meta-vault: skill specification driving FSD generation.
