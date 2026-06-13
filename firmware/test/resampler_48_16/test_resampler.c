/*
 * test_resampler — host-side sanity checks for resampler_48_16.
 *
 *   1. DC pass: a constant input passes through at unity gain.
 *   2. Continuity: feeding the same total signal in two halves
 *      matches a single-call result (history works).
 *   3. Out-of-band rejection: a 16 kHz sine at fs=48 kHz must come
 *      out with much smaller RMS than a 1 kHz sine of the same
 *      amplitude. The old box filter failed this test loudly.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "resampler_48_16.h"

#define N_IN_FRAMES   480       /* 10 ms at 48 kHz */
#define N_OUT_FRAMES  (N_IN_FRAMES / 3)

static double rms(const int16_t *x, int n)
{
    double s = 0;
    for (int i = 0; i < n; i++) s += (double)x[i] * (double)x[i];
    return sqrt(s / n);
}

static int test_dc_pass(void)
{
    resampler_48_16_state_t r = {0};
    int16_t in[N_IN_FRAMES];
    int16_t out[N_OUT_FRAMES];

    for (int i = 0; i < N_IN_FRAMES; i++) in[i] = 10000;

    /* Prime: first call has zero history so output ramps. Run a few
     * frames to let history fill, then measure on the next frame. */
    for (int prime = 0; prime < 5; prime++) {
        resampler_48_16_decimate(&r, in, N_IN_FRAMES, out);
    }
    resampler_48_16_decimate(&r, in, N_IN_FRAMES, out);

    double mean = 0;
    for (int i = 0; i < N_OUT_FRAMES; i++) mean += out[i];
    mean /= N_OUT_FRAMES;

    if (fabs(mean - 10000) > 50) {
        printf("FAIL dc_pass: expected ~10000, got %.1f\n", mean);
        return 1;
    }
    printf("ok dc_pass: mean=%.1f\n", mean);
    return 0;
}

static int test_continuity(void)
{
    resampler_48_16_state_t r1 = {0}, r2 = {0};
    int16_t in[N_IN_FRAMES * 2];
    int16_t out_whole[N_OUT_FRAMES * 2 + 2];
    int16_t out_split_a[N_OUT_FRAMES + 2];
    int16_t out_split_b[N_OUT_FRAMES + 2];

    /* Some non-trivial signal: 1 kHz sine. */
    for (int i = 0; i < N_IN_FRAMES * 2; i++) {
        in[i] = (int16_t)(8000.0 * sin(2.0 * M_PI * 1000.0 * i / 48000.0));
    }

    size_t n_whole   = resampler_48_16_decimate(&r1, in,
                                                  N_IN_FRAMES * 2, out_whole);
    size_t n_split_a = resampler_48_16_decimate(&r2, in,
                                                  N_IN_FRAMES, out_split_a);
    size_t n_split_b = resampler_48_16_decimate(&r2, in + N_IN_FRAMES,
                                                  N_IN_FRAMES, out_split_b);

    if (n_split_a + n_split_b != n_whole) {
        printf("FAIL continuity: counts %zu+%zu vs %zu\n",
               n_split_a, n_split_b, n_whole);
        return 1;
    }
    for (size_t i = 0; i < n_split_a; i++) {
        if (out_whole[i] != out_split_a[i]) {
            printf("FAIL continuity: out_whole[%zu]=%d vs split_a[%zu]=%d\n",
                   i, out_whole[i], i, out_split_a[i]);
            return 1;
        }
    }
    for (size_t i = 0; i < n_split_b; i++) {
        if (out_whole[n_split_a + i] != out_split_b[i]) {
            printf("FAIL continuity: out_whole[%zu]=%d vs split_b[%zu]=%d\n",
                   n_split_a + i, out_whole[n_split_a + i],
                   i, out_split_b[i]);
            return 1;
        }
    }
    printf("ok continuity: %zu outputs match exactly\n", n_whole);
    return 0;
}

static int test_alias_rejection(void)
{
    resampler_48_16_state_t r1 = {0}, r2 = {0};
    int16_t in_lo[N_IN_FRAMES];
    int16_t in_hi[N_IN_FRAMES];
    int16_t out_lo[N_OUT_FRAMES];
    int16_t out_hi[N_OUT_FRAMES];

    /* Same amplitude sines: 1 kHz (in-band, should pass) and 16 kHz
     * (above the 8 kHz output Nyquist, should be suppressed). */
    for (int i = 0; i < N_IN_FRAMES; i++) {
        in_lo[i] = (int16_t)(10000.0 * sin(2.0 * M_PI *  1000.0 * i / 48000.0));
        in_hi[i] = (int16_t)(10000.0 * sin(2.0 * M_PI * 16000.0 * i / 48000.0));
    }

    /* Prime history. */
    for (int p = 0; p < 5; p++) {
        resampler_48_16_decimate(&r1, in_lo, N_IN_FRAMES, out_lo);
        resampler_48_16_decimate(&r2, in_hi, N_IN_FRAMES, out_hi);
    }
    resampler_48_16_decimate(&r1, in_lo, N_IN_FRAMES, out_lo);
    resampler_48_16_decimate(&r2, in_hi, N_IN_FRAMES, out_hi);

    double rms_lo = rms(out_lo, N_OUT_FRAMES);
    double rms_hi = rms(out_hi, N_OUT_FRAMES);
    double ratio_db = 20.0 * log10(rms_lo / (rms_hi + 1e-9));

    printf("ok alias_rejection: rms_lo=%.1f rms_hi=%.1f, suppression=%.1f dB\n",
           rms_lo, rms_hi, ratio_db);

    /* Box-filter was around 13 dB. A proper windowed-sinc gives 40+.
     * Demand at least 30 dB to leave headroom for rounding in Q14. */
    if (ratio_db < 30.0) {
        printf("FAIL alias_rejection: only %.1f dB, expected >= 30\n", ratio_db);
        return 1;
    }
    return 0;
}

int main(void)
{
    int fails = 0;
    fails += test_dc_pass();
    fails += test_continuity();
    fails += test_alias_rejection();
    if (fails) {
        printf("\n%d test(s) failed\n", fails);
        return 1;
    }
    printf("\nall tests passed\n");
    return 0;
}
