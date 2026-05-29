# Architecture

Condensed from the planning doc. The full version lives at `~/.claude/plans/this-is-a-new-purring-storm.md` and will be folded in.

## Hardware path (single-chip)

```
INMP441 (I2S mic) --> ESP32 WROVER-E --> ES8388 (I2S codec) --> MAX98357A --> helmet speakers
                          ^         |
                          | one 2.4 GHz radio shared via PTA:
                          |   - Bluedroid: HFP-HF + A2DP-sink to the phone
                          |   - ESP-NOW:   TDMA mesh to other riders
                          |
                       LiPo + TP4056 charge, USB-C, 3 buttons, RGB LED
```

The single-radio coexistence between BT Classic and ESP-NOW is the central risk — see `firmware/main/coex.c` and the *Coexistence Strategy* section of the plan.

## Firmware layers

```
App / UX          buttons, LED, NVS, OTA
Session FSM       intercom ↔ phone-call arbitration
Mixer + JB        N jitter buffers, decode pool, sum to speaker + AEC ref
LC3 codec         google/liblc3 — 24 kbps, 10 ms, 16 kHz
ESP-SR AFE        AEC + NS + VAD
I2S audio I/O     INMP441 in, ES8388 out
Transport         ESP-NOW MAC (TDMA), Bluedroid HFP+A2DP
Platform          FreeRTOS, NVS, coex
```

## Audio constants

| | |
|---|---|
| Sample rate | 16 kHz mono |
| Frame size | 10 ms = 160 samples |
| LC3 bitrate | 24 kbps -> 30 B/frame |
| Mouth-to-ear target | ≤ 200 ms |
| AEC reference | mixer output (not just phone audio) |

## Modules

| File | Role |
|---|---|
| `main.c` | Platform init, module wiring |
| `audio_pipeline.{c,h}` | I2S DMA + ESP-SR AFE |
| `codec_lc3.{c,h}` | liblc3 wrapper, decoder pool |
| `mesh_mac.{c,h}` | TDMA on ESP-NOW; see `mesh_protocol.md` |
| `mixer.{c,h}` | Jitter buffers + summing mixer |
| `bt_classic.{c,h}` | Bluedroid HFP-HF + A2DP-sink |
| `session_fsm.{c,h}` | Arbitration policy |
| `ui.{c,h}` | Buttons, LED, battery |
| `nvs_cfg.{c,h}` | Persistent settings |
| `coex.{c,h}` | Radio coex tuning |

## CPU budgeting (original ESP32, no PIE vector ISA)

| | Core 1 |
|---|---|
| ESP-SR AFE | 30–40% |
| LC3 N decoders (VAD-gated, ~2 active) | 10–15% |
| Mixer + I2S | ~10% |
| Headroom | ~35% |

Without VAD gating, 7 simultaneous decoders push worst-case to ~85%. **VAD gating is not optional.**

## See also

- `mesh_protocol.md` — frame format + TDMA spec
- `build_v0.md` — bring-up on LyraT-Mini dev boards
- `decisions/` — ADRs for big calls (MCU, codec, architecture)
