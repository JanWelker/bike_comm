/*
 * mesh_proto — pure-C protocol logic for the TDMA mesh.
 *
 * Intentionally has zero ESP-IDF dependencies so it can be unit-tested
 * on the host. The ESP-IDF glue lives in mesh_mac.c.
 *
 * See docs/mesh_protocol.md for the canonical spec.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

/* ---- constants (duplicated from mesh_mac.h on purpose: this header
 * must not pull in esp_err.h). ----------------------------------- */

#define MESH_PROTO_MAX_RIDERS         8
#define MESH_PROTO_LC3_BYTES          30
#define MESH_PROTO_FRAME_BYTES        68          /* full on-air frame   */
#define MESH_PROTO_CRC_COVER_BYTES    66          /* bytes 0..65         */
#define MESH_PROTO_REPLAY_WINDOW      16          /* 2 superframes worth */

/* Flags bitfield (mirrors mesh_mac.h; defined here so host tests don't
 * need to include the ESP-IDF header). */
#define MESH_PROTO_FLAG_VAD_ACTIVE       (1u << 0)
#define MESH_PROTO_FLAG_JOIN             (1u << 1)
#define MESH_PROTO_FLAG_LEAVE            (1u << 2)
#define MESH_PROTO_FLAG_BEACON           (1u << 3)
/* The lc3_prev slot carries a real LC3 frame (the one one tick before
 * lc3). Cleared on the first packet from a rider and on beacons (where
 * the lc3+lc3_prev area is overlaid with the beacon payload). */
#define MESH_PROTO_FLAG_LC3_PREV_VALID   (1u << 4)

#define MESH_PROTO_BEACON_MAGIC      0xB1

/* ---- on-air frame layout (packed, little-endian) ---- */

typedef struct __attribute__((packed)) {
    uint8_t  rider_id;                            /* 0..7                  */
    uint8_t  flags;                               /* VAD|JOIN|LEAVE|...    */
    uint16_t seq;                                 /* seq of lc3 (the newer
                                                     of the two frames);
                                                     lc3_prev has seq-1   */
    uint16_t superframe;                          /* coord-broadcast ctr   */
    uint8_t  lc3[MESH_PROTO_LC3_BYTES];           /* current 10 ms voice   */
    uint8_t  lc3_prev[MESH_PROTO_LC3_BYTES];      /* previous 10 ms voice,
                                                     gated by
                                                     LC3_PREV_VALID flag   */
    uint16_t crc;                                 /* CRC-16/CCITT-FALSE    */
} mesh_frame_t;

_Static_assert(sizeof(mesh_frame_t) == MESH_PROTO_FRAME_BYTES,
               "mesh_frame_t must be 68 bytes on the wire");

/* ---- beacon payload — lives in the bytes where lc3+lc3_prev normally
 * sit (offset 6 .. 65) when MESH_PROTO_FLAG_BEACON is set. The two LC3
 * slots are mutually exclusive with a beacon: beacons carry no audio. */

typedef struct __attribute__((packed)) {
    uint8_t  magic;                               /* MESH_PROTO_BEACON_MAGIC */
    uint32_t coord_mac_low;                       /* lowest 4 B of coord MAC */
    uint32_t us_timestamp;                        /* coord esp_timer mod 2^32 */
    uint8_t  slot_map;                            /* bit i = slot i claimed   */
    uint8_t  group_version;                       /* bumped on PSK/schema     */
    uint8_t  reserved[49];                        /* zeros, future use        */
} mesh_beacon_t;

_Static_assert(sizeof(mesh_beacon_t) == 2 * MESH_PROTO_LC3_BYTES,
               "mesh_beacon_t must fit in lc3+lc3_prev space (60 B)");

/* ---- CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflect) ---- */

uint16_t mesh_proto_crc16(const uint8_t *data, size_t len);

/* ---- Anti-replay ----
 *
 * Accept new_seq iff it is strictly newer than last_seq and within
 * `window` of it, in 16-bit modular arithmetic. The first frame from a
 * rider (signaled by caller initialising last_seq = new_seq - 1 or by
 * separate first-seen bookkeeping) is the caller's responsibility — we
 * keep this function pure.
 *
 * Concretely: delta = (int16_t)(new_seq - last_seq); accept iff
 * 0 < delta <= window.
 */
bool mesh_proto_seq_accept(uint16_t last_seq,
                           uint16_t new_seq,
                           uint16_t window);

/* ---- Slot map ---- */

void mesh_proto_slot_claim(uint8_t *map, uint8_t slot);
void mesh_proto_slot_release(uint8_t *map, uint8_t slot);
/* Lowest bit that is 0, returning 0..7, or -1 if all 8 slots are claimed. */
int  mesh_proto_lowest_free_slot(uint8_t map);

/* ---- Coordinator election ----
 *
 * Returns true iff our_mac_low is strictly less than every entry in
 * peer_mac_lows. Equality with a peer should not happen (MACs are
 * unique) but is treated as "we win" for determinism.
 */
bool mesh_proto_we_are_coordinator(uint32_t our_mac_low,
                                   const uint32_t *peer_mac_lows,
                                   size_t peer_count);
