/*
 * Host-side LC3 roundtrip smoke test.
 *
 * Builds against the vendored liblc3 sources directly (no ESP-IDF) and
 * runs on macOS / Linux. Generates a 1 kHz sine, encodes it at the
 * firmware's operating point (16 kHz / 10 ms / 24 kbps → 30 B / frame),
 * decodes it back, and checks that the output RMS sits within 6 dB of
 * the input RMS. LC3 is lossy but should preserve energy comfortably
 * within that window for a clean tone.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lc3.h"

#define SAMPLE_RATE_HZ  16000
#define FRAME_DUR_US    10000
#define FRAME_SAMPLES   160       /* 10 ms @ 16 kHz */
#define FRAME_BYTES     30        /* 24 kbps */
#define NUM_FRAMES      10        /* 100 ms total */
#define TOTAL_SAMPLES   (FRAME_SAMPLES * NUM_FRAMES)

#define TONE_HZ         1000.0
#define TONE_AMPLITUDE  16384     /* half-scale int16 — leaves headroom */

#define RMS_TOLERANCE_DB 6.0

static double rms_db(const int16_t *samples, size_t n)
{
    double acc = 0.0;
    for (size_t i = 0; i < n; i++) {
        double s = (double)samples[i];
        acc += s * s;
    }
    double rms = sqrt(acc / (double)n);
    if (rms < 1.0) rms = 1.0;  /* clamp to avoid -inf dB on silence */
    return 20.0 * log10(rms / 32768.0);
}

int main(void)
{
    /* Generate the test tone. */
    int16_t pcm_in[TOTAL_SAMPLES];
    for (size_t i = 0; i < TOTAL_SAMPLES; i++) {
        double t = (double)i / (double)SAMPLE_RATE_HZ;
        double v = TONE_AMPLITUDE * sin(2.0 * M_PI * TONE_HZ * t);
        pcm_in[i] = (int16_t)v;
    }

    /* Set up encoder and decoder. */
    unsigned enc_sz = lc3_encoder_size(FRAME_DUR_US, SAMPLE_RATE_HZ);
    unsigned dec_sz = lc3_decoder_size(FRAME_DUR_US, SAMPLE_RATE_HZ);
    if (enc_sz == 0 || dec_sz == 0) {
        fprintf(stderr, "FAIL: lc3 size query failed (enc=%u dec=%u)\n",
                enc_sz, dec_sz);
        return 1;
    }

    void *enc_mem = malloc(enc_sz);
    void *dec_mem = malloc(dec_sz);
    if (!enc_mem || !dec_mem) {
        fprintf(stderr, "FAIL: malloc\n");
        return 1;
    }

    lc3_encoder_t enc = lc3_setup_encoder(FRAME_DUR_US, SAMPLE_RATE_HZ, 0, enc_mem);
    lc3_decoder_t dec = lc3_setup_decoder(FRAME_DUR_US, SAMPLE_RATE_HZ, 0, dec_mem);
    if (!enc || !dec) {
        fprintf(stderr, "FAIL: lc3 setup (enc=%p dec=%p)\n",
                (void *)enc, (void *)dec);
        return 1;
    }

    /* Roundtrip frame by frame. */
    int16_t pcm_out[TOTAL_SAMPLES];
    uint8_t bitstream[FRAME_BYTES];

    for (int f = 0; f < NUM_FRAMES; f++) {
        const int16_t *src = &pcm_in[f * FRAME_SAMPLES];
        int16_t       *dst = &pcm_out[f * FRAME_SAMPLES];

        int rc_e = lc3_encode(enc, LC3_PCM_FORMAT_S16,
                              src, 1, FRAME_BYTES, bitstream);
        if (rc_e != 0) {
            fprintf(stderr, "FAIL: lc3_encode frame %d rc=%d\n", f, rc_e);
            return 1;
        }

        int rc_d = lc3_decode(dec, bitstream, FRAME_BYTES,
                              LC3_PCM_FORMAT_S16, dst, 1);
        if (rc_d < 0) {
            fprintf(stderr, "FAIL: lc3_decode frame %d rc=%d\n", f, rc_d);
            return 1;
        }
    }

    /* Compare energy. */
    double in_db  = rms_db(pcm_in,  TOTAL_SAMPLES);
    double out_db = rms_db(pcm_out, TOTAL_SAMPLES);
    double delta  = fabs(in_db - out_db);

    printf("input  RMS: %7.2f dBFS\n", in_db);
    printf("output RMS: %7.2f dBFS\n", out_db);
    printf("delta:      %7.2f dB (tolerance %.1f dB)\n", delta, RMS_TOLERANCE_DB);

    free(enc_mem);
    free(dec_mem);

    if (delta > RMS_TOLERANCE_DB) {
        printf("FAIL: RMS delta %.2f dB exceeds %.1f dB tolerance\n",
               delta, RMS_TOLERANCE_DB);
        return 1;
    }

    printf("PASS\n");
    return 0;
}
