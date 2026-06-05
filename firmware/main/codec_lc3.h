/*
 * codec_lc3 — wraps google/liblc3 for 16 kHz / 10 ms / 32 kbps voice.
 *
 * One shared encoder; one decoder instance per active remote rider
 * (allocated lazily from a pool when the rider first transmits a
 * VAD-active frame, freed on a quiet-timeout).
 *
 * Frame sizes:
 *   - PCM: 160 int16_t  (10 ms @ 16 kHz)
 *   - LC3: 30 bytes     (24 kbps)
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#define LC3_BITRATE_BPS    32000
#define LC3_FRAME_DUR_US   10000
#define LC3_SAMPLE_RATE_HZ 16000
#define LC3_FRAME_BYTES    40        /* derived from bitrate and frame duration */
#define LC3_PCM_SAMPLES    160       /* 10 ms @ 16 kHz */
#define LC3_MAX_DECODERS   8         /* one per rider in an 8-rider mesh */

esp_err_t codec_lc3_init(void);

/* Encode one 10 ms PCM frame; returns bytes written to out (= LC3_FRAME_BYTES). */
size_t    codec_lc3_encode(const int16_t pcm[160], uint8_t out[LC3_FRAME_BYTES]);

/* Allocate/release a decoder slot for a remote rider. */
esp_err_t codec_lc3_decoder_acquire(uint8_t rider_id);
void      codec_lc3_decoder_release(uint8_t rider_id);

/* Decode one frame for the given rider. If bytes is NULL the decoder runs
 * its packet-loss concealment to produce a substitute frame. */
size_t    codec_lc3_decode(uint8_t rider_id,
                           const uint8_t *bytes, size_t len,
                           int16_t out_pcm[160]);
