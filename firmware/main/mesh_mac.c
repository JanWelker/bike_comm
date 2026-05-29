/*
 * mesh_mac — TDMA on ESP-NOW.
 *
 * Status: skeleton. The hard parts (beacon-PLL time sync, slot
 * arbitration, XOR-FEC) get implemented in v0/v0.5. For v0 the goal
 * is a 2-rider deterministic alternating-slot version.
 *
 * See docs/mesh_protocol.md for the spec.
 */

#include "mesh_mac.h"
#include "codec_lc3.h"   /* for LC3_FRAME_BYTES */

#include <string.h>
#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "mesh";

#define MESH_TASK_PRIO   19
#define MESH_CORE        0

static mesh_rx_cb_t    s_rx_cb    = NULL;
static mesh_event_cb_t s_event_cb = NULL;

static uint8_t s_psk[16];
static uint8_t s_own_slot   = 0xFF;      /* 0xFF = not joined */
static uint8_t s_slot_map   = 0;
static uint8_t s_coordinator_mac_low[4] = {0};
static uint32_t s_superframe_counter = 0;

/* On-air header (packed). */
typedef struct __attribute__((packed)) {
    uint8_t  rider_id;
    uint8_t  flags;
    uint16_t seq;
    uint16_t superframe;
    uint8_t  lc3[LC3_FRAME_BYTES /* 30 */];
    uint8_t  fec[LC3_FRAME_BYTES /* 30 */];
    uint16_t crc;
} mesh_frame_t;

_Static_assert(sizeof(mesh_frame_t) == MESH_FRAME_PAYLOAD_BYTES,
               "mesh_frame_t must be 68 bytes on the wire");

static void mesh_tx_task(void *arg);
static void on_esp_now_recv(const esp_now_recv_info_t *info,
                            const uint8_t *data, int len);

esp_err_t mesh_mac_init(const uint8_t group_psk[16])
{
    ESP_LOGI(TAG, "init");
    memcpy(s_psk, group_psk, 16);

    esp_err_t err = esp_now_init();
    if (err != ESP_OK) return err;

    err = esp_now_set_pmk(s_psk);
    if (err != ESP_OK) return err;

    err = esp_now_register_recv_cb(on_esp_now_recv);
    return err;
}

esp_err_t mesh_mac_start(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(mesh_tx_task, "mesh_tx", 4096,
                                            NULL, MESH_TASK_PRIO, NULL,
                                            MESH_CORE);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t mesh_mac_stop(void)
{
    /* TODO: signal task to exit. */
    return ESP_OK;
}

esp_err_t mesh_mac_join(uint8_t *out_slot)
{
    /* TODO:
     *   1. listen for >= 2 superframes (40 ms) for a beacon
     *   2. read slot_map_bitfield from beacon
     *   3. pick lowest free slot
     *   4. transmit in that slot with MESH_FLAG_JOIN set
     *   5. on second superframe, slot is ours
     *   6. on collision (no peer ack — see plan §2.4): backoff
     */
    s_own_slot = 0;
    if (out_slot) *out_slot = s_own_slot;
    if (s_event_cb) s_event_cb(MESH_EVT_JOINED, s_own_slot);
    return ESP_OK;
}

esp_err_t mesh_mac_leave(void)
{
    /* TODO: send LEAVE flag in our next TX, then stop TXing. */
    s_own_slot = 0xFF;
    return ESP_OK;
}

esp_err_t mesh_mac_queue_tx(const uint8_t lc3_frame[30], bool vad_active)
{
    (void)lc3_frame; (void)vad_active;
    /* TODO: copy into the slot's pending buffer; mesh_tx_task will
     * pick it up on its next slot tick. */
    return ESP_OK;
}

void mesh_mac_set_rx_cb(mesh_rx_cb_t cb)       { s_rx_cb    = cb; }
void mesh_mac_set_event_cb(mesh_event_cb_t cb) { s_event_cb = cb; }

/* ---- task + callback bodies ---- */

static void mesh_tx_task(void *arg)
{
    (void)arg;
    /* TODO: superframe scheduler. Use esp_timer or a high-res FreeRTOS
     * timer to fire every MESH_SUPERFRAME_US; within a superframe wait
     * until our own slot start; transmit; sleep. */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(20));
        s_superframe_counter++;
    }
}

static void on_esp_now_recv(const esp_now_recv_info_t *info,
                            const uint8_t *data, int len)
{
    if (len != (int)sizeof(mesh_frame_t)) return;
    const mesh_frame_t *f = (const mesh_frame_t *)data;
    /* TODO: validate CRC, check seq for replay, decode flags,
     *       handle JOIN/LEAVE/BEACON, run XOR-FEC reconstruction. */
    if (s_rx_cb) {
        s_rx_cb(f->rider_id,
                (f->flags & MESH_FLAG_VAD_ACTIVE) != 0,
                f->lc3, LC3_FRAME_BYTES);
    }
}
