/*
 * resampler_16_8 — 2:1 anti-aliased halfband FIR resampler between
 * 16 kHz and 8 kHz mono PCM.
 *
 * Used by the codec2 backend to bridge audio_pipeline.c's 16 kHz tick
 * and codec2's 8 kHz speech assumption. A halfband Hamming-windowed
 * sinc (31 taps, Q14 fixed point) gives ~40 dB stopband attenuation
 * above 4.6 kHz — well below 16 kHz audible artefacts and well above
 * the 3 kHz upper edge of speech intelligibility.
 *
 * State is a 30-sample history per direction per channel; carry across
 * calls so the filter wraps continuously over consecutive 20 ms ticks.
 *
 * No allocation. No FPU. Inner loop is ~31 MACs per output sample;
 * total cost at 16 kHz input / 8 kHz output is well under 1 % of
 * one LX6 core, so we leave the polyphase optimisation for later
 * even though half the taps are zero by halfband design.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

#define RESAMPLER_TAPS    31
#define RESAMPLER_HIST    (RESAMPLER_TAPS - 1)   /* = 30 */

typedef struct {
    int16_t hist[RESAMPLER_HIST];   /* last RESAMPLER_HIST input samples */
} resampler_state_t;

static inline void resampler_init(resampler_state_t *r)
{
    for (int i = 0; i < RESAMPLER_HIST; ++i) r->hist[i] = 0;
}

/* Decimate `n_in` 16 kHz samples to `n_in/2` 8 kHz samples.
 * Requires n_in even. */
void resampler_decimate(resampler_state_t *r,
                        const int16_t *in, size_t n_in,
                        int16_t *out);

/* Interpolate `n_in` 8 kHz samples to `2*n_in` 16 kHz samples. */
void resampler_interpolate(resampler_state_t *r,
                           const int16_t *in, size_t n_in,
                           int16_t *out);
