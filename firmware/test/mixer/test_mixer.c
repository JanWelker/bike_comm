/*
 * Host-side unit tests for mixer_jb (pure-C jitter buffer + duck math).
 *
 * Builds against firmware/main/mixer_jb.c only — no ESP-IDF, no FreeRTOS,
 * no liblc3. Runs on macOS / Linux.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mixer_jb.h"

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, label)                                         \
    do {                                                           \
        tests_run++;                                               \
        if (cond) {                                                \
            tests_passed++;                                        \
            printf("  PASS  %s\n", label);                         \
        } else {                                                   \
            printf("  FAIL  %s  (%s:%d)\n", label,                 \
                   __FILE__, __LINE__);                            \
        }                                                          \
    } while (0)

/* Convenience: build a payload that encodes its seq for verification. */
static void make_payload(uint8_t out[MIXER_JB_LC3_BYTES], uint16_t seq)
{
    memset(out, 0, MIXER_JB_LC3_BYTES);
    out[0] = (uint8_t)(seq & 0xFF);
    out[1] = (uint8_t)((seq >> 8) & 0xFF);
}

static uint16_t payload_seq(const uint8_t buf[MIXER_JB_LC3_BYTES])
{
    return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

/* ------------------------------------------------------------------ */
/* Jitter buffer                                                       */
/* ------------------------------------------------------------------ */

static void test_jb_in_order(void)
{
    printf("test_jb_in_order\n");
    mixer_jb_t jb;
    mixer_jb_init(&jb);

    uint8_t p[MIXER_JB_LC3_BYTES];
    for (uint16_t s = 1; s <= 3; s++) {
        make_payload(p, s);
        mixer_jb_push(&jb, s, true, p, MIXER_JB_LC3_BYTES);
    }

    uint8_t out[MIXER_JB_LC3_BYTES]; uint8_t len; bool vad;
    bool ok;

    ok = mixer_jb_pull(&jb, out, &len, &vad);
    CHECK(ok && len == MIXER_JB_LC3_BYTES && vad && payload_seq(out) == 1, "pull 1");

    ok = mixer_jb_pull(&jb, out, &len, &vad);
    CHECK(ok && payload_seq(out) == 2, "pull 2");

    ok = mixer_jb_pull(&jb, out, &len, &vad);
    CHECK(ok && payload_seq(out) == 3, "pull 3");

    ok = mixer_jb_pull(&jb, out, &len, &vad);
    CHECK(!ok, "fourth pull returns false");
}

static void test_jb_out_of_order(void)
{
    printf("test_jb_out_of_order\n");
    mixer_jb_t jb;
    mixer_jb_init(&jb);

    uint8_t p[MIXER_JB_LC3_BYTES];
    uint16_t order[] = { 3, 1, 2 };
    for (int i = 0; i < 3; i++) {
        make_payload(p, order[i]);
        mixer_jb_push(&jb, order[i], true, p, MIXER_JB_LC3_BYTES);
    }

    uint8_t out[MIXER_JB_LC3_BYTES]; uint8_t len; bool vad;
    bool ok;
    ok = mixer_jb_pull(&jb, out, &len, &vad);
    CHECK(ok && payload_seq(out) == 1, "ooo pull -> 1");
    ok = mixer_jb_pull(&jb, out, &len, &vad);
    CHECK(ok && payload_seq(out) == 2, "ooo pull -> 2");
    ok = mixer_jb_pull(&jb, out, &len, &vad);
    CHECK(ok && payload_seq(out) == 3, "ooo pull -> 3");
}

static void test_jb_duplicate(void)
{
    printf("test_jb_duplicate\n");
    mixer_jb_t jb;
    mixer_jb_init(&jb);

    uint8_t p[MIXER_JB_LC3_BYTES];
    make_payload(p, 1);
    mixer_jb_push(&jb, 1, true, p, MIXER_JB_LC3_BYTES);
    mixer_jb_push(&jb, 1, true, p, MIXER_JB_LC3_BYTES);

    uint8_t out[MIXER_JB_LC3_BYTES]; uint8_t len; bool vad;
    bool ok;
    ok = mixer_jb_pull(&jb, out, &len, &vad);
    CHECK(ok && payload_seq(out) == 1, "dedup: first pull yields seq 1");
    ok = mixer_jb_pull(&jb, out, &len, &vad);
    CHECK(!ok, "dedup: second pull returns false");
}

static void test_jb_stale(void)
{
    printf("test_jb_stale\n");
    mixer_jb_t jb;
    mixer_jb_init(&jb);

    uint8_t p[MIXER_JB_LC3_BYTES];
    make_payload(p, 5);
    mixer_jb_push(&jb, 5, true, p, MIXER_JB_LC3_BYTES);

    uint8_t out[MIXER_JB_LC3_BYTES]; uint8_t len; bool vad;
    bool ok = mixer_jb_pull(&jb, out, &len, &vad);
    CHECK(ok && payload_seq(out) == 5, "pull seq 5");

    /* Push older seq=4 — should be rejected (stale). */
    make_payload(p, 4);
    mixer_jb_push(&jb, 4, true, p, MIXER_JB_LC3_BYTES);

    ok = mixer_jb_pull(&jb, out, &len, &vad);
    CHECK(!ok, "stale seq 4 dropped");
}

static void test_jb_overflow(void)
{
    printf("test_jb_overflow\n");
    /* Push 1..5 with a 3-slot buffer. We expect to retain the 3 newest:
     * 3, 4, 5. Policy: when full and incoming is newer than oldest,
     * evict oldest. */
    mixer_jb_t jb;
    mixer_jb_init(&jb);

    uint8_t p[MIXER_JB_LC3_BYTES];
    for (uint16_t s = 1; s <= 5; s++) {
        make_payload(p, s);
        mixer_jb_push(&jb, s, true, p, MIXER_JB_LC3_BYTES);
    }

    uint8_t out[MIXER_JB_LC3_BYTES]; uint8_t len; bool vad;
    bool ok;
    ok = mixer_jb_pull(&jb, out, &len, &vad);
    CHECK(ok && payload_seq(out) == 3, "overflow keeps newest: pull 3");
    ok = mixer_jb_pull(&jb, out, &len, &vad);
    CHECK(ok && payload_seq(out) == 4, "overflow keeps newest: pull 4");
    ok = mixer_jb_pull(&jb, out, &len, &vad);
    CHECK(ok && payload_seq(out) == 5, "overflow keeps newest: pull 5");
    ok = mixer_jb_pull(&jb, out, &len, &vad);
    CHECK(!ok, "overflow: nothing left");
}

static void test_jb_seq_wrap(void)
{
    printf("test_jb_seq_wrap\n");
    mixer_jb_t jb;
    mixer_jb_init(&jb);

    uint8_t p[MIXER_JB_LC3_BYTES];
    make_payload(p, 0xFFFE);
    mixer_jb_push(&jb, 0xFFFE, true, p, MIXER_JB_LC3_BYTES);

    uint8_t out[MIXER_JB_LC3_BYTES]; uint8_t len; bool vad;
    bool ok = mixer_jb_pull(&jb, out, &len, &vad);
    CHECK(ok && payload_seq(out) == 0xFFFE, "wrap: pull 0xFFFE");

    /* 0x0001 is newer than 0xFFFE under wrap-aware compare. */
    make_payload(p, 0x0001);
    mixer_jb_push(&jb, 0x0001, true, p, MIXER_JB_LC3_BYTES);
    ok = mixer_jb_pull(&jb, out, &len, &vad);
    CHECK(ok && payload_seq(out) == 0x0001, "wrap: 0x0001 accepted");

    /* 0xFFFD is older than 0x0001 under wrap-aware compare — stale. */
    make_payload(p, 0xFFFD);
    mixer_jb_push(&jb, 0xFFFD, true, p, MIXER_JB_LC3_BYTES);
    ok = mixer_jb_pull(&jb, out, &len, &vad);
    CHECK(!ok, "wrap: 0xFFFD rejected (stale across wrap)");
}

static void test_jb_vad_passthrough(void)
{
    printf("test_jb_vad_passthrough\n");
    mixer_jb_t jb;
    mixer_jb_init(&jb);

    uint8_t p[MIXER_JB_LC3_BYTES];
    make_payload(p, 10);
    mixer_jb_push(&jb, 10, false, p, MIXER_JB_LC3_BYTES);
    make_payload(p, 11);
    mixer_jb_push(&jb, 11, true,  p, MIXER_JB_LC3_BYTES);

    uint8_t out[MIXER_JB_LC3_BYTES]; uint8_t len; bool vad;
    bool ok = mixer_jb_pull(&jb, out, &len, &vad);
    CHECK(ok && payload_seq(out) == 10 && vad == false, "vad=false preserved");
    ok = mixer_jb_pull(&jb, out, &len, &vad);
    CHECK(ok && payload_seq(out) == 11 && vad == true, "vad=true preserved");
}

/* ------------------------------------------------------------------ */
/* Duck math                                                           */
/* ------------------------------------------------------------------ */

static void test_duck_0db(void)
{
    printf("test_duck_0db\n");
    int16_t q = mixer_jb_duck_q15(0);
    int16_t y = mixer_jb_apply_duck(16383, q);
    int diff = (int)y - 16383;
    if (diff < 0) diff = -diff;
    CHECK(diff <= 1, "0 dB: 16383 -> ~16383");
}

static void test_duck_6db(void)
{
    printf("test_duck_6db\n");
    int16_t q = mixer_jb_duck_q15(6);
    int16_t y = mixer_jb_apply_duck(16383, q);
    int diff = (int)y - 8200;
    if (diff < 0) diff = -diff;
    CHECK(diff <= 100, "-6 dB: 16383 -> ~8200 (+/-100)");
}

static void test_duck_12db(void)
{
    printf("test_duck_12db\n");
    int16_t q = mixer_jb_duck_q15(12);
    int16_t y = mixer_jb_apply_duck(16383, q);
    int diff = (int)y - 4115;
    if (diff < 0) diff = -diff;
    CHECK(diff <= 100, "-12 dB: 16383 -> ~4115 (+/-100)");
}

static void test_duck_saturation(void)
{
    printf("test_duck_saturation\n");
    int16_t q = mixer_jb_duck_q15(0);
    int16_t y = mixer_jb_apply_duck(INT16_MAX, q);
    /* 0 dB rounds slightly down (32767/32768) — within 1 LSB of INT16_MAX. */
    CHECK(y == INT16_MAX || y == INT16_MAX - 1,
          "0 dB: INT16_MAX stays at/near INT16_MAX, no overflow");
}

/* ------------------------------------------------------------------ */

int main(void)
{
    printf("mixer_jb unit tests\n");
    printf("===================\n");

    test_jb_in_order();
    test_jb_out_of_order();
    test_jb_duplicate();
    test_jb_stale();
    test_jb_overflow();
    test_jb_seq_wrap();
    test_jb_vad_passthrough();

    test_duck_0db();
    test_duck_6db();
    test_duck_12db();
    test_duck_saturation();

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
