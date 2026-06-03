# bike_comm

Open-source motorcycle helmet communication system. Mesh voice between riders + Bluetooth Classic to a phone for calls and music.

In the spirit of the commercial alternatives, but designed to be reproducible by anyone with a soldering iron and an order at JLCPCB.

## Status

**v0 mesh-audio working on the bench, two boards.** Mic on board A,
mic on board B, both heard on the opposite board. Plumbing end-to-end
from `i2s_channel_read` -> LC3 encode -> ESP-NOW broadcast ->
ESP-NOW recv callback -> queue -> mixer JB -> LC3 decode ->
`i2s_channel_write`. Mesh discovery, slot claim, beacon (alternating
with audio in the coordinator's slot), and quiet-timeout failover all
implemented and verified across tens of seconds of soak. No HFP/A2DP
to the phone yet; that's the v0.5 piece.

See [`docs/architecture.md`](docs/architecture.md) for the design and
[`docs/mesh_protocol.md`](docs/mesh_protocol.md) for the wire spec.

## Hardware

- **MCU:** Original Espressif ESP32 (WROVER-E), single-chip. BT Classic + ESP-NOW on one radio.
- **Dev board for v0:** ESP32-LyraT-Mini v1.2. ES8311 playback + ES7243 mic ADC + onboard MEMS on AINRP/AINRN.
- **Custom PCB target:** ES8388 codec, INMP441 MEMS mic, MAX98357A I2S amp (planned for v1).

## Firmware

- ESP-IDF v5.5, FreeRTOS.
- ESP-ADF for audio pipeline, ESP-SR for AEC/NS/VAD.
- Bluedroid for BT Classic (HFP-HF + A2DP-sink).
- Custom TDMA MAC over ESP-NOW for the mesh; google/liblc3 for the codec.

## Repository layout

```
docs/        architecture, mesh protocol spec, build guides, ADRs
firmware/    ESP-IDF project (main + components)
hardware/    KiCad schematic + PCB, enclosure files
tools/       helper scripts (PSK gen, log parser, flash)
```

## Roadmap

- **v0**  — 2 dev boards talking voice over ESP-NOW (no phone). 2 weeks.
- **v0.5** — 4-rider mesh + phone HFP/A2DP. +3 weeks.
- **v1**  — Custom PCB. +6 weeks.
- **v2**  — Field beta + helmet enclosure. +8 weeks.

## License

Apache-2.0. Hardware files released under [CERN-OHL-P-2.0](https://ohwr.org/cern_ohl_p_v2.txt) (added when hardware files land).
