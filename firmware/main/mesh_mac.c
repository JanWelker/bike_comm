/*
 * mesh_mac — TDMA on ESP-NOW, ESP-IDF glue layer.
 *
 * Pure-C protocol logic lives in mesh_proto.{h,c}; this file owns the
 * radio, the FreeRTOS task, and the per-rider runtime state.
 *
 * Spec: docs/mesh_protocol.md.
 */

#include "mesh_mac.h"
#include "mesh_proto.h"
#include "codec.h"       /* for CODEC_FRAME_BYTES */

#include <inttypes.h>
#include <string.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"      /* esp_rom_delay_us */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "mesh";

/* The codec frame size and the wire layout are defined in separate
 * dependency-free headers; pin them together here so a codec change
 * (e.g. the planned Codec2 alternate) can't silently desync them. */
_Static_assert(CODEC_FRAME_BYTES == MESH_PROTO_LC3_BYTES,
               "codec frame size must match the wire slot size");

#define MESH_TASK_PRIO          19
#define MESH_CORE                0
#define MESH_JOIN_LISTEN_MS     60        /* 3 superframes */
#define MESH_COORD_LOSS_SFRAMES  10       /* 200 ms: tolerates short bursts of RF
                                             beacon loss now that the coordinator
                                             only beacons every other superframe */
#define MESH_PEER_QUIET_SFRAMES 10        /* §implicit leave        */

/* Heartbeat period for VAD-silent slots.
 *
 * Phase-1 VAD gating only skips the encode + queue, so a silent rider still
 * burned a slot every superframe just to refresh peers' quiet counters.
 * Phase 2 (this constant) skips the radio entirely on silent slots, and
 * keeps the slot claim alive by forcing one header-only TX every K
 * superframes. K must be strictly < MESH_PEER_QUIET_SFRAMES; we pick 5
 * (= 100 ms keep-alive period, half the quiet timeout) so a single
 * heartbeat loss still leaves four superframes of headroom before the
 * receiver tears us down.
 *
 * The previous attempt at this scheme was reverted 2026-06 — the
 * receiver dropped every post-gap frame because:
 *   (1) s_tx_seq advanced even on un-sent slots,
 *   (2) the anti-replay window was a bounded 16-seq forward window
 *       (= 8 superframes), so a 5-superframe gap left every later seq
 *       outside the window,
 *   (3) JOIN was one-shot, so the quiet-timeout collapse couldn't be
 *       repaired by a stale claim.
 * All three are now fixed (seq increments are gated on send_this_slot,
 * mesh_proto_seq_accept is "any strictly-forward delta = accept", JOIN
 * is level-triggered until the beacon acks). With that foundation the
 * heartbeat is safe — but bench-soak before trusting it.
 *
 * Coordinators are exempt from the heartbeat check: they already emit a
 * beacon every other superframe (40 ms cadence) which acts as their
 * keep-alive, and skipping audio-slot transmissions on the coord is the
 * only way coordinator silence ever wins airtime here. */
#define MESH_HEARTBEAT_SFRAMES   5

/* Forward-only-slewed mesh time sync. The coordinator's local esp_timer
 * IS the mesh clock; joiners maintain s_mesh_clock_offset_us so
 * mesh_now_us() returns the coord's notion of "now." On each beacon RX
 * the joiner computes delta = coord_timestamp - mesh_now_us() and
 * applies it:
 *   - first beacon ever:                       snap to align.
 *   - subsequent, delta > MESH_CLOCK_LARGE_STEP: snap forward (coord
 *       changed, packet loss accumulated drift).
 *   - subsequent, 0 < delta <= LARGE_STEP:     slew by delta/SLEW_DEN.
 *   - delta <= 0 (we're ahead of coord):       ignore — forward-only.
 *
 * Forward-only protects joiners from running their slot scheduler
 * backwards through coord failover. Algorithm cribbed from
 * Hemisphere-Project/ESPNowMeshClock (GPL-3.0; algorithm only, no
 * code copied). */
#define MESH_CLOCK_SLEW_DEN        16     /* alpha = 1/16: ~640 ms half-life */
#define MESH_CLOCK_LARGE_STEP_US   5000   /* >5 ms = jump, not slew  */

static const uint8_t BROADCAST_MAC[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* ---- public callbacks ---- */
static mesh_rx_cb_t    s_rx_cb    = NULL;
static mesh_event_cb_t s_event_cb = NULL;

/* Spinlock guarding all state shared between the ESP-NOW recv callback
 * (wifi task, prio 23, core 0) and mesh_tx_task (prio 19, core 0):
 * slot map, per-rider bookkeeping, clock offset/anchor, pending flags.
 * Plain RMW on these raced before — the tx task's quiet-counter ++
 * could overwrite the recv path's reset to 0, and the int64 clock
 * offset could tear (two 32-bit ops on Xtensa). Critical sections are
 * kept to a few loads/stores; no FreeRTOS calls or logging inside. */
static portMUX_TYPE s_mac_mux = portMUX_INITIALIZER_UNLOCKED;

/* ---- local state (shared fields protected by s_mac_mux) ---- */
static uint8_t  s_psk[16];
static uint8_t  s_own_slot   = 0xFF;     /* 0xFF = not joined */
static uint8_t  s_slot_map   = 0;        /* echoed by coordinator beacon */
static uint8_t  s_group_version = 0;
static bool     s_group_version_locked = false;  /* adopted from 1st beacon */
static uint32_t s_our_mac_low   = 0;
static uint32_t s_coord_mac_low = 0xFFFFFFFFu;
static uint16_t s_superframe_counter = 0;
static uint16_t s_tx_seq = 0;

/* per-rider book-keeping (index = rider_id) */
static uint16_t s_last_seq[MESH_MAX_RIDERS];
static bool     s_seq_seen[MESH_MAX_RIDERS];
static uint32_t s_peer_mac_low[MESH_MAX_RIDERS];
static uint16_t s_peer_quiet_sframes[MESH_MAX_RIDERS];

/* coordinator tracking */
static uint16_t s_sframes_since_beacon = 0xFFFF;   /* large → "never heard" */

/* Mesh time sync — see MESH_CLOCK_* block above. */
static int64_t s_mesh_clock_offset_us = 0;
static bool    s_mesh_clock_locked    = false;

/* Slot-grid phase anchor: mesh time of a coordinator superframe (slot-0)
 * boundary, reconstructed from the beacon's us_timestamp on every beacon
 * RX. The clock sync above aligns the VALUE of mesh_now_us(); this
 * aligns the PHASE of the 20 ms slot grid — without it each rider's
 * grid sits at a power-on-random 0..20 ms offset from the coordinator's
 * and slot N of one rider can permanently overlap slot 0 of another. */
static int64_t s_sframe_anchor_us    = 0;
static bool    s_sframe_anchor_valid = false;

/* Deferred-log state: the recv callback must not call ESP_LOG (it
 * blocks on the shared stdout lock); it records here and mesh_tx_task
 * logs during end-of-superframe housekeeping. */
#define MESH_CLOCK_LOG_NONE 0
#define MESH_CLOCK_LOG_LOCK 1
#define MESH_CLOCK_LOG_SNAP 2
static volatile uint8_t  s_clock_log_kind  = MESH_CLOCK_LOG_NONE;
static volatile int32_t  s_clock_log_delta = 0;
static volatile uint32_t s_gv_mismatch_count = 0;   /* beacons dropped */

/* mesh state machine */
typedef enum {
    MESH_S_IDLE = 0,
    MESH_S_JOINING_LISTEN,
    MESH_S_JOINED,
} mesh_state_t;

static volatile mesh_state_t s_state = MESH_S_IDLE;

/* TX ring — protected by s_tx_mtx.
 *
 * The mic encodes at 100 fps but a rider's slot only fires at 50 fps,
 * and the two clocks (audio_io's I2S DMA vs mesh_tx's esp_timer) are
 * independent — short-term phase drift means a naive 2-slot buffer
 * sometimes has only 1 frame at TX time and sometimes has 3 arrive
 * between TXes. We absorb that variance in a small FIFO: queue_tx
 * appends to the head, TX consumes the two oldest unsent. With a
 * 4-slot ring (= 40 ms of audio) and the long-term 2:1 mic:TX ratio,
 * the ring stays under-full in steady state and overflow only kicks
 * in under sustained back-pressure (eviction is FIFO — oldest goes). */
#define MESH_TX_RING_SLOTS 4

typedef struct {
    uint8_t lc3[CODEC_FRAME_BYTES];
    bool    vad;
} tx_ring_slot_t;

static SemaphoreHandle_t s_tx_mtx = NULL;
static tx_ring_slot_t    s_tx_ring[MESH_TX_RING_SLOTS];
static uint8_t           s_tx_ring_head  = 0;  /* next write position    */
static uint8_t           s_tx_ring_count = 0;  /* unsent frames in ring  */

/* Join/leave signalling — under s_mac_mux (NOT s_tx_mtx) because the
 * recv callback also sets s_join_unacked when it renegotiates a slot,
 * and the recv callback may never block on a mutex.
 *
 * s_join_unacked is level-triggered, not one-shot: the JOIN flag rides
 * on every TX until a beacon echoes our claim (bit set AND slot_owner
 * == our MAC). A single lost JOIN frame used to leave the slot
 * unclaimed on the coordinator forever. */
static bool s_pending_leave = false;
static bool s_join_unacked  = false;

static TaskHandle_t s_tx_task_handle = NULL;
static volatile bool s_task_should_exit = false;

/* ---- static forward decls ---- */
static void mesh_tx_task(void *arg);
static void on_esp_now_recv(const esp_now_recv_info_t *info,
                            const uint8_t *data, int len);
static void on_esp_now_send(const esp_now_send_info_t *info,
                            esp_now_send_status_t status);

/* ---- helpers ---- */

static uint32_t mac_low_from_bytes(const uint8_t mac[6])
{
    /* lowest 4 bytes, big-endian (so 02:00:00:AA:BB:CC → 0x00AABBCC) */
    return ((uint32_t)mac[2] << 24) |
           ((uint32_t)mac[3] << 16) |
           ((uint32_t)mac[4] <<  8) |
           ((uint32_t)mac[5]);
}

/* Mesh-time "now" — local esp_timer plus the slewed coord offset.
 * Coordinator's offset stays 0, so mesh_now_us() == esp_timer_get_time()
 * on the coord side. Joiners' offset is set/slewed on each beacon RX.
 * The offset is int64 and written by the recv callback; read it under
 * the spinlock so it can't tear on the 32-bit LX6. */
static int64_t mesh_now_us(void)
{
    portENTER_CRITICAL(&s_mac_mux);
    int64_t off = s_mesh_clock_offset_us;
    portEXIT_CRITICAL(&s_mac_mux);
    return esp_timer_get_time() + off;
}

/* Low 3 bytes of a mac_low, as carried in beacon slot_owner[]. */
static uint32_t mac_low24(uint32_t mac_low)
{
    return mac_low & 0xFFFFFFu;
}

static void owner_encode(uint8_t out[MESH_PROTO_OWNER_BYTES], uint32_t mac_low)
{
    out[0] = (uint8_t)(mac_low >> 16);
    out[1] = (uint8_t)(mac_low >> 8);
    out[2] = (uint8_t)(mac_low);
}

static uint32_t owner_decode(const uint8_t in[MESH_PROTO_OWNER_BYTES])
{
    return ((uint32_t)in[0] << 16) | ((uint32_t)in[1] << 8) | (uint32_t)in[2];
}

/* "Should I beacon right now?" — true iff we currently hold the role per
 * s_coord_mac_low, which is only ever assigned from (1) our own bootstrap,
 * (2) a failover takeover, or (3) a received beacon. JOIN frames do NOT
 * change s_coord_mac_low, so a lower-MAC peer joining the group does not
 * preempt us — the spec requires us to yield only on hearing their
 * actual beacon. */
static bool we_hold_coordinator_role(void)
{
    return s_coord_mac_low == s_our_mac_low ||
           s_coord_mac_low == 0xFFFFFFFFu;
}

/* "If the coordinator is lost, am I next in line?" — pure MAC-low compare
 * across us + all currently-claimed peers. Used by the failover handler
 * to decide which rider takes over after MESH_COORD_LOSS_SFRAMES (10)
 * missed beacons. */
static bool we_are_lowest_mac_in_group(void)
{
    uint32_t peers[MESH_MAX_RIDERS];
    size_t   n = 0;
    portENTER_CRITICAL(&s_mac_mux);
    for (int i = 0; i < MESH_MAX_RIDERS; ++i) {
        if ((s_slot_map & (1u << i)) && s_peer_mac_low[i] != 0) {
            peers[n++] = s_peer_mac_low[i];
        }
    }
    portEXIT_CRITICAL(&s_mac_mux);
    return mesh_proto_we_are_coordinator(s_our_mac_low, peers, n);
}

static void fill_beacon_payload(mesh_frame_t *f, int64_t sframe_boundary_us)
{
    /* Beacon overlays the 40 B of lc3_prev only; lc3 stays available
     * for audio so the coordinator can still ship one mic frame per
     * beacon slot.
     *
     * us_timestamp carries the mesh time of THIS superframe's slot-0
     * boundary (== the scheduled TX instant of this beacon), not "now":
     * receivers use it both for clock sync and to phase-lock their slot
     * grid. Mesh time stays continuous through failover (a takeover
     * coordinator keeps its slewed offset), so joiners' slew never sees
     * a discontinuity. */
    mesh_beacon_t b = (mesh_beacon_t){0};
    b.magic         = MESH_PROTO_BEACON_MAGIC;
    b.coord_mac_low = s_our_mac_low;
    b.us_timestamp  = (uint32_t)sframe_boundary_us;
    portENTER_CRITICAL(&s_mac_mux);
    b.slot_map      = s_slot_map;
    b.group_version = s_group_version;
    for (int i = 0; i < MESH_MAX_RIDERS; ++i) {
        if ((s_slot_map & (1u << i)) == 0) continue;
        uint32_t owner = (i == s_own_slot) ? s_our_mac_low
                                           : s_peer_mac_low[i];
        owner_encode(b.slot_owner[i], mac_low24(owner));
    }
    portEXIT_CRITICAL(&s_mac_mux);
    memcpy(&f->lc3_prev[0], &b, sizeof(b));
}

/* ---- init / start / stop ---- */

esp_err_t mesh_mac_init(const uint8_t group_psk[16])
{
    ESP_LOGI(TAG, "init");
    memcpy(s_psk, group_psk, 16);

    /* Reset per-rider state. */
    memset(s_last_seq, 0, sizeof(s_last_seq));
    memset(s_seq_seen, 0, sizeof(s_seq_seen));
    memset(s_peer_mac_low, 0, sizeof(s_peer_mac_low));
    memset(s_peer_quiet_sframes, 0, sizeof(s_peer_quiet_sframes));
    s_tx_ring_head  = 0;
    s_tx_ring_count = 0;
    s_join_unacked  = false;
    s_pending_leave = false;
    s_mesh_clock_offset_us = 0;
    s_mesh_clock_locked    = false;
    s_sframe_anchor_valid  = false;
    s_group_version_locked = false;
    s_clock_log_kind       = MESH_CLOCK_LOG_NONE;
    s_gv_mismatch_count    = 0;

    if (s_tx_mtx == NULL) {
        s_tx_mtx = xSemaphoreCreateMutex();
        if (s_tx_mtx == NULL) return ESP_ERR_NO_MEM;
    }

    /* Learn our own MAC. We do this *before* esp_now_init so a missing
     * Wi-Fi driver fails noisily. */
    uint8_t mac[6] = {0};
    esp_err_t err = esp_efuse_mac_get_default(mac);
    if (err != ESP_OK) return err;
    s_our_mac_low = mac_low_from_bytes(mac);
    ESP_LOGI(TAG, "our mac_low = 0x%08lx", (unsigned long)s_our_mac_low);

    err = esp_now_init();
    if (err != ESP_OK) return err;

    err = esp_now_set_pmk(s_psk);
    if (err != ESP_OK) return err;

    err = esp_now_register_recv_cb(on_esp_now_recv);
    if (err != ESP_OK) return err;

    /* Register an empty send callback — we don't act on tx_status (lossy
     * voice doesn't retransmit), but ESP-NOW requires the callback to
     * exist if we want to keep statistics. */
    err = esp_now_register_send_cb(on_esp_now_send);
    if (err != ESP_OK) return err;

    /* Add broadcast peer.
     *
     * v0 ships unencrypted on the wire — ESP-NOW only applies AES-128-CCM
     * to unicast peers with encrypt=true; broadcast is plaintext. We
     * still install the PSK above so we don't re-architect when we add
     * encryption. See docs/mesh_protocol.md "Security" for the two
     * options (app-layer CCM vs. unicast-with-CCM) and the decision
     * gate (before any public/field deployment, well before v1). */
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, BROADCAST_MAC, 6);
    peer.channel = 0;          /* current channel */
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = false;      /* broadcast can't be CCM-encrypted */
    err = esp_now_add_peer(&peer);
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) return err;

    return ESP_OK;
}

esp_err_t mesh_mac_start(void)
{
    s_task_should_exit = false;
    BaseType_t ok = xTaskCreatePinnedToCore(mesh_tx_task, "mesh_tx", 4096,
                                            NULL, MESH_TASK_PRIO,
                                            &s_tx_task_handle,
                                            MESH_CORE);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t mesh_mac_stop(void)
{
    s_task_should_exit = true;
    /* Task self-terminates on next loop. Caller can join via task handle
     * if needed; for v0 we just drop the handle. */
    return ESP_OK;
}

/* ---- join / leave ---- */

esp_err_t mesh_mac_join(uint8_t *out_slot)
{
    s_state = MESH_S_JOINING_LISTEN;
    ESP_LOGI(TAG, "join: listening for beacon (up to %d ms)", MESH_JOIN_LISTEN_MS);

    /* Listen for ≥ 2 superframes while on_esp_now_recv refreshes
     * s_slot_map from any beacon we hear. */
    vTaskDelay(pdMS_TO_TICKS(MESH_JOIN_LISTEN_MS));

    /* Bootstrap case: if no beacon was heard during the listen window
     * we are the first rider in the group. Claim slot 0, mark our own
     * bit in the local slot_map, self-elect coordinator, transition to
     * JOINED. The TX task picks up beaconing on the next superframe.
     * If a second rider bootstrapped simultaneously (both powered on
     * inside the same listen window), the higher-MAC one yields and
     * renegotiates a slot when it hears the lower one's beacon — see
     * the dual-coordinator handling in on_esp_now_recv. */
    if (s_sframes_since_beacon == 0xFFFF) {
        portENTER_CRITICAL(&s_mac_mux);
        s_own_slot      = 0;
        mesh_proto_slot_claim(&s_slot_map, 0);
        s_coord_mac_low = s_our_mac_low;
        portEXIT_CRITICAL(&s_mac_mux);
        s_state = MESH_S_JOINED;
        if (out_slot) *out_slot = 0;
        ESP_LOGI(TAG, "join: bootstrap as solo rider, slot=0 (coordinator)");
        if (s_event_cb) s_event_cb(MESH_EVT_JOINED, 0);
        if (s_event_cb) s_event_cb(MESH_EVT_COORDINATOR_ME, 0);
        return ESP_OK;
    }

    /* Normal join: heard at least one beacon, so a group already exists.
     * Pick the lowest free slot and commit locally; the TX task repeats
     * the JOIN flag every superframe until a beacon echoes our claim
     * (bit set AND slot_owner == our MAC). A lost claim or a same-slot
     * collision is detected by the beacon-RX path, which renegotiates a
     * fresh slot automatically. */
    uint8_t claimed;
    portENTER_CRITICAL(&s_mac_mux);
    int slot = mesh_proto_lowest_free_slot(s_slot_map);
    if (slot >= 0) {
        s_own_slot = (uint8_t)slot;
        mesh_proto_slot_claim(&s_slot_map, (uint8_t)slot);
        s_join_unacked = true;
    }
    portEXIT_CRITICAL(&s_mac_mux);

    if (slot < 0) {
        ESP_LOGW(TAG, "join: no free slot (map=0x%02x)", s_slot_map);
        s_state = MESH_S_IDLE;
        return ESP_ERR_TIMEOUT;
    }
    claimed = (uint8_t)slot;
    s_state = MESH_S_JOINED;
    if (out_slot) *out_slot = claimed;
    ESP_LOGI(TAG, "join: claiming slot %d (JOIN repeats until beacon ack)",
             claimed);
    if (s_event_cb) s_event_cb(MESH_EVT_JOINED, claimed);
    return ESP_OK;
}

esp_err_t mesh_mac_leave(void)
{
    if (s_state != MESH_S_JOINED) return ESP_OK;
    portENTER_CRITICAL(&s_mac_mux);
    s_pending_leave = true;
    s_join_unacked  = false;
    portEXIT_CRITICAL(&s_mac_mux);
    /* Give the task one superframe to emit the LEAVE frame. */
    vTaskDelay(pdMS_TO_TICKS(20));
    portENTER_CRITICAL(&s_mac_mux);
    mesh_proto_slot_release(&s_slot_map, s_own_slot);
    s_own_slot = 0xFF;
    portEXIT_CRITICAL(&s_mac_mux);
    s_state = MESH_S_IDLE;
    if (s_event_cb) s_event_cb(MESH_EVT_LEFT, 0);
    return ESP_OK;
}

/* ---- TX queue ---- */

esp_err_t mesh_mac_queue_tx(const uint8_t lc3_frame[40], bool vad_active)
{
    if (!lc3_frame) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_tx_mtx, 0) != pdTRUE) {
        /* Don't block the audio task — if we can't get the mutex this
         * tick, drop the frame. */
        return ESP_ERR_TIMEOUT;
    }
    if (s_tx_ring_count == MESH_TX_RING_SLOTS) {
        /* Ring full — drop the oldest. The position to-be-written
         * (s_tx_ring_head) currently holds the oldest entry; reducing
         * the count by 1 logically removes it before we overwrite. */
        s_tx_ring_count--;
    }
    memcpy(s_tx_ring[s_tx_ring_head].lc3, lc3_frame, CODEC_FRAME_BYTES);
    s_tx_ring[s_tx_ring_head].vad = vad_active;
    s_tx_ring_head = (uint8_t)((s_tx_ring_head + 1) % MESH_TX_RING_SLOTS);
    s_tx_ring_count++;
    xSemaphoreGive(s_tx_mtx);
    return ESP_OK;
}

void mesh_mac_set_rx_cb(mesh_rx_cb_t cb)       { s_rx_cb    = cb; }
void mesh_mac_set_event_cb(mesh_event_cb_t cb) { s_event_cb = cb; }

/* ---- TX task: superframe scheduler ----------------------------------
 *
 * v0 timing model: a single mesh_now_us()-driven loop that wakes once
 * per superframe, sleeps until our slot, transmits, then sleeps to the
 * next superframe boundary. Joiners get their mesh_now_us() slewed
 * forward by the beacon RX path (see MESH_CLOCK_* above), so all slot
 * timings are in coord-relative mesh time. Sleeps use local esp_timer
 * under the hood — the difference between two mesh-time values IS the
 * duration we want to sleep regardless.
 */
static void mesh_tx_task(void *arg)
{
    (void)arg;
    int64_t  superframe_start = mesh_now_us();
    int64_t  last_slot_tx_us  = INT64_MIN / 2;
    uint32_t gv_logged        = 0;

    /* Heartbeat bookkeeping. Counts the run of superframes since we last
     * actually radiated in our slot, regardless of cause (VAD-silent,
     * coordinator-role + audio slot empty, etc). The skip/sent counters
     * feed a per-window diag log; the per-skip detail line is gated
     * behind a single LOGD that can be promoted with esp_log_level_set. */
    uint16_t sframes_since_tx     = 0;
    uint32_t hb_skipped_count     = 0;
    uint32_t hb_heartbeat_count   = 0;
    uint32_t hb_audio_count       = 0;

    while (!s_task_should_exit) {
        /* If we're not in the group, just wait a superframe. */
        if (s_state != MESH_S_JOINED) {
            vTaskDelay(pdMS_TO_TICKS(20));
            superframe_start = mesh_now_us();
            s_superframe_counter++;
            continue;
        }

        portENTER_CRITICAL(&s_mac_mux);
        bool    anchored = s_sframe_anchor_valid &&
                           s_coord_mac_low != s_our_mac_low;
        int64_t anchor   = s_sframe_anchor_us;
        uint8_t own_slot = s_own_slot;
        portEXIT_CRITICAL(&s_mac_mux);
        if (own_slot >= MESH_MAX_RIDERS) {   /* renegotiation gave up */
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        /* Phase-lock our superframe grid to the coordinator's. The
         * clock slew aligns the VALUE of mesh_now_us(); this aligns
         * the PHASE of the 20 ms grid by re-deriving this iteration's
         * superframe start from the beacon's slot-0 boundary anchor.
         * Free-running instead (the old behaviour) left every rider's
         * grid at a power-on-random 0..20 ms offset, so slot N of one
         * rider could permanently overlap slot 0 of another. */
        if (anchored) {
            int64_t now = mesh_now_us();
            int64_t k   = (now - anchor) / MESH_SUPERFRAME_US;
            if (now < anchor) k -= 1;
            int64_t snapped = anchor + k * MESH_SUPERFRAME_US;
            /* If the anchor shifted backward past our last TX, don't
             * re-service a superframe we already transmitted in. */
            if (snapped + (int64_t)own_slot * MESH_SLOT_US
                    <= last_slot_tx_us + MESH_SUPERFRAME_US / 2) {
                snapped += MESH_SUPERFRAME_US;
            }
            superframe_start = snapped;
        }

        /* Sleep until just before our slot start. */
        int64_t slot_start_us = superframe_start +
                                (int64_t)own_slot * MESH_SLOT_US;
        int64_t now           = mesh_now_us();
        int64_t to_sleep_us   = slot_start_us - now;
        if (to_sleep_us > 1000) {
            vTaskDelay(pdMS_TO_TICKS((to_sleep_us - 500) / 1000));
        }
        /* Burn the last few hundred µs in busy-wait to align tighter
         * than the FreeRTOS tick. TODO(v0.5): use the high-res timer
         * with an ISR-driven semaphore release instead. */
        now = mesh_now_us();
        if (now < slot_start_us) {
            esp_rom_delay_us((uint32_t)(slot_start_us - now));
        }

        /* Build the frame. */
        mesh_frame_t f = (mesh_frame_t){0};
        f.rider_id   = own_slot;
        f.flags      = 0;
        f.superframe = s_superframe_counter;

        /* Coordinator role: alternate slot-0 between beacon (even
         * superframes) and audio (odd superframes). The 40 B beacon
         * lives in lc3_prev only, so beacon slots can still ship one
         * audio frame in lc3 — bumps coord -> joiner from 50 fps to
         * 75 fps. Full 100 fps closure needs a dedicated 9th beacon
         * slot. The coordinator always holds slot 0 (bootstrap and
         * failover both claim it), so the beacon schedule never needs
         * a second wake-up inside the superframe. */
        bool will_beacon = (own_slot == 0 && we_hold_coordinator_role() &&
                            (s_superframe_counter & 1u) == 0);

        portENTER_CRITICAL(&s_mac_mux);
        if (s_join_unacked)  f.flags |= MESH_PROTO_FLAG_JOIN;
        if (s_pending_leave) { f.flags |= MESH_PROTO_FLAG_LEAVE;
                               s_pending_leave = false; }
        portEXIT_CRITICAL(&s_mac_mux);

        bool header_only = true;
        xSemaphoreTake(s_tx_mtx, portMAX_DELAY);
        if (s_tx_ring_count > 0) {
            /* On audio slots, bundle the two oldest unsent into
             * lc3 + lc3_prev. On beacon slots, only pull one (the
             * newer goes into lc3; lc3_prev will be overlaid with the
             * beacon below). f.seq names the newer frame; lc3_prev's
             * implicit seq is f.seq - 1. s_tx_seq stays 1:1 with LC3
             * frames. */
            uint8_t oldest = (uint8_t)((s_tx_ring_head
                                        + MESH_TX_RING_SLOTS
                                        - s_tx_ring_count)
                                       % MESH_TX_RING_SLOTS);
            bool vad_union = s_tx_ring[oldest].vad;
            if (!will_beacon && s_tx_ring_count >= 2) {
                uint8_t newer = (uint8_t)((oldest + 1) % MESH_TX_RING_SLOTS);
                memcpy(f.lc3_prev, s_tx_ring[oldest].lc3, CODEC_FRAME_BYTES);
                memcpy(f.lc3,      s_tx_ring[newer].lc3,  CODEC_FRAME_BYTES);
                vad_union = vad_union || s_tx_ring[newer].vad;
                f.flags |= MESH_PROTO_FLAG_LC3_PREV_VALID;
                f.seq = s_tx_seq + 1;
                s_tx_seq += 2;
                s_tx_ring_count -= 2;
            } else {
                memcpy(f.lc3, s_tx_ring[oldest].lc3, CODEC_FRAME_BYTES);
                f.seq = s_tx_seq;
                s_tx_seq += 1;
                s_tx_ring_count -= 1;
            }
            if (vad_union) f.flags |= MESH_PROTO_FLAG_VAD_ACTIVE;
            header_only = false;
        } else {
            /* No audio queued — header-only frame. The seq is only
             * consumed below if the frame actually goes on air, so a
             * TX gap can't silently burn through the seq space. */
            f.seq = s_tx_seq;
        }
        xSemaphoreGive(s_tx_mtx);

        if (will_beacon) {
            /* Beacon overlays lc3_prev only; lc3 keeps whatever audio
             * the ring drained into it. LC3_PREV_VALID stays cleared
             * because lc3_prev is beacon payload here, not audio. */
            f.flags |= MESH_PROTO_FLAG_BEACON;
            fill_beacon_payload(&f, superframe_start);
        }

        /* TX decision.
         *
         * Old policy (pre-heartbeat): every joined rider radiated every
         * superframe, even header-only, to refresh peers' quiet counters.
         * That sustained ~4 % of the 1 Mbps PHY at 8 riders for nothing
         * during silence.
         *
         * New policy:
         *   - Carrying audio                 → send (covers VAD-active).
         *   - JOIN / LEAVE / BEACON pending  → send (signalling).
         *   - Otherwise, send only every Kth slot to refresh the claim.
         *
         * K = MESH_HEARTBEAT_SFRAMES (5) — strictly < MESH_PEER_QUIET_SFRAMES.
         *
         * Beacon slots on the coordinator already cover the keep-alive
         * (BEACON flag forces send_this_slot regardless), so coordinators
         * never reach the heartbeat path: their silent audio-slot frames
         * get skipped outright. Net coord airtime during silence is one
         * beacon every 40 ms instead of two frames every 20 ms. */
        bool has_signaling = (f.flags & (MESH_PROTO_FLAG_JOIN  |
                                         MESH_PROTO_FLAG_LEAVE |
                                         MESH_PROTO_FLAG_BEACON)) != 0;
        bool heartbeat_due = sframes_since_tx >= MESH_HEARTBEAT_SFRAMES - 1;
        bool send_this_slot = !header_only || has_signaling || heartbeat_due;

        if (send_this_slot) {
            if (header_only) s_tx_seq += 1;
            f.crc = mesh_proto_crc16((const uint8_t *)&f,
                                     MESH_PROTO_CRC_COVER_BYTES);
            esp_err_t err = esp_now_send(BROADCAST_MAC,
                                         (const uint8_t *)&f,
                                         sizeof(f));
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "esp_now_send: %d", err);
            }
            last_slot_tx_us = slot_start_us;
            sframes_since_tx = 0;
            if (header_only) hb_heartbeat_count++;
            else             hb_audio_count++;
        } else {
            sframes_since_tx++;
            hb_skipped_count++;
            ESP_LOGD(TAG, "tx skip: header-only silence (since_tx=%u)",
                     (unsigned)sframes_since_tx);
        }

        /* Sleep to the end of this superframe, then advance. */
        int64_t next_superframe = superframe_start + MESH_SUPERFRAME_US;
        now = mesh_now_us();
        if (next_superframe > now) {
            int64_t rem = next_superframe - now;
            if (rem > 1000) vTaskDelay(pdMS_TO_TICKS((rem - 500) / 1000));
            now = mesh_now_us();
            if (now < next_superframe) {
                esp_rom_delay_us((uint32_t)(next_superframe - now));
            }
        }
        superframe_start = next_superframe;
        s_superframe_counter++;

        /* End-of-superframe housekeeping. Counter RMWs share state with
         * the recv callback, so they run under the spinlock; events and
         * logs fire after it is released. */
        uint8_t dropped[MESH_MAX_RIDERS];
        int     n_dropped = 0;
        portENTER_CRITICAL(&s_mac_mux);
        if (s_sframes_since_beacon != 0xFFFF) s_sframes_since_beacon++;
        uint16_t sframes_since_beacon = s_sframes_since_beacon;

        /* Implicit-leave timeout: any rider silent for
         * MESH_PEER_QUIET_SFRAMES gets booted from our local view. */
        for (int i = 0; i < MESH_MAX_RIDERS; ++i) {
            if ((s_slot_map & (1u << i)) == 0) continue;
            if (i == s_own_slot)               continue;
            s_peer_quiet_sframes[i]++;
            if (s_peer_quiet_sframes[i] >= MESH_PEER_QUIET_SFRAMES) {
                mesh_proto_slot_release(&s_slot_map, (uint8_t)i);
                s_peer_mac_low[i] = 0;
                s_peer_quiet_sframes[i] = 0;
                s_seq_seen[i] = false;
                dropped[n_dropped++] = (uint8_t)i;
            }
        }
        portEXIT_CRITICAL(&s_mac_mux);

        for (int i = 0; i < n_dropped; ++i) {
            ESP_LOGI(TAG, "peer %d quiet for %d sframes, dropping",
                     dropped[i], MESH_PEER_QUIET_SFRAMES);
            if (s_event_cb) s_event_cb(MESH_EVT_PEER_LEFT, dropped[i]);
        }

        /* Flush logs the recv callback deferred (it must not touch the
         * stdout lock). */
        if (s_clock_log_kind != MESH_CLOCK_LOG_NONE) {
            uint8_t kind  = s_clock_log_kind;
            int32_t delta = s_clock_log_delta;
            s_clock_log_kind = MESH_CLOCK_LOG_NONE;
            ESP_LOGI(TAG, "%s %ld us",
                     kind == MESH_CLOCK_LOG_LOCK
                         ? "mesh clock locked, initial offset"
                         : "mesh clock snap forward",
                     (long)delta);
        }
        if (s_gv_mismatch_count != gv_logged &&
            (s_superframe_counter % 50u) == 0) {
            gv_logged = s_gv_mismatch_count;
            ESP_LOGW(TAG, "ignored %lu beacons (group_version mismatch)",
                     (unsigned long)gv_logged);
        }

        /* Heartbeat TX-stats window (~10 s @ 50 sframes/s). Sum is the
         * raw slot count, not packets/sec — divide by 10 mentally on the
         * bench. Resets in lockstep so the ratio is window-local. */
        if ((s_superframe_counter % 500u) == 0 &&
            (hb_audio_count | hb_heartbeat_count | hb_skipped_count) != 0) {
            uint32_t total = hb_audio_count + hb_heartbeat_count +
                             hb_skipped_count;
            ESP_LOGI(TAG, "tx 10s: audio=%lu hb=%lu skip=%lu (%lu%% silenced)",
                     (unsigned long)hb_audio_count,
                     (unsigned long)hb_heartbeat_count,
                     (unsigned long)hb_skipped_count,
                     (unsigned long)((100ul * hb_skipped_count) /
                                     (total ? total : 1u)));
            hb_audio_count     = 0;
            hb_heartbeat_count = 0;
            hb_skipped_count   = 0;
        }

        /* Coordinator failover: if no beacon heard for
         * MESH_COORD_LOSS_SFRAMES (10) AND we are next-lowest-MAC, take
         * the coordinator role. The role is welded to slot 0 (the
         * beacon rides slot 0's parity schedule), so vacate our old
         * slot and claim slot 0 — the dead coordinator's claim is
         * forcibly released, we haven't heard it for 10 superframes.
         *
         * Skip when s_sframes_since_beacon is still at the 0xFFFF
         * sentinel — that means we have never heard a beacon at all
         * (e.g. solo bootstrap path), so there's nothing to "fail over
         * from." */
        if (sframes_since_beacon != 0xFFFF &&
            sframes_since_beacon >= MESH_COORD_LOSS_SFRAMES &&
            s_coord_mac_low != s_our_mac_low &&
            we_are_lowest_mac_in_group()) {
            uint8_t old_slot;
            bool    old_coord_claimed;
            portENTER_CRITICAL(&s_mac_mux);
            old_slot          = s_own_slot;
            old_coord_claimed = (s_slot_map & 1u) != 0;
            mesh_proto_slot_release(&s_slot_map, old_slot);
            s_peer_mac_low[0]       = 0;
            s_peer_quiet_sframes[0] = 0;
            s_seq_seen[0]           = false;
            mesh_proto_slot_claim(&s_slot_map, 0);
            s_own_slot      = 0;
            s_coord_mac_low = s_our_mac_low;
            s_join_unacked  = false;   /* our own beacons are the ack */
            portEXIT_CRITICAL(&s_mac_mux);
            ESP_LOGW(TAG, "coordinator lost, taking role (slot %u -> 0)",
                     old_slot);
            if (s_event_cb) s_event_cb(MESH_EVT_COORDINATOR_LOST, 0);
            if (old_coord_claimed && old_slot != 0 && s_event_cb) {
                s_event_cb(MESH_EVT_PEER_LEFT, 0);
            }
            if (s_event_cb) s_event_cb(MESH_EVT_COORDINATOR_ME, 0);
        }
    }

    s_tx_task_handle = NULL;
    vTaskDelete(NULL);
}

/* ---- RX path ---- */

static void on_esp_now_recv(const esp_now_recv_info_t *info,
                            const uint8_t *data, int len)
{
    if (len != (int)sizeof(mesh_frame_t)) return;

    const mesh_frame_t *f = (const mesh_frame_t *)data;

    /* CRC. */
    uint16_t want = mesh_proto_crc16(data, MESH_PROTO_CRC_COVER_BYTES);
    if (want != f->crc) {
        ESP_LOGD(TAG, "crc mismatch from rider %u (have 0x%04x want 0x%04x)",
                 f->rider_id, f->crc, want);
        return;
    }

    uint8_t rid = f->rider_id;
    if (rid >= MESH_MAX_RIDERS) return;

    /* This callback runs in the wifi task: no blocking, no allocation,
     * no ESP_LOGI/W (the stdout lock can stall behind a UART flush).
     * Shared-state updates run under the s_mac_mux spinlock; events are
     * collected and fired after it is released (the event callback only
     * enqueues, but FreeRTOS queue ops are not critical-section-safe). */
    struct { mesh_event_t evt; uint8_t rid; } events[MESH_MAX_RIDERS + 3];
    int n_events = 0;

    mesh_beacon_t b;
    bool is_beacon = false;
    if (f->flags & MESH_PROTO_FLAG_BEACON) {
        memcpy(&b, &f->lc3_prev[0], sizeof(b));
        is_beacon = (b.magic == MESH_PROTO_BEACON_MAGIC);
    }

    /* A frame in our own slot is either a rival coordinator's beacon
     * (simultaneous bootstrap, failover crossfire) or a slot collision.
     * We fall through only for a lower-MAC coordinator's beacon — the
     * own-slot validation below then renegotiates our slot, which is
     * how the higher-MAC side of a dual-coordinator split backs down.
     * Anything else in our slot is ignored so a colliding transmitter
     * can't poison our per-rider bookkeeping. */
    if (s_state == MESH_S_JOINED && rid == s_own_slot &&
        !(is_beacon && b.coord_mac_low < s_our_mac_low)) {
        return;
    }

    /* Anti-replay (skip on first-ever frame from this rider), then
     * per-rider bookkeeping. Accepted frames are the only liveness
     * credit — replayed duplicates don't reset the quiet counter. */
    bool     had_seq;
    uint16_t prev_last_seq;
    bool     accepted;
    portENTER_CRITICAL(&s_mac_mux);
    had_seq       = s_seq_seen[rid];
    prev_last_seq = s_last_seq[rid];
    accepted      = !had_seq || mesh_proto_seq_accept(prev_last_seq, f->seq);
    if (accepted) {
        s_last_seq[rid] = f->seq;
        s_seq_seen[rid] = true;
        s_peer_quiet_sframes[rid] = 0;
        /* Track this peer's MAC-low (from info->src_addr). First
         * claimant wins: a colliding transmitter must not overwrite
         * the recorded owner, or the beacon's slot_owner would flap
         * between the two and both sides would renegotiate forever. */
        if (s_peer_mac_low[rid] == 0 && info && info->src_addr) {
            s_peer_mac_low[rid] = mac_low_from_bytes(info->src_addr);
        }
    }
    portEXIT_CRITICAL(&s_mac_mux);
    if (!accepted) {
        ESP_LOGD(TAG, "replay drop rider=%u last=%u new=%u",
                 rid, prev_last_seq, f->seq);
        return;
    }

    /* Beacon handling. The 40 B beacon lives in lc3_prev; lc3 still
     * carries one audio frame (LC3_PREV_VALID is always cleared on
     * beacons). Falls through to audio delivery below. */
    if (is_beacon) {
        portENTER_CRITICAL(&s_mac_mux);
        bool adopt = true;
        /* Group-version gate: first beacon fixes the version, later
         * mismatches (foreign group, incompatible build) are ignored
         * rather than silently merging two incompatible meshes. */
        if (!s_group_version_locked) {
            s_group_version        = b.group_version;
            s_group_version_locked = true;
        } else if (b.group_version != s_group_version) {
            s_gv_mismatch_count++;
            adopt = false;
        }

        if (adopt) {
            uint8_t old_map = s_slot_map;
            s_slot_map      = b.slot_map;
            s_coord_mac_low = b.coord_mac_low;
            s_sframes_since_beacon = 0;

            /* Slots the coordinator released since our last view:
             * clear the per-rider bookkeeping so a later claimant of
             * the same slot starts from a clean seq history. */
            for (int i = 0; i < MESH_MAX_RIDERS; ++i) {
                if (i == s_own_slot) continue;
                if ((old_map & (1u << i)) && !(b.slot_map & (1u << i))) {
                    s_peer_mac_low[i]       = 0;
                    s_peer_quiet_sframes[i] = 0;
                    s_seq_seen[i]           = false;
                    events[n_events].evt = MESH_EVT_PEER_LEFT;
                    events[n_events].rid = (uint8_t)i;
                    n_events++;
                }
            }

            /* Forward-only-slew our mesh clock toward the coord's.
             * Skip if we ARE the coord (offset stays 0 by definition).
             * The wire timestamp is the bottom 32 bits of the coord's
             * mesh time at its slot-0 boundary; difference vs our
             * current mesh-time (mod 2^32) is taken as a signed int32
             * so the diff wraps cleanly. Transit time is treated as
             * negligible — the slew absorbs sub-ms error within tens
             * of beacons. */
            if (s_coord_mac_low != s_our_mac_low) {
                uint32_t our_now_mod = (uint32_t)(esp_timer_get_time() +
                                                  s_mesh_clock_offset_us);
                int32_t  delta_us    = (int32_t)(b.us_timestamp - our_now_mod);
                if (!s_mesh_clock_locked) {
                    s_mesh_clock_offset_us += delta_us;
                    s_mesh_clock_locked = true;
                    s_clock_log_kind  = MESH_CLOCK_LOG_LOCK;
                    s_clock_log_delta = delta_us;
                } else if (delta_us > MESH_CLOCK_LARGE_STEP_US) {
                    s_mesh_clock_offset_us += delta_us;
                    s_clock_log_kind  = MESH_CLOCK_LOG_SNAP;
                    s_clock_log_delta = delta_us;
                } else if (delta_us > 0) {
                    s_mesh_clock_offset_us += delta_us / MESH_CLOCK_SLEW_DEN;
                }
                /* delta_us <= 0: we're ahead of coord — ignore. */

                /* Slot-grid phase anchor: the beacon timestamp IS a
                 * superframe boundary; re-express it in (post-update)
                 * mesh time for the TX task's grid derivation. */
                int64_t now2 = esp_timer_get_time() + s_mesh_clock_offset_us;
                int32_t d2   = (int32_t)(b.us_timestamp - (uint32_t)now2);
                s_sframe_anchor_us    = now2 + d2;
                s_sframe_anchor_valid = true;
            }

            /* Own-slot validation against the authoritative beacon. */
            if (s_state == MESH_S_JOINED && s_own_slot < MESH_MAX_RIDERS) {
                uint8_t  slot    = s_own_slot;
                bool     bit_set = (b.slot_map & (1u << slot)) != 0;
                uint32_t owner   = owner_decode(b.slot_owner[slot]);
                uint32_t us24    = mac_low24(s_our_mac_low);
                if (bit_set && owner == us24) {
                    s_join_unacked = false;          /* claim confirmed */
                } else if (bit_set && owner != 0 && owner != us24) {
                    /* Someone else owns our slot (join collision lost,
                     * coordinator handed it away during a fade, or we
                     * are the yielding half of a dual-coordinator
                     * split): renegotiate a fresh slot. */
                    int ns = mesh_proto_lowest_free_slot(s_slot_map);
                    if (ns >= 0) {
                        s_own_slot = (uint8_t)ns;
                        mesh_proto_slot_claim(&s_slot_map, (uint8_t)ns);
                        s_join_unacked = true;
                        events[n_events].evt = MESH_EVT_JOINED;
                        events[n_events].rid = (uint8_t)ns;
                        n_events++;
                    } else {
                        s_own_slot     = 0xFF;
                        s_join_unacked = false;
                        s_state        = MESH_S_IDLE;
                        events[n_events].evt = MESH_EVT_LEFT;
                        events[n_events].rid = 0;
                        n_events++;
                    }
                } else if (!bit_set) {
                    /* Coordinator dropped us (RF fade outlived the
                     * quiet timeout) — the slot is free per the beacon,
                     * so re-claim it and let the JOIN flag re-announce. */
                    mesh_proto_slot_claim(&s_slot_map, slot);
                    s_join_unacked = true;
                }
                /* bit_set && owner == 0: coordinator hasn't learned our
                 * MAC yet — keep the JOIN flag running. */
            }
        }
        portEXIT_CRITICAL(&s_mac_mux);
    }

    /* JOIN flag: another rider claimed a slot. */
    if (f->flags & MESH_PROTO_FLAG_JOIN) {
        portENTER_CRITICAL(&s_mac_mux);
        bool fresh = (s_slot_map & (1u << rid)) == 0;
        if (fresh) mesh_proto_slot_claim(&s_slot_map, rid);
        portEXIT_CRITICAL(&s_mac_mux);
        if (fresh) {
            events[n_events].evt = MESH_EVT_PEER_JOINED;
            events[n_events].rid = rid;
            n_events++;
        }
    }

    /* LEAVE flag: explicit departure. */
    if (f->flags & MESH_PROTO_FLAG_LEAVE) {
        portENTER_CRITICAL(&s_mac_mux);
        mesh_proto_slot_release(&s_slot_map, rid);
        s_peer_mac_low[rid] = 0;
        s_seq_seen[rid]     = false;
        portEXIT_CRITICAL(&s_mac_mux);
        if (s_event_cb) {
            for (int i = 0; i < n_events; ++i) {
                s_event_cb(events[i].evt, events[i].rid);
            }
            s_event_cb(MESH_EVT_PEER_LEFT, rid);
        }
        return;
    }

    if (s_event_cb) {
        for (int i = 0; i < n_events; ++i) {
            s_event_cb(events[i].evt, events[i].rid);
        }
    }

    /* Deliver up to two LC3 frames per packet.
     *
     * The packet's f->seq is the seq of the current (newer) frame; the
     * previous frame's seq is implicitly f->seq - 1.
     *
     * lc3_prev is delivered first (so the JB sees frames in seq order)
     * iff (a) the sender flagged it valid AND (b) we haven't already
     * pulled past it. Concretely: deliver iff prev_last_seq is older
     * than (f->seq - 1) — i.e. we missed it in the prior packet — or
     * this is the first packet we've seen from this rider.
     *
     * Both frames are delivered regardless of VAD — the vad flag rides
     * along on the callback so the mixer can skip decode for silence
     * frames (clean zero contribution) without falling through to PLC.
     * Anti-replay is already enforced on f->seq above; the JB will
     * dedup if lc3_prev happens to duplicate something it buffered. */
    bool vad = (f->flags & MESH_PROTO_FLAG_VAD_ACTIVE) != 0;
    if (s_rx_cb && (f->flags & MESH_PROTO_FLAG_LC3_PREV_VALID)) {
        uint16_t prev_seq = (uint16_t)(f->seq - 1);
        bool deliver_prev = !had_seq ||
                            (int16_t)(prev_seq - prev_last_seq) > 0;
        if (deliver_prev) {
            s_rx_cb(rid, prev_seq, vad, f->lc3_prev, CODEC_FRAME_BYTES);
        }
    }
    if (s_rx_cb) {
        s_rx_cb(rid, f->seq, vad, f->lc3, CODEC_FRAME_BYTES);
    }
}

static void on_esp_now_send(const esp_now_send_info_t *info,
                            esp_now_send_status_t status)
{
    /* Voice is loss-tolerant; we never retransmit. Status logged at
     * debug level only. */
    (void)info; (void)status;
}
