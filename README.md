# bike_comm

Open-source motorcycle helmet communication system. Mesh voice between
riders plus Bluetooth Classic to a phone for calls and music.

Designed to be reproducible by anyone with a soldering iron and an order
at JLCPCB.

## Status

**v0 mesh-audio working on the bench, two boards.** Mic on board A,
mic on board B, both heard on the opposite board. The full chain runs
end-to-end: `i2s_channel_read` -> LC3 encode -> ESP-NOW broadcast ->
recv callback -> queue -> mixer JB -> LC3 decode -> `i2s_channel_write`.
Mesh discovery, slot claim, beacon (alternating with audio in the
coordinator's slot), and quiet-timeout failover are all implemented and
verified across tens of seconds of soak. No HFP/A2DP to the phone yet;
that's the v0.5 piece.

See [`docs/architecture.md`](docs/architecture.md) for the design,
[`docs/mesh_protocol.md`](docs/mesh_protocol.md) for the wire spec, and
[`docs/build_v0.md`](docs/build_v0.md) for the bench bring-up walkthrough.

## Hardware

- **MCU:** Original Espressif ESP32 (WROVER-E), single-chip. BT Classic
  plus ESP-NOW on one radio.
- **Dev board for v0:** ESP32-LyraT-Mini v1.2. ES8311 playback + ES7243
  mic ADC with the onboard MEMS on `AINRP/AINRN` (right channel).
- **Custom PCB target (v1):** ES8388 codec, INMP441 MEMS mic, MAX98357A
  I2S amp.

## Firmware

- ESP-IDF v5.5.4, FreeRTOS.
- `esp_codec_dev` managed component for ES8311 + ES7243 control.
- `google/liblc3` v1.1.1 (vendored) for the voice codec at 16 kHz / 10 ms
  / 32 kbps.
- Custom TDMA MAC over ESP-NOW for the mesh (20 ms superframe, 8 slots).
- Bluedroid for BT Classic HFP-HF + A2DP-sink (planned, not yet wired).
- ESP-SR AFE for AEC + NS + VAD (planned, not yet wired).

## Quick start

```sh
. ~/esp/esp-idf/export.sh
cd firmware/
idf.py set-target esp32
idf.py build
idf.py -p /dev/cu.usbserial-XXXX flash monitor
```

The long version, including the parts list and ESP-IDF install, is in
[`docs/build_v0.md`](docs/build_v0.md).

## Repository layout

```
docs/        architecture, mesh protocol spec, build guide, ADRs
firmware/    ESP-IDF project (main + vendored liblc3 + managed components)
hardware/    KiCad schematic + PCB, enclosure files (v1 placeholder)
tools/       helper scripts (PSK gen, future log parser, future flash)
```

## Roadmap

- **v0**  — 2 dev boards talking voice over ESP-NOW (no phone). 2 weeks.
- **v0.5** — 4-rider mesh + phone HFP/A2DP. +3 weeks.
- **v1**  — Custom PCB. +6 weeks.
- **v2**  — Field beta + helmet enclosure. +8 weeks.

## License

Apache-2.0. Hardware files released under
[CERN-OHL-P-2.0](https://ohwr.org/cern_ohl_p_v2.txt) (added when the
hardware files land).
