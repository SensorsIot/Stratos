# Notice

Stratos firmware is licensed under **GPL-2.0** (see [LICENSE](LICENSE)).

The choice of GPL-2.0 is dictated by the licenses of the third-party works
this project incorporates or derives from, listed below.

## Derivative work

### rs1729/RS — radiosonde demodulators and frame parsers

- **Upstream:** <https://github.com/rs1729/RS>
- **License:** GPL-2.0 (or any later version)
- **Author:** rs1729

`rs1729/RS` is the canonical open-source collection of radiosonde
demodulators and frame parsers (Vaisala RS41, Graw DFM, Meteomodem M10/M20,
Pilot, RS92, iMet, MRZ, Meisei, LMS6 …). Most open-source radiosonde
receivers — including Stratos — derive their demodulator and frame-parser
algorithms from this work.

Stratos vendors the RS41 demodulator and frame parser from `rs1729/RS`
inside `components/decoder_rs41/`. Source files derived from `rs1729/RS`
carry an SPDX header naming `rs1729/RS` as the upstream and stating the
GPL-2.0 license. As long as Stratos contains any code derived from
`rs1729/RS`, the combined work must be distributed under GPL-2.0 or a
later GPL version.

Stratos extends `rs1729/RS` only inside the `decoder_rs41` component;
the rest of the firmware is original code written for this project but
released under the same license to keep the combined work coherent.

## Wire-protocol compatibility

### MySondyGo API v3.0 — BLE ASCII frame format

- **Upstream specification:** <https://download.farenight.it/MySondyGoAPI_V3.pdf>
- **Author:** Mirko Dalmonte (IZ4PNN)
- **Relationship to Stratos:** Stratos's BLE payload format is wire-compatible
  with the MySondyGo API v3.0 ASCII protocol. No code is copied from any
  MySondyGo implementation — the protocol is implemented from the published
  specification under the project's own GPL-2.0 license. Wire compatibility
  lets MySondyGo-aware client apps (BalloonHunter included) connect to
  Stratos without modification.

## Cross-references and acknowledgements

These projects are referenced for hardware quirks, protocol corroboration,
or empirical RF parameter values, but no code is copied from them.

| Project | Use |
|---|---|
| [`dl9rdz/rdz_ttgo_sonde`](https://github.com/dl9rdz/rdz_ttgo_sonde) | TTGO LoRa32 board quirks, SX1276 RS41 sync word and FSK profile values |
| [BalloonHunter](https://github.com/SensorsIot/BalloonHunter) | Companion iOS/Android app — Stratos's primary BLE consumer |
| [SondeHub](https://sondehub.org) | Global radiosonde tracking network — referenced in docs |

## Reporting an issue with attribution

If you believe an attribution here is incorrect, incomplete, or missing
for code you authored, please open an issue at
<https://github.com/SensorsIot/Stratos/issues>.
