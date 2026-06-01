/*
 * mixer_jb — pure-C jitter buffer + duck math.
 *
 * No ESP-IDF, no FreeRTOS, no liblc3 — just stdint + string + assert.
 * Unit-tested from the host via firmware/test/mixer.
 */

#include "mixer_jb.h"

#include <string.h>

/* Q15 multipliers for a handful of duck steps. Hand-computed:
 *   q15 = round(32767 * 10^(-dB/20)).
 * dB column is positive = attenuation amount, matching the mixer API.
 */
static const struct {
    int8_t  db;
    int16_t q15;
} duck_table[] = {
    { 0,  32767 },
    { 3,  23197 },
    { 6,  16422 },
    { 9,  11627 },
    { 12,  8231 },
    { 18,  4124 },
    { 24,  2067 },
};

#define DUCK_TABLE_LEN (int)(sizeof(duck_table) / sizeof(duck_table[0]))

bool mixer_jb_seq_newer_than(uint16_t a, uint16_t b)
{
    /* Signed 16-bit difference handles wrap. Strict "newer than". */
    return (int16_t)(a - b) > 0;
}

void mixer_jb_init(mixer_jb_t *jb)
{
    if (!jb) return;
    memset(jb, 0, sizeof(*jb));
}

void mixer_jb_push(mixer_jb_t *jb,
                   uint16_t seq,
                   bool vad_active,
                   const uint8_t *lc3,
                   uint8_t len)
{
    if (!jb || !lc3) return;
    if (len > MIXER_JB_LC3_BYTES) return;

    /* Anti-stale: drop anything not strictly newer than last pulled. */
    if (jb->has_pulled && !mixer_jb_seq_newer_than(seq, jb->last_pulled_seq)) {
        return;
    }

    /* Dedup: drop if seq already buffered. */
    for (uint8_t i = 0; i < jb->count; i++) {
        if (jb->seq[i] == seq) return;
    }

    if (jb->count >= MIXER_JB_SLOTS) {
        /* Full. Slot 0 is oldest (we keep entries seq-sorted ascending).
         * If incoming is newer than oldest, evict oldest. Otherwise
         * drop incoming (it'd be older than something we already kept). */
        if (!mixer_jb_seq_newer_than(seq, jb->seq[0])) {
            return;
        }
        memmove(&jb->lc3[0], &jb->lc3[1],
                sizeof(jb->lc3[0]) * (MIXER_JB_SLOTS - 1));
        memmove(&jb->seq[0], &jb->seq[1],
                sizeof(jb->seq[0]) * (MIXER_JB_SLOTS - 1));
        memmove(&jb->vad[0], &jb->vad[1],
                sizeof(jb->vad[0]) * (MIXER_JB_SLOTS - 1));
        jb->count--;
    }

    /* Insertion-sort position: first index i where seq[i] is newer
     * than incoming. We shift [i..count) right by one and insert. */
    uint8_t pos = jb->count;
    for (uint8_t i = 0; i < jb->count; i++) {
        if (mixer_jb_seq_newer_than(jb->seq[i], seq)) {
            pos = i;
            break;
        }
    }

    if (pos < jb->count) {
        for (uint8_t i = jb->count; i > pos; i--) {
            memcpy(jb->lc3[i], jb->lc3[i - 1], MIXER_JB_LC3_BYTES);
            jb->seq[i] = jb->seq[i - 1];
            jb->vad[i] = jb->vad[i - 1];
        }
    }

    memset(jb->lc3[pos], 0, MIXER_JB_LC3_BYTES);
    memcpy(jb->lc3[pos], lc3, len);
    jb->seq[pos] = seq;
    jb->vad[pos] = vad_active ? 1 : 0;
    jb->count++;
}

bool mixer_jb_pull(mixer_jb_t *jb,
                   uint8_t *lc3_out,
                   uint8_t *len_out,
                   bool *vad_out)
{
    if (!jb) return false;
    if (jb->count == 0) return false;

    if (lc3_out) memcpy(lc3_out, jb->lc3[0], MIXER_JB_LC3_BYTES);
    if (len_out) *len_out = MIXER_JB_LC3_BYTES;
    if (vad_out) *vad_out = jb->vad[0] ? true : false;

    jb->last_pulled_seq = jb->seq[0];
    jb->has_pulled = true;

    /* Shift remaining down by one. */
    if (jb->count > 1) {
        memmove(&jb->lc3[0], &jb->lc3[1],
                sizeof(jb->lc3[0]) * (jb->count - 1));
        memmove(&jb->seq[0], &jb->seq[1],
                sizeof(jb->seq[0]) * (jb->count - 1));
        memmove(&jb->vad[0], &jb->vad[1],
                sizeof(jb->vad[0]) * (jb->count - 1));
    }
    jb->count--;

    return true;
}

int16_t mixer_jb_duck_q15(int8_t duck_db)
{
    if (duck_db < 0)  duck_db = 0;
    if (duck_db > 24) duck_db = 24;

    /* Nearest-tabled-value lookup. Table is small + sorted. */
    int best = 0;
    int best_diff = duck_db - duck_table[0].db;
    if (best_diff < 0) best_diff = -best_diff;
    for (int i = 1; i < DUCK_TABLE_LEN; i++) {
        int d = duck_db - duck_table[i].db;
        if (d < 0) d = -d;
        if (d < best_diff) {
            best_diff = d;
            best = i;
        }
    }
    return duck_table[best].q15;
}

int16_t mixer_jb_apply_duck(int16_t sample, int16_t q15_mult)
{
    int32_t y = ((int32_t)sample * (int32_t)q15_mult) >> 15;
    if (y >  32767) y =  32767;
    if (y < -32768) y = -32768;
    return (int16_t)y;
}
