/*
 * resampler_48_16 — 3:1 decimator, 48 kHz mono S16 -> 16 kHz mono S16.
 *
 * Used by bt_a2dp.c to bring SBC-decoded music down to the mixer's
 * 16 kHz tick. The previous bt_a2dp box-filter (3-tap MA) had only
 * ~13 dB stopband which audibly aliased on bright music; this
 * Hamming-windowed sinc gives ~40+ dB and a clean 8 kHz cutoff.
 *
 * Implementation is a sample-by-sample delay-line FIR: each input
 * sample shifts into a 31-sample circular buffer, every third input
 * triggers an output computation. This avoids the past-end input read
 * the virtual-stream form has (resampler_16_8 inherits that latent
 * bug; this module sidesteps it). Continuity across calls is exact
 * regardless of input chunking — the input phase counter is part of
 * the state.
 *
 * Frame model: caller passes n_in mono S16 samples at 48 kHz. The
 * function returns the actual output count (≈ n_in/3 but exact value
 * depends on the phase state from the previous call).
 *
 * Cost: 31 MACs per output sample = ~5500 MACs/sec at 16 kHz output.
 * Polyphase would skip the 10 zero taps for ~30 % saving; not needed
 * at this rate.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

#define R48_16_TAPS  31

typedef struct {
    int16_t dline[R48_16_TAPS]; /* circular delay line */
    uint8_t write_idx;          /* where next sample is written (oldest) */
    uint8_t phase;              /* 0..2, inputs since last output */
} resampler_48_16_state_t;

/* Decimate n_in mono S16 samples at 48 kHz into 16 kHz mono S16 in
 * `out` (max output capacity = n_in/3 + 1). Returns the actual output
 * count written. State persists across calls so frame boundaries
 * don't glitch and the cumulative input/output rate is exactly 3:1. */
size_t resampler_48_16_decimate(resampler_48_16_state_t *r,
                                const int16_t *in, size_t n_in,
                                int16_t *out);
