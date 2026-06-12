/*
 * mesh_mac — custom TDMA on top of ESP-NOW.
 *
 * Wire format (see docs/mesh_protocol.md for the canonical spec):
 *
 *   on-air mesh_wire_t (106 B) =
 *     | 4 B nonce_lo (cleartext, covered by MIC as AAD)         |
 *     | 86 B AES-128-CCM ciphertext of mesh_frame_t:            |
 *     |   1 B rider_id (0..7)                                   |
 *     |   1 B flags (VAD|JOIN|LEAVE|BEACON|PREV)                |
 *     |   2 B seq (of lc3)                                      |
 *     |   2 B superframe_counter                                |
 *     |   40 B lc3 (current)                                    |
 *     |   40 B lc3_prev (current-1, gated PREV)                 |
 *     | 16 B CCM auth tag                                       |
 *
 * Each packet carries up to two LC3 frames so the 100 Hz mic encode
 * rate doesn't get halved by the 50 Hz slot cadence. lc3_prev's wire
 * seq is implicitly seq-1.
 *
 * Timing:
 *   - superframe = 20 ms = 8 x 2.5 ms slots
 *   - the coordinator (lowest-MAC rider, always slot 0) beacons in
 *     slot 0 on even superframes; the beacon overlays lc3_prev only,
 *     so beacon frames still carry one audio frame in lc3
 *   - each rider TXs in their own claimed slot; PHY otherwise sleeps
 *   - joiners phase-lock their slot grid to the beacon timestamp
 *     (which is stamped at the coordinator's superframe boundary)
 *
 * Security:
 *   - App-layer AES-128-CCM, key = 16 B group PSK from NVS, nonce =
 *     src_mac (6) || 0 (3) || nonce_lo (4). nonce_lo is a per-device
 *     monotonic counter watermarked in NVS with skip-ahead, so the
 *     (key, nonce) pair never repeats — even across reboots. See
 *     mesh_crypto.{h,c}.
 *   - L2 src_mac is pinned to a slot on first frame and required to
 *     match on every subsequent frame; a PSK-holding insider must
 *     also spoof the MAC to impersonate another slot.
 *   - Forward-only seq window plus a per-peer nonce_lo high-watermark
 *     reject in-session replays AND post-seq-reset replays of captured
 *     pre-reset frames.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "mesh_proto.h"

#define MESH_MAX_RIDERS         MESH_PROTO_MAX_RIDERS
#define MESH_SUPERFRAME_US      20000
#define MESH_SLOT_US            2500

typedef enum {
    MESH_EVT_JOINED,           /* we successfully claimed a slot */
    MESH_EVT_LEFT,             /* we left the group              */
    MESH_EVT_PEER_JOINED,      /* another rider joined           */
    MESH_EVT_PEER_LEFT,        /* another rider left or timed out */
    MESH_EVT_COORDINATOR_LOST, /* lowest-MAC stopped beaconing   */
    MESH_EVT_COORDINATOR_ME,   /* we are now the coordinator      */
} mesh_event_t;

/* Delivers one LC3 frame to the application. Called twice per dual
 * mesh packet — first for lc3_prev (with seq = wire_seq - 1), then for
 * lc3 (with seq = wire_seq). The seq is the wire seq; receivers feed it
 * straight into a seq-aware jitter buffer. */
typedef void (*mesh_rx_cb_t)(uint8_t rider_id, uint16_t seq, bool vad_active,
                             const uint8_t *lc3_frame, size_t len);

typedef void (*mesh_event_cb_t)(mesh_event_t evt, uint8_t rider_id);

esp_err_t mesh_mac_init(const uint8_t group_psk[16]);
esp_err_t mesh_mac_start(void);
esp_err_t mesh_mac_stop(void);

/* Try to join the group as a new rider (listens for >= 2 superframes,
 * then claims the lowest free slot). Returns the assigned slot index
 * via out_slot, or ESP_ERR_TIMEOUT if no slot is free. The JOIN flag
 * keeps repeating in our slot until a beacon echoes our claim (bit set
 * AND slot_owner matches our MAC); a lost claim or stolen slot makes
 * the RX path renegotiate a fresh slot automatically. */
esp_err_t mesh_mac_join(uint8_t *out_slot);
esp_err_t mesh_mac_leave(void);

/* Queue a frame for TX. Frames land in a small FIFO ring (4 slots =
 * 40 ms); the TX task drains the two oldest per slot. On overflow the
 * oldest frame is evicted — too late means too late. */
esp_err_t mesh_mac_queue_tx(const uint8_t lc3_frame[MESH_PROTO_LC3_BYTES],
                            bool vad_active);

void      mesh_mac_set_rx_cb(mesh_rx_cb_t cb);
void      mesh_mac_set_event_cb(mesh_event_cb_t cb);
