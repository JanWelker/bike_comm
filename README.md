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
- Noise suppression via vendored `cpuimage/WebRTC_NS` (planned) — the
  precompiled `espressif/esp-sr` AFE shipped for the original ESP32 has
  a runtime heap-check that crashes on this chip, so we're taking the
  WebRTC source-tree route instead.

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

- **v0**  — 2 dev boards talking voice over ESP-NOW (no phone). Done.
- **v0.5** — 4-8 rider mesh + phone HFP/A2DP + noise suppression. The
  concrete backlog (NS via WebRTC_NS, mesh time-sync via the
  forward-only-slew algorithm from ESPNowMeshClock, optional Codec2
  alt codec, BTstack-vs-Bluedroid bench eval) lives in `CLAUDE.md`
  under "Open work."
- **v1**  — Custom PCB on ESP32-S3 (PIE vector ops let us run the
  full ESP-SR AFE pipeline that can't fit on the LX6).
- **v2**  — Field beta + helmet enclosure.

## Related open-source work

bike_comm draws on, and complements, the existing ESP32-voice
ecosystem. None of these are direct substitutes — they tend to be
half-duplex PTT or star topologies, not continuous full-duplex
TDMA mesh — but they're the closest prior art and worth a look:

- [google/liblc3](https://github.com/google/liblc3) — the voice
  codec we use, vendored at v1.1.1.
- [atomic14/esp32-walkie-talkie](https://github.com/atomic14/esp32-walkie-talkie)
  — selectable UDP / ESP-NOW transport, half-duplex PTT.
- [sh123/esp32_loradv](https://github.com/sh123/esp32_loradv)
  — runtime-selectable Codec2 / Opus on plain WROOM32 LX6; proves
  both codecs fit on our MCU class.
- [M17 protocol](https://m17project.org/about/) +
  [onemikedelta/M17-ESP32](https://github.com/onemikedelta/M17-ESP32)
  — Codec2-based open digital radio, frame layout we'd borrow if
  we add Codec2 as an alternate codec.
- [tanakamasayuki/PCMFlowG722](https://github.com/tanakamasayuki/PCMFlowG722)
  — G.722 over ESP-NOW; useful as a small/cheap wideband fallback
  if LC3 ever pinches.
- [cpuimage/WebRTC_NS](https://github.com/cpuimage/WebRTC_NS) and
  [rjsachse/ESP32-SpeexDSP](https://github.com/rjsachse/ESP32-SpeexDSP)
  — the two known-good vendorable DSP trees (NS, AEC, AGC, VAD)
  for the LX6 since `espressif/esp-sr` doesn't work here.

## License

Apache-2.0. Hardware files released under
[CERN-OHL-P-2.0](https://ohwr.org/cern_ohl_p_v2.txt) (added when the
hardware files land).
