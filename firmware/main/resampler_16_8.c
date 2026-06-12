/*
 * resampler_16_8 — see resampler_16_8.h.
 *
 * Filter coefficients are a 31-tap halfband Hamming-windowed sinc,
 * cutoff at fs/4 (= 4 kHz at fs=16 kHz), scaled to Q14. By halfband
 * design, every other tap (positions 1, 3, ..., 13, 17, ..., 29) is
 * exactly zero — the inner loop runs them anyway since the MAC cost
 * is negligible at 16 kHz output / 8 kHz output rate; promote to a
 * polyphase form if that ever shows up in `codec_perf_log_and_reset`.
 *
 * Tap values were computed via:
 *
 *     h_ideal[k] = 0.5 * sinc((k - 15) / 2)
 *     w[k]       = 0.54 - 0.46 * cos(2*pi*k / 30)
 *     h[k]       = h_ideal[k] * w[k]
 *     H[k]       = round(h[k] / sum(h) * 2^14)        // DC gain = 1 in Q14
 */

#include "resampler_16_8.h"

#define Q       14
#define ROUND   (1 << (Q - 1))

static const int16_t H[RESAMPLER_TAPS] = {
       -28,      0,     48,      0,   -110,      0,    231,      0,
      -439,      0,    804,      0,  -1588,      0,   5171,   8205,
      5171,      0,  -1588,      0,    804,      0,   -439,      0,
       231,      0,   -110,      0,     48,      0,    -28,
};

static inline int16_t sat16(int32_t v)
{
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

void resampler_decimate(resampler_state_t *r,
                        const int16_t *in, size_t n_in,
                        int16_t *out)
{
    /* Build a virtual stream: history (30) || input (n_in). For each
     * output sample n we sample input position 2n and convolve with H
     * centred there. The center tap H[15] sits at input position 2n,
     * so we read positions 2n+15 .. 2n-15. Indices in the virtual
     * stream: 30 + 2n + 15 - k for k = 0..30, i.e. 30+2n+15-k.
     *
     * Output count = n_in / 2 (assumed n_in even). */
    const size_t n_out = n_in / 2;
    for (size_t n = 0; n < n_out; ++n) {
        int32_t acc = ROUND;
        /* Center of the filter sits at virtual position 30 + 2n
         * (the next "new" sample), and the filter spans 31 taps
         * centred there → indices 30+2n-15 .. 30+2n+15.
         * = 2n+15 .. 2n+45 in the virtual stream. */
        int base = (int)(2 * n) + 15;   /* virtual stream index of H[0] */
        for (int k = 0; k < RESAMPLER_TAPS; ++k) {
            int vidx = base + k;
            int16_t s;
            if (vidx < RESAMPLER_HIST) {
                s = r->hist[vidx];
            } else {
                s = in[vidx - RESAMPLER_HIST];
            }
            acc += (int32_t)H[k] * (int32_t)s;
        }
        out[n] = sat16(acc >> Q);
    }

    /* Roll history forward: keep the last RESAMPLER_HIST input samples. */
    if (n_in >= RESAMPLER_HIST) {
        for (int i = 0; i < RESAMPLER_HIST; ++i) {
            r->hist[i] = in[n_in - RESAMPLER_HIST + i];
        }
    } else {
        /* Short call (shouldn't happen in our pipeline) — shift. */
        int keep = RESAMPLER_HIST - (int)n_in;
        for (int i = 0; i < keep; ++i) {
            r->hist[i] = r->hist[i + (int)n_in];
        }
        for (size_t i = 0; i < n_in; ++i) {
            r->hist[keep + i] = in[i];
        }
    }
}

void resampler_interpolate(resampler_state_t *r,
                           const int16_t *in, size_t n_in,
                           int16_t *out)
{
    /* Zero-stuff the input to 2*n_in and convolve. The virtual stream
     * holds history (RESAMPLER_HIST=30 stuffed samples) plus the
     * zero-stuffed input (2*n_in samples). For halfband filters this
     * collapses to two cheap polyphase branches — we just do direct
     * convolution for clarity.
     *
     * For each output sample m we read virtual positions m-30..m at
     * the centered-filter convention: out[m] = sum H[k] * stuffed[m+15-k]
     * but we choose to write out the simpler form: out is indexed by
     * a marching pointer that sees both stuffed positions. */
    const size_t n_out = 2 * n_in;

    /* History is stored in NON-stuffed form: the last RESAMPLER_HIST/2
     * + leftover INPUT samples. Easier: keep the history as the last
     * RESAMPLER_HIST input samples (8 kHz domain). When the filter
     * needs stuffed position p, the corresponding 8 kHz sample is at
     * floor(p/2) when p is even; when p is odd the stuffed value is
     * exactly zero. */
    /* On halfband, only the centre tap multiplies zero-stuffed odd
     * positions to zero — except the centre H[15] sits at the input
     * sample. Even-tap multiplications dominate. */
    for (size_t m = 0; m < n_out; ++m) {
        int32_t acc = ROUND;
        for (int k = 0; k < RESAMPLER_TAPS; ++k) {
            int p = (int)m + 15 - k;   /* virtual stuffed position */
            if ((p & 1) != 0) {
                /* odd stuffed position → x_stuffed[p] = 0 */
                continue;
            }
            int q = p >> 1;   /* corresponding 8 kHz input index */
            int16_t s;
            if (q < 0) {
                int hidx = q + RESAMPLER_HIST;
                if (hidx < 0) continue;
                s = r->hist[hidx];
            } else if ((size_t)q < n_in) {
                s = in[q];
            } else {
                continue;
            }
            acc += (int32_t)H[k] * (int32_t)s;
        }
        /* Interpolator gain doubles in passband (zero-stuffing halves
         * average energy; filter recovers it by 2x). Compensate by
         * shifting one less. */
        out[m] = sat16((acc << 1) >> Q);
    }

    /* Roll history forward in 8 kHz domain. */
    if (n_in >= RESAMPLER_HIST) {
        for (int i = 0; i < RESAMPLER_HIST; ++i) {
            r->hist[i] = in[n_in - RESAMPLER_HIST + i];
        }
    } else {
        int keep = RESAMPLER_HIST - (int)n_in;
        for (int i = 0; i < keep; ++i) {
            r->hist[i] = r->hist[i + (int)n_in];
        }
        for (size_t i = 0; i < n_in; ++i) {
            r->hist[keep + i] = in[i];
        }
    }
}
