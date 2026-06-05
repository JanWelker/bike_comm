/*
 * mixer_jb — pure-C jitter buffer + duck math for the audio mixer.
 *
 * Carved out of mixer.c so we can unit-test it on the host without
 * pulling in ESP-IDF, FreeRTOS, or liblc3. No platform deps in here.
 *
 * Each rider gets one mixer_jb_t. It holds up to MIXER_JB_SLOTS LC3
 * frames in seq-sorted order, dedups, drops late/stale frames, and
 * tracks per-slot VAD so the mixer can skip decode for inactive slots.
 *
 * Duck math is a Q15-multiplier lookup at a handful of dB steps.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Slot count. Matches the existing MIXER_JITTER_FRAMES = 3 (30 ms). */
#define MIXER_JB_SLOTS       3

/* LC3 frame size at 32 kbps / 10 ms / 16 kHz. Duplicated from
 * codec_lc3.h to keep this header dependency-free for host tests. */
#define MIXER_JB_LC3_BYTES   40

typedef struct {
    uint8_t  lc3[MIXER_JB_SLOTS][MIXER_JB_LC3_BYTES];
    uint16_t seq[MIXER_JB_SLOTS];
    uint8_t  vad[MIXER_JB_SLOTS];     /* 1 if frame had VAD active, else 0 */
    uint8_t  count;
    uint16_t last_pulled_seq;
    bool     has_pulled;              /* last_pulled_seq is meaningful */
} mixer_jb_t;

/* Zero state. */
void mixer_jb_init(mixer_jb_t *jb);

/* Push an LC3 frame for sequence number `seq`. Drops if:
 *   - seq is older-or-equal than last_pulled_seq (16-bit wrap aware)
 *   - seq already in buffer (dedup)
 *   - buffer full AND incoming is older than the buffer's oldest slot
 * On overflow with a newer incoming, the oldest slot is evicted to
 * keep the 3 newest unique seqs in order. */
void mixer_jb_push(mixer_jb_t *jb,
                   uint16_t seq,
                   bool vad_active,
                   const uint8_t *lc3,
                   uint8_t len);

/* Pull the oldest queued frame. Returns false if empty.
 * On success copies lc3 (always MIXER_JB_LC3_BYTES), len_out, vad_out,
 * advances state, and updates last_pulled_seq + has_pulled. */
bool mixer_jb_pull(mixer_jb_t *jb,
                   uint8_t *lc3_out,
                   uint8_t *len_out,
                   bool *vad_out);

/* Pick the nearest tabled Q15 multiplier for a duck level in dB.
 * Argument convention matches mixer.c: positive = attenuation amount,
 * i.e. duck_db=6 means -6 dB. duck_db is clamped to [0, 24]. */
int16_t mixer_jb_duck_q15(int8_t duck_db);

/* Multiply sample by a Q15 mul and saturate to int16. */
int16_t mixer_jb_apply_duck(int16_t sample, int16_t q15_mult);

/* 16-bit-wrap-aware "is a strictly newer than b?". Exposed for tests
 * and for use by the mixer when ordering frames across modules. */
bool mixer_jb_seq_newer_than(uint16_t a, uint16_t b);

#ifdef __cplusplus
}
#endif
