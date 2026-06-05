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

### WebRTC noise suppression (`WebRtcNs_Process`)

After landing `cpuimage/WebRTC_NS` (mode = medium, 16 kHz, 10 ms /
160-sample frames). Same instrumentation pattern in audio_pipeline.c
— `esp_timer_get_time` around every `WebRtcNs_Process` call, logged
every 1000 frames.

| Board | Sample 1 mean / max | Sample 2 mean / max | Sample 3 mean / max |
|---|---|---|---|
| B1 (coord) | 1474 / 2394 µs | 1464 / 2305 µs | 1454 / 2425 µs |
| B2 (joiner) | 1584 / 3262 µs | 1573 / 3143 µs | 1582 / 3292 µs |

**NS is ~1.5 ms per 10 ms frame** = **15% of one core**. B2's max sits
higher than B1's — likely the ESP-NOW RX interrupts disturbing wall-
clock more on the joiner side (it receives slightly different traffic
patterns than the coord). The mean is stable within ~10% across both
boards.

Note: LC3 encode and decode means crept up slightly once NS was
co-resident (encode ~4.1 → 4.5 ms, decode ~1.3 → 1.4 ms on B1). Most
of that is cache pressure — the NS state lives in PSRAM (~25 KB,
allocated above the 16 KB internal-RAM threshold), and the audio_io
task now interleaves PSRAM access with the LC3 codec state in
internal RAM. PSRAM cache misses don't show up in this counter but
do nudge the surrounding work.

## What this means for budgeting

audio_io tick = 10 ms (mic DMA-gated). Per tick, with NS in the chain:

| Stage | Cost on LX6 |
|---|---|
| WebRTC NS (always, 100 fps)             | ~1.5 ms |
| LC3 encode (always, 100 fps)            | ~4.5 ms |
| LC3 decode (1 active rider)             | ~1.4 ms |
| **Subtotal DSP+codec, 2-rider mesh**    | **~7.4 ms / tick** |
| Everything else (mic+spk DMA setup, mesh queue+drain, mixer math, JB, log) | ~ remainder |
| **Total tick budget**                   | 10 ms |

So at two riders we sit around **~74% of one core** in NS + codec
combined. Headroom is real but not generous — the 10 ms tick can't
absorb another comparable DSP block without restructuring (e.g.
moving NS to its own task on the other core, or trimming the LC3
encode cost).

### Scaling to 8 riders (worst case)

With NS in the chain and **no VAD gating** (the mixer would decode all
8 incoming streams every tick):

- NS + 1 encode + 8 decodes × ~1.4 ms = 1.5 + 4.5 + 11.2 = **~17.2 ms**
  → over budget.

This is why VAD gating is non-negotiable for the v0.5 6-8 rider
target. With realistic call patterns (≤2 simultaneous active talkers)
the worst case is:

- NS + 1 encode + 2 real decodes + 6 PLC decodes (PLC ≈ 500 µs)
  ≈ 1.5 + 4.5 + 2.8 + 3.0 = **~11.8 ms** → still over budget.

With the mixer's current "skip decode when VAD inactive" path (which
contributes silence at zero cost):

- NS + 1 encode + 2 real decodes ≈ 1.5 + 4.5 + 2.8 = **~8.8 ms** →
  fits, just barely.

**Implication:** before going past ~4 active riders, the mixer must
honour the VAD bit on the wire (it already does on the TX side; the
receive path delivers VAD-inactive frames as of commit `e0e1ac8`).
And with NS landed, the 8-rider VAD-gated ceiling is tight enough
that the next sensible step is moving NS to its own task on core 0 so
the audio_io budget no longer absorbs it.

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

The counters live in `codec_lc3.c` (LC3) and `audio_pipeline.c` (NS)
and log via the audio_io task every 1000 frames
(`codec_lc3_perf_log_and_reset` + the NS log block right after it).
Cost is one `esp_timer_get_time` pair per codec/NS call (~1 µs each)
— effectively free, so the instrumentation stays in tree.

```sh
idf.py -p /dev/cu.usbserial-XXX monitor 2>&1 \
    | grep -E 'lc3: enc|lc3: dec|audio: ns'
```
