/*
 * Host-side unit tests for mesh_proto. Pure C, plain assert().
 *
 *   cd firmware/test/mesh_mac && make && ./test_mesh_proto
 */

#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mesh_proto.h"

static int tests_run    = 0;
static int tests_passed = 0;

#define TEST(name) static void name(void)
#define RUN(name)  do {                                                       \
    tests_run++;                                                              \
    printf("  [RUN ] %s\n", #name);                                           \
    name();                                                                   \
    tests_passed++;                                                           \
    printf("  [PASS] %s\n", #name);                                           \
} while (0)

/* ------------------------------------------------------------------ */
/* CRC-16/CCITT-FALSE                                                  */
/* ------------------------------------------------------------------ */

TEST(test_crc_kat_123456789)
{
    /* Canonical CCITT-FALSE test vector: "123456789" → 0x29B1. */
    const uint8_t v[] = "123456789";
    uint16_t crc = mesh_proto_crc16(v, 9);
    assert(crc == 0x29B1);
}

TEST(test_crc_kat_empty)
{
    /* Empty input → init value, 0xFFFF. */
    uint16_t crc = mesh_proto_crc16((const uint8_t *)"", 0);
    assert(crc == 0xFFFF);
}

TEST(test_crc_kat_A)
{
    /* Single byte "A" (0x41).
     *
     * Manual derivation:
     *   crc = 0xFFFF ^ (0x41 << 8) = 0xBEFF
     *   loop 8 bits, applying x^16+x^12+x^5+1 on top bit
     *   → 0xB915 (verified against an external CCITT-FALSE calculator).
     */
    const uint8_t v[] = { 'A' };
    uint16_t crc = mesh_proto_crc16(v, 1);
    assert(crc == 0xB915);
}

TEST(test_crc_changes_on_bitflip)
{
    uint8_t a[8] = { 0,1,2,3,4,5,6,7 };
    uint8_t b[8] = { 0,1,2,3,4,5,6,7 };
    b[3] ^= 0x01;
    assert(mesh_proto_crc16(a, 8) != mesh_proto_crc16(b, 8));
}

/* ------------------------------------------------------------------ */
/* Anti-replay                                                         */
/* ------------------------------------------------------------------ */

TEST(test_replay_strict_increase)
{
    /* Strictly increasing seqs within window are accepted. */
    assert( mesh_proto_seq_accept(100, 101, 16));
    assert( mesh_proto_seq_accept(101, 110, 16));
}

TEST(test_replay_stale_rejected)
{
    /* Stale by 1 — same value — out-of-order rejected. */
    assert(!mesh_proto_seq_accept(101, 100, 16));
    assert(!mesh_proto_seq_accept(101, 101, 16));   /* duplicate */
    assert(!mesh_proto_seq_accept(200, 100, 16));   /* far stale */
}

TEST(test_replay_future_in_window)
{
    /* Future by 1 (and up to window) is accepted. */
    assert( mesh_proto_seq_accept(100, 101, 16));
    assert( mesh_proto_seq_accept(100, 116, 16));
    /* Beyond the window — reject (treat as "way off, possibly attacker
     * or stale"). */
    assert(!mesh_proto_seq_accept(100, 117, 16));
}

TEST(test_replay_wraparound)
{
    /* last_seq = 0xFFFE, new_seq = 0x0001 → delta +3 → accepted. */
    assert( mesh_proto_seq_accept(0xFFFE, 0x0001, 16));
    /* last_seq = 0x0001, new_seq = 0xFFF0 → delta -17 → rejected. */
    assert(!mesh_proto_seq_accept(0x0001, 0xFFF0, 16));
    /* last_seq = 0xFFFE, new_seq = 0xFFFE → duplicate → rejected. */
    assert(!mesh_proto_seq_accept(0xFFFE, 0xFFFE, 16));
}

/* ------------------------------------------------------------------ */
/* Slot map                                                            */
/* ------------------------------------------------------------------ */

TEST(test_slot_claim_release)
{
    uint8_t map = 0;
    mesh_proto_slot_claim(&map, 0);
    mesh_proto_slot_claim(&map, 1);
    mesh_proto_slot_claim(&map, 3);
    assert(map == 0x0B);                       /* bits 0,1,3 */

    mesh_proto_slot_release(&map, 1);
    assert(map == 0x09);                       /* bits 0,3 */

    /* Idempotent release. */
    mesh_proto_slot_release(&map, 1);
    assert(map == 0x09);

    /* Out-of-range is a no-op. */
    mesh_proto_slot_claim(&map, 9);
    mesh_proto_slot_release(&map, 9);
    assert(map == 0x09);
}

TEST(test_slot_lowest_free)
{
    assert(mesh_proto_lowest_free_slot(0x00) == 0);
    assert(mesh_proto_lowest_free_slot(0x01) == 1);
    assert(mesh_proto_lowest_free_slot(0x0B) == 2);  /* 0,1,3 taken */
    assert(mesh_proto_lowest_free_slot(0xFE) == 0);  /* slot 0 free  */
    assert(mesh_proto_lowest_free_slot(0xFF) == -1); /* full         */
}

/* ------------------------------------------------------------------ */
/* Coordinator election                                                */
/* ------------------------------------------------------------------ */

TEST(test_coord_we_win_no_peers)
{
    /* Empty peer list → we are coordinator. */
    assert(mesh_proto_we_are_coordinator(0x1234ABCD, NULL, 0));
}

TEST(test_coord_we_win_higher_peers)
{
    uint32_t peers[] = { 0x20000000, 0x30000000 };
    assert( mesh_proto_we_are_coordinator(0x10000000, peers, 2));
}

TEST(test_coord_we_lose_to_lower_peer)
{
    uint32_t peers[] = { 0x05000000, 0x30000000 };
    assert(!mesh_proto_we_are_coordinator(0x10000000, peers, 2));
}

/* ------------------------------------------------------------------ */
/* On-wire layout sanity                                               */
/* ------------------------------------------------------------------ */

TEST(test_wire_layout_sizes)
{
    /* These are also _Static_asserts in the header, but re-check at
     * runtime for documentation. */
    assert(sizeof(mesh_frame_t)  == 68);
    assert(sizeof(mesh_beacon_t) == 60);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    printf("mesh_proto host tests\n");
    printf("=====================\n");

    RUN(test_crc_kat_123456789);
    RUN(test_crc_kat_empty);
    RUN(test_crc_kat_A);
    RUN(test_crc_changes_on_bitflip);

    RUN(test_replay_strict_increase);
    RUN(test_replay_stale_rejected);
    RUN(test_replay_future_in_window);
    RUN(test_replay_wraparound);

    RUN(test_slot_claim_release);
    RUN(test_slot_lowest_free);

    RUN(test_coord_we_win_no_peers);
    RUN(test_coord_we_win_higher_peers);
    RUN(test_coord_we_lose_to_lower_peer);

    RUN(test_wire_layout_sizes);

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
