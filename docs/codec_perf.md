# Codec cost on ESP32 LX6

Sections: [LC3](#lc3-numbers) (the daily driver) and
[Codec2 mode 3200](#codec2-mode-3200-numbers) (build-time alternate).

## Why this doc exists

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
- audio_io task: core 1, prio 22, 7 KB stack (LC3 build).

## LC3 numbers

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

### WebRTC noise suppression (`WebRtcNs_Analyze` + `WebRtcNs_Process`)

After landing `cpuimage/WebRTC_NS` (mode = medium, 16 kHz, 10 ms /
160-sample frames). Same instrumentation pattern in audio_pipeline.c
— `esp_timer_get_time` around the NS calls, logged every 1000 frames.

Process-only numbers (original measurement — NOTE: this configuration
turned out to be broken; see below):

| Board | Sample 1 mean / max | Sample 2 mean / max | Sample 3 mean / max |
|---|---|---|---|
| B1 (coord) | 1474 / 2394 µs | 1464 / 2305 µs | 1454 / 2425 µs |
| B2 (joiner) | 1584 / 3262 µs | 1573 / 3143 µs | 1582 / 3292 µs |

**Process-only is ~1.5 ms per 10 ms frame**, but Process alone never
updates the speech-probability model (`priorSpeechProb` only updates
inside `AnalyzeCore`), which left the VAD gate stuck shut — see the
2026-06-12 entry in CLAUDE.md "Open work". `WebRtcNs_Analyze` must
also run; every frame it costs ~2.6 ms extra (combined mean ~4.1 ms,
max ~18 ms, pushing the speech-period tick to ~11.5 ms — over
budget).

Shipping configuration: **Analyze every 2nd frame + Process every
frame**, measured 2026-06-12 under sustained speech:

| Board | Combined NS mean / max (per tick, averaged) |
|---|---|
| B1/B2 | 2450-2718 / 5585-7183 µs |

**NS is ~2.6 ms per 10 ms frame** = **26% of one core**. The
probability model still updates 50x/s — orders of magnitude faster
than the 500 ms VAD hold — and the noise estimate Process consumes
lags at most one frame.

Note: LC3 encode and decode means crept up slightly once NS was
co-resident (encode ~4.1 → 4.5 ms, decode ~1.3 → 1.4 ms on B1). Most
of that is cache pressure — the NS state lives in PSRAM (~25 KB,
allocated above the 16 KB internal-RAM threshold), and the audio_io
task now interleaves PSRAM access with the LC3 codec state in
internal RAM. PSRAM cache misses don't show up in this counter but
do nudge the surrounding work.

## What this means for budgeting

audio_io tick = 10 ms (mic DMA-gated). Per tick, with NS in the chain,
during speech (the worst case — VAD-inactive ticks skip the encode
entirely, saving the full ~4.5 ms):

| Stage | Cost on LX6 |
|---|---|
| WebRTC NS (Analyze/2 + Process, 100 fps) | ~2.6 ms |
| LC3 encode (speech only, 100 fps)        | ~4.5 ms |
| LC3 decode (1 active rider)              | ~1.4 ms |
| **Subtotal DSP+codec, 2-rider mesh**     | **~8.5 ms / tick** |
| Everything else (mic+spk DMA setup, mesh queue+drain, mixer math, JB, log) | ~ remainder |
| **Total tick budget**                    | 10 ms |

So at two riders we sit around **~85% of one core** in NS + codec
combined while talking (measured 2026-06-12: tick ~10.0-10.15 ms
under sustained speech; the I2S DMA backlog absorbs the residual
percent and recovers in speech gaps). Quiet ticks run ~4 ms. There is
NO headroom for another DSP block on core 1 during speech — AEC/AGC
work must live on the other core or displace something.

### Scaling to 8 riders (worst case)

With NS in the chain and **no VAD gating** (the mixer would decode all
8 incoming streams every tick):

- NS + 1 encode + 8 decodes × ~1.4 ms = 2.6 + 4.5 + 11.2 = **~18.3 ms**
  → over budget.

This is why VAD gating is non-negotiable for the v0.5 6-8 rider
target. With realistic call patterns (≤2 simultaneous active talkers)
the worst case is:

- NS + 1 encode + 2 real decodes + 6 PLC decodes (PLC ≈ 500 µs)
  ≈ 2.6 + 4.5 + 2.8 + 3.0 = **~12.9 ms** → over budget. (Mitigated:
  the mixer now caps PLC at 5 consecutive underruns per rider, so
  silent/departed riders cost nothing in steady state.)

With the mixer's "skip decode when VAD inactive" path (which
contributes silence at zero cost):

- NS + 1 encode + 2 real decodes ≈ 2.6 + 4.5 + 2.8 = **~9.9 ms** →
  fits with no slack; one more simultaneous talker blows it.

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

Anything that fits these holes is a viable LC3 substitute. Codec2
mode 3200 was measured the same way — see below.

## Codec2 mode 3200 numbers

Bench-validated 2026-06-12 on the same two-board LyraT-Mini setup,
codec2 selected via `CONFIG_BIKE_CODEC_CODEC2=y`. All codec2 state
(encoder + 8 decoders) lives in PSRAM via the `__EMBEDDED__` hook
in `firmware/components/codec2/codec2_alloc.c` (otherwise the many
small mallocs stay in internal DRAM under the 16 KB
`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` threshold and starve mbedtls).
The audio_loopback task stack is bumped 7 KB → 24 KB on this build —
codec2's NLP/FFT/quantise paths have stack-allocated float arrays
that overflow 16 KB on the decode side; 32 KB exceeded the largest
contiguous internal-DRAM block and `xTaskCreatePinnedToCoreWithCaps
(... MALLOC_CAP_SPIRAM)` fails `xPortcheckValidStackMem` (PSRAM
stacks not supported on the LX6 Xtensa port).

Operating point: 8 kHz internal (codec2 native), 16 kHz audio
pipeline via a 31-tap halfband resampler (`firmware/main/
resampler_16_8.c`), one codec2 frame per 20 ms = 8 B → zero-padded
into the 40 B wire slot every other tick.

### Encode + Decode (per call, sustained speech, both boards)

| Stage | Mean | Max |
|---|---|---|
| `codec2_encode` (per 20 ms frame) | ~12 ms | ~16 ms |
| `codec2_decode` (per 20 ms frame) | ~13 ms | ~16 ms |
| WebRTC NS (Analyze/2 + Process, 100 fps) | ~2 ms | ~7 ms |

Decode max shows a warmup tail: 30-40 ms outliers in the first
window or two after the first frames arrive (PSRAM cache cold);
steady state settles at ~13-16 ms.

### What this means for budgeting

audio_loopback tick = 10 ms. Per tick during speech:

- Worst combined: NS (~2) + codec2_encode (~12-16) + codec2_decode
  (~12-16) = **~26-34 ms / tick**, ~3× over budget.
- `task_wdt` fires periodically on Core 1 (warning, not fatal —
  audio still flows, but IDLE on Core 1 can't pet the watchdog).

Audio is intelligible end-to-end (peak speaker amplitudes 10 K+
during speech, codec2's vocoder character clearly audible), so the
codec layer works correctly. The DSP cost is what knocks it out:
codec2 is ~25× LC3 on the LX6.

### Verdict

**Codec2 mode 3200 is functional but not viable as a primary codec
on the LX6.** Kept in tree as scaffolding behind
`CONFIG_BIKE_CODEC_CODEC2` for the move-to-S3 decision. To make it
viable on the LX6 would need: hot loops in IRAM (cache-resident
FFT/quant), encode/decode split across the two cores via a worker
task, or PSRAM-resident state keyed to cache-line alignment plus
the LX7+PIE vector ISA (so: ESP32-S3R8). Espressif's own LC3
numbers on S3R8 suggest a ~5× speedup for the FFT-heavy paths;
codec2 should see similar gains.

## Re-running the measurement

The counters live in `codec_lc3.c` / `codec_codec2.c` (codec) and
`audio_pipeline.c` (NS) and log via the audio_io task every 1000
frames (`codec_perf_log_and_reset` + the NS log block right after it).
Cost is one `esp_timer_get_time` pair per codec/NS call (~1 µs each)
— effectively free, so the instrumentation stays in tree.

```sh
idf.py -p /dev/cu.usbserial-XXX monitor 2>&1 \
    | grep -E 'lc3: enc|lc3: dec|codec2: enc|codec2: dec|audio: ns'
```
