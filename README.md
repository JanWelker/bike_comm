# bike_comm

Open-source motorcycle helmet communication system. Mesh voice between riders + Bluetooth Classic to a phone for calls and music.

In the spirit of the commercial alternatives, but designed to be reproducible by anyone with a soldering iron and an order at JLCPCB.

## Status

**Pre-v0.** Architecture is locked; firmware skeleton in place; waiting on first dev boards.

See [`docs/architecture.md`](docs/architecture.md) for the design. The full planning document is at `~/.claude/plans/this-is-a-new-purring-storm.md` for now and will be folded in.

## Hardware

- **MCU:** Original Espressif ESP32 (WROVER-E or WROVER-IE), single-chip. BT Classic + ESP-NOW on one radio.
- **Audio:** ES8388 codec, INMP441 MEMS mic, MAX98357A I2S amp.
- **Dev board for bring-up:** ESP32-LyraT-Mini (has the whole audio chain integrated).

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
