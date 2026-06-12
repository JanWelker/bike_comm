/*
 * codec — voice codec abstraction.
 *
 * One public API; the build picks an implementation via Kconfig:
 *
 *   CONFIG_BIKE_CODEC_LC3      → codec_lc3.c     (google/liblc3, default)
 *   CONFIG_BIKE_CODEC_CODEC2   → codec_codec2.c  (drowe67/codec2 3200 bps)
 *
 * The two backends share the same on-wire and PCM frame shapes — the
 * mesh protocol's slot still carries CODEC_FRAME_BYTES (40 B), the
 * audio pipeline still ticks every CODEC_FRAME_SAMPLES (160 = 10 ms
 * @ 16 kHz). codec2 produces 8 B per 20 ms internally; the codec2
 * backend buffers two ticks of PCM, emits the 8 B encoded payload
 * zero-padded into the 40 B wire slot every other tick, and returns
 * zero bytes on intermediate ticks (caller treats that as "no audio
 * this tick" and uses PLC at the receiver). Keeps the protocol
 * unchanged at the cost of ~64 B wasted per packet — acceptable for
 * an A/B alternate; the next iteration would shrink the wire slot.
 *
 * Both backends pre-allocate everything in codec_init() — no malloc
 * in the hot encode/decode paths and none in the ESP-NOW recv path
 * (see CLAUDE.md "ESP-NOW recv callback cannot block or allocate").
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#define CODEC_FRAME_DUR_US     10000
#define CODEC_SAMPLE_RATE_HZ   16000
#define CODEC_FRAME_BYTES      40        /* wire slot — both backends pad/fit */
#define CODEC_FRAME_SAMPLES    160       /* 10 ms @ 16 kHz */
#define CODEC_MAX_DECODERS     8         /* one per rider in an 8-rider mesh */

esp_err_t codec_init(void);

/* Encode one CODEC_FRAME_SAMPLES PCM frame. Returns bytes written to
 * `out` (typically CODEC_FRAME_BYTES; the codec2 backend may return 0
 * on intermediate ticks while it buffers a 20 ms window). */
size_t    codec_encode(const int16_t pcm[CODEC_FRAME_SAMPLES],
                       uint8_t out[CODEC_FRAME_BYTES]);

/* Decode one frame for the given rider. If `bytes` is NULL the decoder
 * runs its packet-loss concealment to produce a substitute frame. */
size_t    codec_decode(uint8_t rider_id,
                       const uint8_t *bytes, size_t len,
                       int16_t out_pcm[CODEC_FRAME_SAMPLES]);

/* Log accumulated encode/decode wall-clock stats (mean / max in us) and
 * reset the counters. Intended to be called periodically (e.g. once
 * per 10 s) from the audio_io task. Pure diagnostic; the LC3 numbers
 * captured this way live in docs/codec_perf.md. */
void      codec_perf_log_and_reset(void);
