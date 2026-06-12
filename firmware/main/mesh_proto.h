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
#define MESH_PROTO_LC3_BYTES          40
#define MESH_PROTO_BODY_BYTES         86          /* plaintext frame body */
#define MESH_PROTO_MIC_BYTES          16          /* AES-128-CCM tag      */
#define MESH_PROTO_NONCE_LO_BYTES     4           /* per-device counter   */
#define MESH_PROTO_WIRE_BYTES         (MESH_PROTO_NONCE_LO_BYTES + \
                                       MESH_PROTO_BODY_BYTES +     \
                                       MESH_PROTO_MIC_BYTES)        /* 106 */

/* Beacons carry the low 3 bytes of each claimed slot's MAC so riders
 * can detect a stolen or double-allocated slot (not just a cleared
 * bit). 3 bytes is plenty for collision odds across <= 8 riders. */
#define MESH_PROTO_OWNER_BYTES        3

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

/* ---- on-air layout (packed, little-endian) ----
 *
 *   mesh_wire_t (106 B on air) =
 *      nonce_lo   (4 B, cleartext but covered by the MIC as AAD)
 *      cipher     (86 B, AES-128-CCM encryption of mesh_frame_t)
 *      mic        (16 B, AES-128-CCM auth tag)
 *
 * The CCM nonce reconstructed by the receiver is
 *      src_mac (6) || 0 (3) || nonce_lo (4)             = 13 B
 * The key is the 16 B group PSK (installed at boot from NVS).
 *
 * mesh_frame_t below is the *plaintext* body — exactly what gets
 * encrypted and what comes out of decryption. mesh_mac never serialises
 * a mesh_frame_t directly onto the air; everything goes through the
 * mesh_crypto layer.
 */

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
} mesh_frame_t;

_Static_assert(sizeof(mesh_frame_t) == MESH_PROTO_BODY_BYTES,
               "mesh_frame_t must equal MESH_PROTO_BODY_BYTES (plaintext body)");

typedef struct __attribute__((packed)) {
    uint32_t nonce_lo;                            /* AAD, cleartext        */
    uint8_t  cipher[MESH_PROTO_BODY_BYTES];       /* CCM-encrypted body    */
    uint8_t  mic[MESH_PROTO_MIC_BYTES];           /* CCM auth tag          */
} mesh_wire_t;

_Static_assert(sizeof(mesh_wire_t) == MESH_PROTO_WIRE_BYTES,
               "mesh_wire_t must equal MESH_PROTO_WIRE_BYTES on the wire");

/* ---- beacon payload — overlays the lc3_prev slot (offset 46 .. 85 of
 * the plaintext body) when MESH_PROTO_FLAG_BEACON is set. The lc3 slot
 * stays free for audio, letting the coordinator transmit one mic frame
 * on every beacon-bearing slot 0 instead of going silent. LC3_PREV_VALID
 * is always cleared on beacons (lc3_prev is beacon payload, not audio). */

typedef struct __attribute__((packed)) {
    uint8_t  magic;                               /* MESH_PROTO_BEACON_MAGIC */
    uint32_t coord_mac_low;                       /* lowest 4 B of coord MAC */
    uint32_t us_timestamp;                        /* mesh time of this
                                                     superframe's slot-0
                                                     boundary, mod 2^32 —
                                                     doubles as clock-sync
                                                     reference and slot-grid
                                                     phase anchor           */
    uint8_t  slot_map;                            /* bit i = slot i claimed   */
    uint8_t  group_version;                       /* bumped on PSK/schema     */
    uint8_t  slot_owner[MESH_PROTO_MAX_RIDERS][MESH_PROTO_OWNER_BYTES];
                                                  /* low 3 B of owner MAC per
                                                     claimed slot, 0 if free /
                                                     unknown (big-endian)     */
    uint8_t  reserved[5];                         /* zeros, future use        */
} mesh_beacon_t;

_Static_assert(sizeof(mesh_beacon_t) == MESH_PROTO_LC3_BYTES,
               "mesh_beacon_t must fit in lc3_prev space (= LC3 frame size)");

/* ---- Anti-replay ----
 *
 * Accept new_seq iff it is strictly newer than last_seq in 16-bit
 * modular arithmetic: delta = (int16_t)(new_seq - last_seq) > 0.
 *
 * Forward jumps of any size are accepted as a resync. A bounded
 * forward window looks safer but is a liveness trap: a TX gap longer
 * than the window (RF fade, future TX-skip heartbeats) makes the
 * first frame after the gap land outside it, and since the gap only
 * grows from there every later frame is rejected too — the receiver
 * stays deaf until the quiet-timeout tears the peer down. Replayed
 * recordings still fail (their seq is <= last seen). The first frame
 * from a rider (no last_seq yet) is the caller's responsibility.
 *
 * Post-MIC, this is in-session replay defence: a captured valid frame
 * (the ciphertext + MIC together) is still a valid CCM, but its seq
 * and per-rider monotonic nonce_lo are both replay-detectable. mesh_mac
 * also tracks the highest seen nonce_lo per peer slot for tighter
 * cross-seq-reset replay defence.
 */
bool mesh_proto_seq_accept(uint16_t last_seq, uint16_t new_seq);

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
