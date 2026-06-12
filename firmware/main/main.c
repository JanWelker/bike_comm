/*
 * bike_comm — open-source motorcycle helmet intercom
 *
 * Module init order matters here:
 *   1. NVS (config + BT pairing keys)
 *   2. Network interface (Wi-Fi MAC for ESP-NOW; no AP association)
 *   3. BT controller + Bluedroid host
 *   4. Coex tuning (must come after both radios are up)
 *   5. App modules in dependency order:
 *        nvs_cfg -> audio_pipeline -> codec -> mixer
 *                                           -> mesh_mac
 *                                           -> bt_classic
 *        session_fsm + ui glue everything together
 */

#include <stdio.h>
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "nvs_flash.h"

#include "audio_pipeline.h"
#include "codec.h"
#include "mesh_mac.h"
#include "mixer.h"
#include "bt_classic.h"
#include "session_fsm.h"
#include "ui.h"
#include "nvs_cfg.h"
#include "coex.h"

#include "freertos/queue.h"

static const char *TAG = "main";

_Static_assert(CODEC_MAX_DECODERS == MESH_MAX_RIDERS,
               "one codec decoder per mesh rider");

/* Bridge from the ESP-NOW recv callback (wifi task) to the audio_io
 * task via a FreeRTOS queue. Anything the recv callback does itself
 * stalls the wifi task; even brief work is enough to drop beacons
 * and trip the coordinator-lost timer on a peer. Drain happens in
 * audio_io's normal 10 ms tick via mesh_rx_drain_to_mixer().
 *
 * mesh_mac calls on_mesh_rx once per LC3 frame, so a dual-frame mesh
 * packet causes two enqueues. The queue is depth 16 to absorb that
 * 2x without triggering back-pressure drops under nominal load. */
typedef struct {
    uint8_t  rider_id;
    bool     vad_active;
    uint16_t seq;                  /* wire seq, fed straight into the JB */
    uint8_t  len;
    uint8_t  lc3[CODEC_FRAME_BYTES];
} rx_msg_t;

static QueueHandle_t s_rx_queue = NULL;

static void on_mesh_rx(uint8_t rider_id, uint16_t seq, bool vad_active,
                       const uint8_t *lc3_frame, size_t len)
{
    if (rider_id >= MESH_MAX_RIDERS) return;
    if (len == 0 || len > CODEC_FRAME_BYTES) return;
    if (!s_rx_queue) return;

    rx_msg_t msg = { .rider_id = rider_id, .vad_active = vad_active,
                     .seq = seq, .len = (uint8_t)len };
    memcpy(msg.lc3, lc3_frame, len);
    /* Non-blocking: if the queue is full, drop the frame. JB + PLC
     * absorb it. */
    (void)xQueueSend(s_rx_queue, &msg, 0);
}

/* Drain everything currently queued and push into the mixer. Called
 * from audio_io once per 10 ms tick. */
void mesh_rx_drain_to_mixer(void)
{
    rx_msg_t msg;
    static uint32_t  s_drain_count[MESH_MAX_RIDERS] = {0};
    static TickType_t s_last_log_tick = 0;
    while (s_rx_queue && xQueueReceive(s_rx_queue, &msg, 0) == pdTRUE) {
        mixer_push_remote_frame(msg.rider_id, msg.seq,
                                msg.vad_active, msg.lc3, msg.len);
        s_drain_count[msg.rider_id]++;
    }
    TickType_t now = xTaskGetTickCount();
    if (now - s_last_log_tick >= pdMS_TO_TICKS(1000)) {
        ESP_LOGI(TAG, "rx drain: r0=%lu r1=%lu r2=%lu r3=%lu",
                 (unsigned long)s_drain_count[0],
                 (unsigned long)s_drain_count[1],
                 (unsigned long)s_drain_count[2],
                 (unsigned long)s_drain_count[3]);
        s_last_log_tick = now;
    }
}

/* Mesh events may be emitted from the ESP-NOW recv callback (wifi
 * task), which must not log or do FSM work — on_mesh_event only
 * enqueues; app_main drains the queue and does the real handling. */
typedef struct {
    mesh_event_t evt;
    uint8_t      rider_id;
} mesh_evt_msg_t;

static QueueHandle_t s_mesh_evt_queue = NULL;

static void on_mesh_event(mesh_event_t evt, uint8_t rider_id)
{
    if (!s_mesh_evt_queue) return;
    mesh_evt_msg_t m = { .evt = evt, .rider_id = rider_id };
    (void)xQueueSend(s_mesh_evt_queue, &m, 0);
}

static void handle_mesh_event(mesh_event_t evt, uint8_t rider_id)
{
    switch (evt) {
    case MESH_EVT_JOINED:           ESP_LOGI(TAG, "mesh: joined at slot %u", rider_id); break;
    case MESH_EVT_LEFT:              ESP_LOGI(TAG, "mesh: left"); break;
    case MESH_EVT_PEER_JOINED:       ESP_LOGI(TAG, "mesh: peer joined at slot %u", rider_id); break;
    case MESH_EVT_PEER_LEFT:         ESP_LOGI(TAG, "mesh: peer left at slot %u", rider_id); break;
    case MESH_EVT_COORDINATOR_LOST:  ESP_LOGW(TAG, "mesh: coordinator lost"); break;
    case MESH_EVT_COORDINATOR_ME:    ESP_LOGI(TAG, "mesh: we are now coordinator"); break;
    }

    /* A slot changing hands must clear the mixer's per-rider state:
     * the jitter buffer's last-pulled seq watermark otherwise rejects
     * the new (or rebooted) occupant's restarted seq numbers for up to
     * ~5 minutes, and a departed rider would keep its in_use flag and
     * burn PLC decodes. */
    if (evt == MESH_EVT_PEER_LEFT || evt == MESH_EVT_PEER_JOINED) {
        mixer_rider_reset(rider_id);
    }

    session_fsm_on_mesh(evt, rider_id);
}

static void platform_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    /* STA mode is non-negotiable: Espressif's coexist.html marks
     * ESP-NOW RX as "S" (stable in STA mode only) under every BR/EDR
     * coexistence state. APSTA/AP will degrade ESP-NOW RX (audio +
     * beacons) once BT Classic is enabled. Asserted below so a future
     * change can't silently swap us into a broken mode. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    wifi_mode_t actual_mode = WIFI_MODE_NULL;
    ESP_ERROR_CHECK(esp_wifi_get_mode(&actual_mode));
    if (actual_mode != WIFI_MODE_STA) {
        ESP_LOGE(TAG, "wifi mode is %d, not WIFI_MODE_STA (%d) — "
                      "ESP-NOW RX is undefined when BT Classic is on",
                 actual_mode, WIFI_MODE_STA);
        abort();
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BTDM));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());
}

void app_main(void)
{
    ESP_LOGI(TAG, "bike_comm starting (chip: ESP32 single-chip path)");

    platform_init();
    app_coex_init();

    ESP_ERROR_CHECK(nvs_cfg_init());
    ESP_ERROR_CHECK(audio_pipeline_init());
    ESP_ERROR_CHECK(codec_init());
    ESP_ERROR_CHECK(mixer_init());

    uint8_t psk[16];
    ESP_ERROR_CHECK(nvs_cfg_get_psk(psk));
    ESP_ERROR_CHECK(mesh_mac_init(psk));
    /* Event queue must exist before the callback is registered — events
     * can fire from the wifi task as soon as frames arrive. */
    s_mesh_evt_queue = xQueueCreate(8, sizeof(mesh_evt_msg_t));
    ESP_ERROR_CHECK(s_mesh_evt_queue ? ESP_OK : ESP_ERR_NO_MEM);
    mesh_mac_set_event_cb(on_mesh_event);
    /* Allocate the recv-queue before registering the wifi callback so
     * the very first frame after registration has somewhere to land.
     * audio_io drains this queue every tick. Depth 16: each mesh packet
     * may carry 2 LC3 frames (= 2 enqueues), so 16 covers ~4 packets'
     * worth of back-pressure. */
    s_rx_queue = xQueueCreate(16, sizeof(rx_msg_t));
    ESP_ERROR_CHECK(s_rx_queue ? ESP_OK : ESP_ERR_NO_MEM);
    mesh_mac_set_rx_cb(on_mesh_rx);

    ESP_ERROR_CHECK(bt_classic_init());
    session_fsm_init();
    ui_init();
    /* Route BT and button events into the session FSM — without these
     * wires the arbitration layer (mesh ducking, coex preference) is
     * dead code and SCO would start without coex_prefer_bt_call(). */
    bt_classic_set_event_cb(session_fsm_on_bt);
    ui_set_button_cb(session_fsm_on_button);

    /* Start the pipelines. Each module spawns its own FreeRTOS tasks
     * pinned per the plan (audio on Core 1, radio + control on Core 0). */
    audio_pipeline_start();
    ESP_ERROR_CHECK(mesh_mac_start());

    uint8_t slot;
    esp_err_t join_err = mesh_mac_join(&slot);
    if (join_err != ESP_OK) {
        ESP_LOGW(TAG, "mesh join failed: %s", esp_err_to_name(join_err));
    }

    ESP_LOGI(TAG, "bike_comm ready");

    /* app_main stays alive as the control-plane task: it drains the
     * deferred mesh events (queued from the wifi/mesh tasks) into the
     * log and the session FSM. */
    for (;;) {
        mesh_evt_msg_t m;
        if (xQueueReceive(s_mesh_evt_queue, &m, portMAX_DELAY) == pdTRUE) {
            handle_mesh_event(m.evt, m.rider_id);
        }
    }
}
