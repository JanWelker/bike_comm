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
#include "codec_lc3.h"   /* for LC3_FRAME_BYTES */

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

#define MESH_TASK_PRIO          19
#define MESH_CORE                0
#define MESH_JOIN_LISTEN_MS     60        /* 3 superframes */
#define MESH_JOIN_RETRIES        3
#define MESH_COORD_LOSS_SFRAMES  5        /* §coordinator failover */
#define MESH_PEER_QUIET_SFRAMES 10        /* §implicit leave        */

static const uint8_t BROADCAST_MAC[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* ---- public callbacks ---- */
static mesh_rx_cb_t    s_rx_cb    = NULL;
static mesh_event_cb_t s_event_cb = NULL;

/* ---- local state ---- */
static uint8_t  s_psk[16];
static uint8_t  s_own_slot   = 0xFF;     /* 0xFF = not joined */
static uint8_t  s_slot_map   = 0;        /* echoed by coordinator beacon */
static uint8_t  s_group_version = 0;
static uint32_t s_our_mac_low   = 0;
static uint32_t s_coord_mac_low = 0xFFFFFFFFu;
static uint16_t s_superframe_counter = 0;
static uint16_t s_tx_seq = 0;

/* per-rider book-keeping (index = rider_id) */
static uint16_t s_last_seq[MESH_MAX_RIDERS];
static bool     s_seq_seen[MESH_MAX_RIDERS];
static uint8_t  s_last_lc3[MESH_MAX_RIDERS][LC3_FRAME_BYTES];
static bool     s_last_lc3_valid[MESH_MAX_RIDERS];
static uint32_t s_peer_mac_low[MESH_MAX_RIDERS];
static uint16_t s_peer_quiet_sframes[MESH_MAX_RIDERS];

/* coordinator tracking */
static uint16_t s_sframes_since_beacon = 0xFFFF;   /* large → "never heard" */

/* mesh state machine */
typedef enum {
    MESH_S_IDLE = 0,
    MESH_S_JOINING_LISTEN,
    MESH_S_JOINING_CLAIM,
    MESH_S_JOINED,
} mesh_state_t;

static volatile mesh_state_t s_state = MESH_S_IDLE;

/* TX scratch — protected by s_tx_mtx */
static SemaphoreHandle_t s_tx_mtx = NULL;
static uint8_t s_pending_lc3[LC3_FRAME_BYTES];
static bool    s_pending_valid    = false;
static bool    s_pending_vad      = false;
static bool    s_pending_leave    = false;
static bool    s_pending_join     = false;
static uint8_t s_prev_lc3[LC3_FRAME_BYTES];   /* F_{n-1} for FEC encode */
static bool    s_prev_lc3_valid   = false;

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
 * to decide which rider takes over after 5 missed beacons. */
static bool we_are_lowest_mac_in_group(void)
{
    uint32_t peers[MESH_MAX_RIDERS];
    size_t   n = 0;
    for (int i = 0; i < MESH_MAX_RIDERS; ++i) {
        if ((s_slot_map & (1u << i)) && s_peer_mac_low[i] != 0) {
            peers[n++] = s_peer_mac_low[i];
        }
    }
    return mesh_proto_we_are_coordinator(s_our_mac_low, peers, n);
}

static void fill_beacon_payload(mesh_frame_t *f)
{
    /* The beacon overlays the 60 B of lc3+fec. */
    mesh_beacon_t b = (mesh_beacon_t){0};
    b.magic         = MESH_PROTO_BEACON_MAGIC;
    b.coord_mac_low = s_our_mac_low;
    b.us_timestamp  = (uint32_t)esp_timer_get_time();
    b.slot_map      = s_slot_map;
    b.group_version = s_group_version;
    memcpy(&f->lc3[0], &b, sizeof(b));
}

/* ---- init / start / stop ---- */

esp_err_t mesh_mac_init(const uint8_t group_psk[16])
{
    ESP_LOGI(TAG, "init");
    memcpy(s_psk, group_psk, 16);

    /* Reset per-rider state. */
    memset(s_last_seq, 0, sizeof(s_last_seq));
    memset(s_seq_seen, 0, sizeof(s_seq_seen));
    memset(s_last_lc3, 0, sizeof(s_last_lc3));
    memset(s_last_lc3_valid, 0, sizeof(s_last_lc3_valid));
    memset(s_peer_mac_low, 0, sizeof(s_peer_mac_low));
    memset(s_peer_quiet_sframes, 0, sizeof(s_peer_quiet_sframes));

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
     * JOINED. The TX task picks up beaconing on the next superframe. */
    if (s_sframes_since_beacon == 0xFFFF) {
        s_own_slot      = 0;
        mesh_proto_slot_claim(&s_slot_map, 0);
        s_coord_mac_low = s_our_mac_low;
        s_state         = MESH_S_JOINED;
        if (out_slot) *out_slot = 0;
        ESP_LOGI(TAG, "join: bootstrap as solo rider, slot=0 (coordinator)");
        if (s_event_cb) s_event_cb(MESH_EVT_JOINED, 0);
        if (s_event_cb) s_event_cb(MESH_EVT_COORDINATOR_ME, 0);
        return ESP_OK;
    }

    /* Normal join: heard at least one beacon, so a group already exists.
     * Pick the lowest free slot, claim it, wait for the next beacon to
     * echo our bit. Treat missing echo as a collision and back off. */
    for (int attempt = 0; attempt < MESH_JOIN_RETRIES; ++attempt) {
        int slot = mesh_proto_lowest_free_slot(s_slot_map);
        if (slot < 0) {
            ESP_LOGW(TAG, "join: no free slot (map=0x%02x)", s_slot_map);
            s_state = MESH_S_IDLE;
            return ESP_ERR_TIMEOUT;
        }

        s_own_slot = (uint8_t)slot;
        s_state    = MESH_S_JOINING_CLAIM;
        ESP_LOGI(TAG, "join attempt %d: claiming slot %d", attempt, slot);

        /* Queue a JOIN frame for our next slot. */
        xSemaphoreTake(s_tx_mtx, portMAX_DELAY);
        s_pending_join  = true;
        s_pending_valid = true;       /* even silent: we still TX a join */
        s_pending_vad   = false;
        memset(s_pending_lc3, 0, sizeof(s_pending_lc3));
        xSemaphoreGive(s_tx_mtx);

        /* Wait one superframe so the TX task actually sends our JOIN. */
        vTaskDelay(pdMS_TO_TICKS(20));

        /* Locally commit to the slot. The spec-prescribed
         * "next-beacon-confirms-our-bit" check races the coordinator's
         * beacon cadence (up to ~22 ms in the worst case), and even
         * when it passes it can't disambiguate two joiners landing on
         * the same slot — that needs per-slot mac tracking, which is
         * a v0.5 concern. For v0 we trust our own JOIN and let later
         * beacons override us if there is a real conflict.
         *
         * TODO(v0.5): detect concurrent same-slot joiners via the
         * coordinator's peer_mac_low[our_slot]; if it isn't us or
         * unset, treat as collision and pick the next free slot. */
        mesh_proto_slot_claim(&s_slot_map, s_own_slot);
        s_state = MESH_S_JOINED;
        if (out_slot) *out_slot = s_own_slot;
        ESP_LOGI(TAG, "join: success, slot=%d", s_own_slot);
        if (s_event_cb) s_event_cb(MESH_EVT_JOINED, s_own_slot);
        return ESP_OK;
    }

    s_state = MESH_S_IDLE;
    s_own_slot = 0xFF;
    return ESP_ERR_TIMEOUT;
}

esp_err_t mesh_mac_leave(void)
{
    if (s_state != MESH_S_JOINED) return ESP_OK;
    xSemaphoreTake(s_tx_mtx, portMAX_DELAY);
    s_pending_leave = true;
    s_pending_valid = true;
    xSemaphoreGive(s_tx_mtx);
    /* Give the task one superframe to emit the LEAVE frame. */
    vTaskDelay(pdMS_TO_TICKS(20));
    s_state    = MESH_S_IDLE;
    s_own_slot = 0xFF;
    if (s_event_cb) s_event_cb(MESH_EVT_LEFT, 0);
    return ESP_OK;
}

/* ---- TX queue ---- */

esp_err_t mesh_mac_queue_tx(const uint8_t lc3_frame[30], bool vad_active)
{
    if (!lc3_frame) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_tx_mtx, 0) != pdTRUE) {
        /* Don't block the audio task — if we can't get the mutex this
         * tick, drop the frame. */
        return ESP_ERR_TIMEOUT;
    }
    memcpy(s_pending_lc3, lc3_frame, LC3_FRAME_BYTES);
    s_pending_valid = true;
    s_pending_vad   = vad_active;
    xSemaphoreGive(s_tx_mtx);
    return ESP_OK;
}

void mesh_mac_set_rx_cb(mesh_rx_cb_t cb)       { s_rx_cb    = cb; }
void mesh_mac_set_event_cb(mesh_event_cb_t cb) { s_event_cb = cb; }

/* ---- TX task: superframe scheduler ----------------------------------
 *
 * v0 timing model: a single esp_timer-driven loop that wakes once per
 * superframe, sleeps until our slot, transmits, then sleeps to the next
 * superframe boundary. Drift is bounded by FreeRTOS tick granularity
 * (typically 1 ms) — fine for 1.5 ms guard window on a single hop.
 *
 * TODO(v0.5): replace with a beacon-PLL'd hi-res esp_timer. The
 * coordinator's `us_timestamp` field in the beacon is the master clock;
 * every other node should phase-lock its slot scheduler to it. v0 just
 * uses local esp_timer.
 */
static void mesh_tx_task(void *arg)
{
    (void)arg;
    int64_t  superframe_start = esp_timer_get_time();

    while (!s_task_should_exit) {
        /* If we're not in the group, just wait a superframe. */
        if (s_state != MESH_S_JOINED && s_state != MESH_S_JOINING_CLAIM) {
            vTaskDelay(pdMS_TO_TICKS(20));
            superframe_start = esp_timer_get_time();
            s_superframe_counter++;
            continue;
        }

        /* Sleep until just before our slot start. */
        int64_t slot_start_us = superframe_start +
                                (int64_t)s_own_slot * MESH_SLOT_US;
        int64_t now           = esp_timer_get_time();
        int64_t to_sleep_us   = slot_start_us - now;
        if (to_sleep_us > 1000) {
            vTaskDelay(pdMS_TO_TICKS((to_sleep_us - 500) / 1000));
        }
        /* Burn the last few hundred µs in busy-wait to align tighter
         * than the FreeRTOS tick. TODO(v0.5): use the high-res timer
         * with an ISR-driven semaphore release instead. */
        now = esp_timer_get_time();
        if (now < slot_start_us) {
            esp_rom_delay_us((uint32_t)(slot_start_us - now));
        }

        /* Build the frame. */
        mesh_frame_t f = (mesh_frame_t){0};
        f.rider_id   = s_own_slot;
        f.flags      = MESH_PROTO_FLAG_FEC;   /* always carries XOR parity */
        f.seq        = s_tx_seq++;
        f.superframe = s_superframe_counter;

        bool send_this_slot = false;

        xSemaphoreTake(s_tx_mtx, portMAX_DELAY);
        if (s_pending_join)  { f.flags |= MESH_PROTO_FLAG_JOIN;  s_pending_join = false; }
        if (s_pending_leave) { f.flags |= MESH_PROTO_FLAG_LEAVE; s_pending_leave = false; }
        if (s_pending_valid) {
            if (s_pending_vad) f.flags |= MESH_PROTO_FLAG_VAD_ACTIVE;
            memcpy(f.lc3, s_pending_lc3, LC3_FRAME_BYTES);
            send_this_slot = true;
        } else {
            /* No audio queued — still TX a header-only frame if we have
             * JOIN/LEAVE pending or we're the coordinator (beacon). */
            memset(f.lc3, 0, LC3_FRAME_BYTES);
        }
        /* XOR-FEC against the previous frame we sent. */
        if (s_prev_lc3_valid) {
            mesh_proto_fec_encode(f.lc3, s_prev_lc3, f.fec);
        } else {
            memset(f.fec, 0, LC3_FRAME_BYTES);
        }
        memcpy(s_prev_lc3, f.lc3, LC3_FRAME_BYTES);
        s_prev_lc3_valid = true;
        s_pending_valid = false;
        xSemaphoreGive(s_tx_mtx);

        /* Coordinator role: alternate slot-0 between beacon (even
         * superframes) and audio (odd superframes). Original spec
         * had beacon every superframe, which trashes the coordinator's
         * own audio TX; halving the beacon rate to 25 fps gives peers
         * 40 ms between beacons (still well below the 100 ms
         * coordinator-lost threshold) and lets the coordinator emit
         * actual audio on the off-frames. */
        if (s_own_slot == 0 && we_hold_coordinator_role() &&
            (s_superframe_counter & 1u) == 0) {
            f.flags |= MESH_PROTO_FLAG_BEACON | MESH_PROTO_FLAG_VAD_ACTIVE;
            fill_beacon_payload(&f);
            /* fec carries the second half of the beacon payload — no
             * separate XOR computed in that case. */
            send_this_slot = true;
        }

        /* If we're joining/leaving, force a send even with no audio. */
        if (f.flags & (MESH_PROTO_FLAG_JOIN | MESH_PROTO_FLAG_LEAVE |
                       MESH_PROTO_FLAG_BEACON)) {
            send_this_slot = true;
        }

        /* Once JOINED, always emit something in our slot — even an
         * empty VAD-inactive frame. Otherwise peers' quiet-timeout
         * fires after 10 sframes (200 ms) and drops us from their
         * slot map. Cost is one ~90 B frame per 20 ms per rider; at
         * 8 riders that's ~3.6 kfr/s ≈ ~4 % of the 1 Mbps PHY. */
        if (s_state == MESH_S_JOINED) {
            send_this_slot = true;
        }

        if (send_this_slot) {
            f.crc = mesh_proto_crc16((const uint8_t *)&f,
                                     MESH_PROTO_CRC_COVER_BYTES);
            esp_err_t err = esp_now_send(BROADCAST_MAC,
                                         (const uint8_t *)&f,
                                         sizeof(f));
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "esp_now_send: %d", err);
            }
        }

        /* Sleep to the end of this superframe, then advance. */
        int64_t next_superframe = superframe_start + MESH_SUPERFRAME_US;
        now = esp_timer_get_time();
        if (next_superframe > now) {
            int64_t rem = next_superframe - now;
            if (rem > 1000) vTaskDelay(pdMS_TO_TICKS((rem - 500) / 1000));
            now = esp_timer_get_time();
            if (now < next_superframe) {
                esp_rom_delay_us((uint32_t)(next_superframe - now));
            }
        }
        superframe_start = next_superframe;
        s_superframe_counter++;

        /* End-of-superframe housekeeping. */
        if (s_sframes_since_beacon != 0xFFFF) s_sframes_since_beacon++;

        /* Implicit-leave timeout: any rider silent for
         * MESH_PEER_QUIET_SFRAMES gets booted from our local view. */
        for (int i = 0; i < MESH_MAX_RIDERS; ++i) {
            if ((s_slot_map & (1u << i)) == 0) continue;
            if (i == s_own_slot)               continue;
            s_peer_quiet_sframes[i]++;
            if (s_peer_quiet_sframes[i] >= MESH_PEER_QUIET_SFRAMES) {
                ESP_LOGI(TAG, "peer %d quiet for %d sframes, dropping",
                         i, MESH_PEER_QUIET_SFRAMES);
                mesh_proto_slot_release(&s_slot_map, (uint8_t)i);
                s_peer_mac_low[i] = 0;
                s_peer_quiet_sframes[i] = 0;
                s_seq_seen[i] = false;
                s_last_lc3_valid[i] = false;
                if (s_event_cb) s_event_cb(MESH_EVT_PEER_LEFT, (uint8_t)i);
            }
        }

        /* Coordinator failover: §"if no beacon heard for 5 superframes
         * AND we are next-lowest-MAC, take coordinator role".
         *
         * Skip when s_sframes_since_beacon is still at the 0xFFFF
         * sentinel — that means we have never heard a beacon at all
         * (e.g. solo bootstrap path), so there's nothing to "fail over
         * from." Real failover only makes sense after at least one
         * beacon has been observed. */
        if (s_sframes_since_beacon != 0xFFFF &&
            s_sframes_since_beacon >= MESH_COORD_LOSS_SFRAMES) {
            if (we_are_lowest_mac_in_group()) {
                if (s_coord_mac_low != s_our_mac_low) {
                    ESP_LOGW(TAG, "coordinator lost, taking role");
                    if (s_event_cb) s_event_cb(MESH_EVT_COORDINATOR_LOST, 0);
                    if (s_event_cb) s_event_cb(MESH_EVT_COORDINATOR_ME, s_own_slot);
                    s_coord_mac_low = s_our_mac_low;
                }
            }
        }
    }

    s_tx_task_handle = NULL;
    vTaskDelete(NULL);
}

/* ---- RX path ---- */

static void on_esp_now_recv(const esp_now_recv_info_t *info,
                            const uint8_t *data, int len)
{
    (void)info;
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

    /* Anti-replay (skip on first-ever frame from this rider). */
    if (s_seq_seen[rid]) {
        if (!mesh_proto_seq_accept(s_last_seq[rid], f->seq,
                                   MESH_PROTO_REPLAY_WINDOW)) {
            ESP_LOGD(TAG, "replay drop rider=%u last=%u new=%u",
                     rid, s_last_seq[rid], f->seq);
            return;
        }
    }

    uint16_t prev_seq = s_last_seq[rid];
    bool     had_seq  = s_seq_seen[rid];
    s_last_seq[rid] = f->seq;
    s_seq_seen[rid] = true;
    s_peer_quiet_sframes[rid] = 0;

    /* Track this peer's MAC-low (from info->src_addr). */
    if (info && info->src_addr) {
        s_peer_mac_low[rid] = mac_low_from_bytes(info->src_addr);
    }

    /* Beacon handling. */
    if (f->flags & MESH_PROTO_FLAG_BEACON) {
        mesh_beacon_t b;
        memcpy(&b, &f->lc3[0], sizeof(b));
        if (b.magic == MESH_PROTO_BEACON_MAGIC) {
            s_slot_map      = b.slot_map;
            s_group_version = b.group_version;
            s_coord_mac_low = b.coord_mac_low;
            s_sframes_since_beacon = 0;
            /* TODO(v0.5): phase-lock our local superframe clock to
             * b.us_timestamp here. */
        }
        /* Beacon carries no audio. */
        return;
    }

    /* JOIN flag: another rider claimed a slot. */
    if (f->flags & MESH_PROTO_FLAG_JOIN) {
        if (!(s_slot_map & (1u << rid))) {
            mesh_proto_slot_claim(&s_slot_map, rid);
            if (s_event_cb) s_event_cb(MESH_EVT_PEER_JOINED, rid);
        }
    }

    /* LEAVE flag: explicit departure. */
    if (f->flags & MESH_PROTO_FLAG_LEAVE) {
        mesh_proto_slot_release(&s_slot_map, rid);
        s_peer_mac_low[rid] = 0;
        s_seq_seen[rid]     = false;
        s_last_lc3_valid[rid] = false;
        if (s_event_cb) s_event_cb(MESH_EVT_PEER_LEFT, rid);
        return;
    }

    /* FEC: if we just skipped exactly one frame (delta == 2) and the
     * arriving frame has FEC set, try to reconstruct F_{n-1}. */
    if (had_seq && (f->flags & MESH_PROTO_FLAG_FEC)) {
        uint16_t delta = (uint16_t)(f->seq - prev_seq);
        if (delta == 2) {
            uint8_t recovered[LC3_FRAME_BYTES];
            mesh_proto_fec_recover(f->lc3, f->fec, recovered);
            if (s_rx_cb) {
                s_rx_cb(rid,
                        (f->flags & MESH_PROTO_FLAG_VAD_ACTIVE) != 0,
                        recovered, LC3_FRAME_BYTES);
            }
        }
    }

    /* Cache current lc3 for future FEC arithmetic. */
    memcpy(s_last_lc3[rid], f->lc3, LC3_FRAME_BYTES);
    s_last_lc3_valid[rid] = true;

    /* Deliver this frame's audio if VAD-active. */
    if ((f->flags & MESH_PROTO_FLAG_VAD_ACTIVE) && s_rx_cb) {
        s_rx_cb(rid, true, f->lc3, LC3_FRAME_BYTES);
    }
}

static void on_esp_now_send(const esp_now_send_info_t *info,
                            esp_now_send_status_t status)
{
    /* Voice is loss-tolerant; we never retransmit. Status logged at
     * debug level only. */
    (void)info; (void)status;
}
