/*
 * resampler_48_16 — see resampler_48_16.h.
 *
 * Filter coefficients are a 31-tap Hamming-windowed sinc, cutoff at
 * fs/6 = 8 kHz (= half the output Nyquist), scaled to Q14. By design
 * 10 of the 31 taps are exactly zero (sinc zero crossings aligned
 * with input grid at every 3rd position from center); the inner loop
 * runs them anyway since the MAC cost is negligible. Promote to
 * polyphase if it ever shows up in a perf log.
 *
 * Taps were generated with:
 *
 *     h_ideal[k] = (1/3) * sinc((k - 15) / 3)
 *     w[k]       = 0.54 - 0.46 * cos(2*pi*k / 30)
 *     h[k]       = h_ideal[k] * w[k]
 *     H[k]       = round(h[k] / sum(h) * 2^14)        // DC gain = 1 in Q14
 *
 * sum(H) = 16385; the +1 LSB rounding error is masked by Q14 quant.
 */

#include "resampler_48_16.h"

#define Q       14
#define ROUND   (1 << (Q - 1))

static const int16_t H[R48_16_TAPS] = {
         0,     29,     42,      0,    -95,   -140,      0,    277,
       379,      0,   -694,   -955,      0,   2164,   4461,   5449,
      4461,   2164,      0,   -955,   -694,      0,    379,    277,
         0,   -140,    -95,      0,     42,     29,      0,
};

static inline int16_t sat16(int32_t v)
{
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

size_t resampler_48_16_decimate(resampler_48_16_state_t *r,
                                const int16_t *in, size_t n_in,
                                int16_t *out)
{
    size_t out_n = 0;
    for (size_t i = 0; i < n_in; i++) {
        /* Write new sample into the circular delay line at write_idx
         * (which always points at the oldest sample, i.e. the next
         * slot to overwrite). After write, write_idx advances mod
         * R48_16_TAPS so it still points at what is now the oldest
         * sample of the next iteration. */
        r->dline[r->write_idx] = in[i];
        r->write_idx = (uint8_t)((r->write_idx + 1) % R48_16_TAPS);

        if (++r->phase < 3) continue;
        r->phase = 0;

        /* FIR convolution: H[0] (right-most, look-ahead tap) multiplies
         * the NEWEST sample (just written, at position write_idx-1);
         * H[TAPS-1] (left-most, look-behind tap) multiplies the OLDEST
         * sample (at position write_idx). Walk forward from write_idx
         * and apply H in reverse so the alignment is right. */
        int32_t acc = ROUND;
        int idx = r->write_idx;   /* oldest */
        for (int k = R48_16_TAPS - 1; k >= 0; k--) {
            acc += (int32_t)H[k] * (int32_t)r->dline[idx];
            idx++;
            if (idx >= R48_16_TAPS) idx = 0;
        }
        out[out_n++] = sat16(acc >> Q);
    }
    return out_n;
}
