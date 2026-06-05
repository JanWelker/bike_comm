# LC3 codec cost on ESP32 LX6

Espressif's `esp_audio_codec` performance tables are measured on
ESP32-S3R8 (LX7 with PIE/vector ops) and **cannot** be transferred to
the original ESP32 (LX6) — the LX6 has no PIE, and Espressif's audio
DSP frequently leans on it. These are our own bench measurements from
the LyraT-Mini v1.2.

## Method

Wall-clock perf counters around every `lc3_encode` and `lc3_decode`
call in `firmware/main/codec_lc3.c` (see `codec_lc3_perf_log_and_reset`).
`esp_timer_get_time` snapshots µs before and after each codec call;
mean and max are logged every 1000 calls (~10 s) and the counters
reset. Numbers are from a two-board bench soak with continuous duplex
audio, no BT.

- Chip: Espressif ESP32-D0WD rev v3.1, dual-core LX6 @ 240 MHz, 8 MB
  PSRAM, 4 MB flash.
- Toolchain: ESP-IDF v5.5.4, optimized build (`-O3`, default IDF flags).
- Operating point: 16 kHz mono, 10 ms frames, **32 kbps → 40 B per frame**.
- liblc3 version: google/liblc3 v1.1.1 (vendored at
  `firmware/components/liblc3/upstream/`).
- audio_io task: core 1, prio 22, 7 KB stack.

## Numbers

Three consecutive 10 s windows on each board. B1 was coord (lowest MAC,
beaconing); B2 was joiner. Both boards encode at the full 100 fps mic
rate; B1 decodes ~100 fps real frames (no PLC), B2 decodes ~75 fps real
+ ~25 fps PLC because of the coord's beacon-slot-overlay asymmetry.

### Encode (`codec_lc3_encode`)

| Board | Sample 1 mean / max | Sample 2 mean / max | Sample 3 mean / max |
|---|---|---|---|
| B1 (coord) | 4022 / 5223 µs | 4103 / 5264 µs | 4124 / 5295 µs |
| B2 (joiner) | 4105 / 5519 µs | 4082 / 5472 µs | 4089 / 5457 µs |

**Encode is ~4.1 ms per 10 ms frame** = **41% of one core**, with
~30% jitter (max sits ~1.2 ms above mean). Stable across boards;
encoder state isn't sensitive to peer rate.

### Decode (`codec_lc3_decode`)

| Board | Sample 1 mean / max | Sample 2 mean / max | Sample 3 mean / max |
|---|---|---|---|
| B1 (coord, ~100% real) | 1297 / 1954 µs | 1327 / 2005 µs | 1341 / 1873 µs |
| B2 (joiner, ~75% real + 25% PLC) | 1067 / 1661 µs | 1072 / 1687 µs | 1073 / 1695 µs |

**Full decode is ~1.3 ms per 10 ms frame** = **13% of one core per
active decoder**. Mean on B2 is ~250 µs lower because PLC frames
(passed as `bytes=NULL`) are cheaper than real frames; back-of-envelope
that puts PLC at ~500 µs.

## What this means for budgeting

audio_io tick = 10 ms (mic DMA-gated). Per tick:

| Stage | Cost on LX6 |
|---|---|
| LC3 encode (always, 100 fps) | ~4.1 ms |
| LC3 decode (1 active rider) | ~1.3 ms |
| **Subtotal codec, 2-rider mesh** | **~5.4 ms / tick** |
| Everything else (mic+spk DMA setup, mesh queue+drain, mixer math, JB, log) | ~ remainder |
| **Total tick budget** | 10 ms |

So at two riders we sit around **54% of one core in the codec** alone.
Room above water but no enormous headroom.

### Scaling to 8 riders (worst case)

With **no VAD gating**, the mixer would decode all 8 incoming streams
every tick:

- 1 encode + 8 decodes × ~1.3 ms = **~14.5 ms** → over budget.

This is why VAD gating is non-negotiable for the v0.5 6-8 rider
target. With realistic call patterns (≤2 simultaneous active talkers)
the worst case is:

- 1 encode + 2 real decodes + 6 PLC decodes (assuming PLC ≈ 500 µs)
  ≈ 4.1 + 2.6 + 3.0 = **~9.7 ms** → still over budget.

With the mixer's current "skip decode when VAD inactive" path (which
contributes silence at zero cost):

- 1 encode + 2 real decodes ≈ **~6.7 ms** → fits comfortably.

**Implication:** before going past ~4 active riders, the mixer must
honour the VAD bit on the wire (it already does on the TX side; the
receive path delivers VAD-inactive frames as of commit `e0e1ac8`).

## Comparison hooks for future codec swaps

When evaluating an alternative codec (Codec2, Opus, G.722), the
target is to fit:

| Slot | Budget at 2-rider | Budget at 8-rider VAD-gated (2 active) |
|---|---|---|
| Encode | ≤4 ms | ≤4 ms |
| Decode per active rider | ≤1.5 ms | ≤1.5 ms |
| Total codec time per tick | ≤6 ms | ≤7 ms |

Anything that fits these holes is a viable LC3 substitute. The
research notes (`CLAUDE.md` → "Open work") flag Codec2 as the most
promising candidate — measure it the same way before swapping.

## Re-running the measurement

The counters live in `codec_lc3.c` and log via the audio_io task in
`audio_pipeline.c` (`codec_lc3_perf_log_and_reset` called every 1000
frames). Cost is one `esp_timer_get_time` pair per codec call (~1 µs)
— effectively free, so the instrumentation stays in tree.

```sh
idf.py -p /dev/cu.usbserial-XXX monitor 2>&1 | grep 'lc3: enc\|lc3: dec'
```
