/*
 * mesh_mac — custom TDMA on top of ESP-NOW.
 *
 * Wire format (see docs/mesh_protocol.md for the canonical spec):
 *
 *   | 1 B rider_id (0..7) | 1 B flags (VAD|JOIN|LEAVE|BEACON|FEC) |
 *   | 2 B seq             | 2 B superframe_counter               |
 *   | 30 B LC3 frame      | 30 B XOR parity (F_{n-1} ^ F_n)      |
 *   | 2 B CRC                                                    |
 *
 * Timing:
 *   - superframe = 20 ms = 8 x 2.5 ms slots
 *   - lowest-MAC rider in the group beacons in slot 0 (piggybacked)
 *   - each rider TXs in their own claimed slot; PHY otherwise sleeps
 *
 * Security:
 *   - v0 traffic is plaintext on the wire. ESP-NOW's built-in AES-128-CCM
 *     only applies to unicast peers with encrypt=true; our flat broadcast
 *     topology can't use it. The 16 B group PSK is still installed via
 *     esp_now_set_pmk so the encrypted path (app-layer CCM, or N unicast
 *     peers) drops in without re-architecting. See docs/mesh_protocol.md
 *     "Security" for the post-v0 plan.
 *   - app-layer monotonic seq for anti-replay
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define MESH_MAX_RIDERS         8
#define MESH_SUPERFRAME_US      20000
#define MESH_SLOT_US            2500
#define MESH_FRAME_PAYLOAD_BYTES 68    /* on-air bytes per slot */

/* Flags bitfield in the on-air header. */
#define MESH_FLAG_VAD_ACTIVE   (1u << 0)
#define MESH_FLAG_JOIN         (1u << 1)
#define MESH_FLAG_LEAVE        (1u << 2)
#define MESH_FLAG_BEACON       (1u << 3)
#define MESH_FLAG_FEC          (1u << 4)

typedef enum {
    MESH_EVT_JOINED,           /* we successfully claimed a slot */
    MESH_EVT_LEFT,             /* we left the group              */
    MESH_EVT_PEER_JOINED,      /* another rider joined           */
    MESH_EVT_PEER_LEFT,        /* another rider left or timed out */
    MESH_EVT_COORDINATOR_LOST, /* lowest-MAC stopped beaconing   */
    MESH_EVT_COORDINATOR_ME,   /* we are now the coordinator      */
} mesh_event_t;

typedef void (*mesh_rx_cb_t)(uint8_t rider_id, bool vad_active,
                             const uint8_t *lc3_frame, size_t len);

typedef void (*mesh_event_cb_t)(mesh_event_t evt, uint8_t rider_id);

esp_err_t mesh_mac_init(const uint8_t group_psk[16]);
esp_err_t mesh_mac_start(void);
esp_err_t mesh_mac_stop(void);

/* Try to join the group as a new rider (listens for >= 2 superframes,
 * then claims the lowest free slot). Returns the assigned slot index
 * via out_slot, or ESP_ERR_TIMEOUT if no slot is free. */
esp_err_t mesh_mac_join(uint8_t *out_slot);
esp_err_t mesh_mac_leave(void);

/* Queue a frame for the next TX slot. Drops if the slot already has a
 * pending frame (we don't buffer audio — too late means too late). */
esp_err_t mesh_mac_queue_tx(const uint8_t lc3_frame[30], bool vad_active);

void      mesh_mac_set_rx_cb(mesh_rx_cb_t cb);
void      mesh_mac_set_event_cb(mesh_event_cb_t cb);
