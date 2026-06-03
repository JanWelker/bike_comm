/*
 * bike_comm — open-source motorcycle helmet intercom
 *
 * Module init order matters here:
 *   1. NVS (config + BT pairing keys)
 *   2. Network interface (Wi-Fi MAC for ESP-NOW; no AP association)
 *   3. BT controller + Bluedroid host
 *   4. Coex tuning (must come after both radios are up)
 *   5. App modules in dependency order:
 *        nvs_cfg -> audio_pipeline -> codec_lc3 -> mixer
 *                                              -> mesh_mac
 *                                              -> bt_classic
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
#include "codec_lc3.h"
#include "mesh_mac.h"
#include "mixer.h"
#include "bt_classic.h"
#include "session_fsm.h"
#include "ui.h"
#include "nvs_cfg.h"
#include "coex.h"

#include "freertos/queue.h"

static const char *TAG = "main";

/* Bridge from the ESP-NOW recv callback (wifi task) to the audio_io
 * task via a FreeRTOS queue. Anything the recv callback does itself
 * stalls the wifi task; even brief work is enough to drop beacons
 * and trip the coordinator-lost timer on a peer. Drain happens in
 * audio_io's normal 10 ms tick via mesh_rx_drain_to_mixer(). */
typedef struct {
    uint8_t  rider_id;
    bool     vad_active;
    uint8_t  len;
    uint8_t  lc3[LC3_FRAME_BYTES];
} rx_msg_t;

static QueueHandle_t s_rx_queue = NULL;
static uint16_t      s_rx_seq[8] = {0};

static void on_mesh_rx(uint8_t rider_id, bool vad_active,
                       const uint8_t *lc3_frame, size_t len)
{
    if (rider_id >= 8) return;
    if (len == 0 || len > LC3_FRAME_BYTES) return;
    if (!s_rx_queue) return;

    rx_msg_t msg = { .rider_id = rider_id, .vad_active = vad_active,
                     .len = (uint8_t)len };
    memcpy(msg.lc3, lc3_frame, len);
    /* Non-blocking: if the queue is full, drop the frame. JB + PLC
     * absorb it. The queue is depth 8 so this only happens under
     * unusual back-pressure. */
    (void)xQueueSend(s_rx_queue, &msg, 0);
}

/* Drain everything currently queued and push into the mixer. Called
 * from audio_io once per 10 ms tick. */
void mesh_rx_drain_to_mixer(void)
{
    rx_msg_t msg;
    static uint32_t  s_drain_count[8] = {0};
    static TickType_t s_last_log_tick = 0;
    while (s_rx_queue && xQueueReceive(s_rx_queue, &msg, 0) == pdTRUE) {
        mixer_push_remote_frame(msg.rider_id, s_rx_seq[msg.rider_id]++,
                                msg.vad_active, msg.lc3, msg.len);
        if (msg.rider_id < 8) s_drain_count[msg.rider_id]++;
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

static void on_mesh_event(mesh_event_t evt, uint8_t rider_id)
{
    switch (evt) {
    case MESH_EVT_JOINED:           ESP_LOGI(TAG, "mesh: joined at slot %u", rider_id); break;
    case MESH_EVT_LEFT:              ESP_LOGI(TAG, "mesh: left"); break;
    case MESH_EVT_PEER_JOINED:       ESP_LOGI(TAG, "mesh: peer joined at slot %u", rider_id); break;
    case MESH_EVT_PEER_LEFT:         ESP_LOGI(TAG, "mesh: peer left at slot %u", rider_id); break;
    case MESH_EVT_COORDINATOR_LOST:  ESP_LOGW(TAG, "mesh: coordinator lost"); break;
    case MESH_EVT_COORDINATOR_ME:    ESP_LOGI(TAG, "mesh: we are now coordinator"); break;
    }
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
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

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
    ESP_ERROR_CHECK(codec_lc3_init());
    ESP_ERROR_CHECK(mixer_init());

    uint8_t psk[16];
    ESP_ERROR_CHECK(nvs_cfg_get_psk(psk));
    ESP_ERROR_CHECK(mesh_mac_init(psk));
    mesh_mac_set_event_cb(on_mesh_event);
    /* Allocate the recv-queue before registering the wifi callback so
     * the very first frame after registration has somewhere to land.
     * audio_io drains this queue every tick. Depth 8 covers a brief
     * back-pressure burst. */
    s_rx_queue = xQueueCreate(8, sizeof(rx_msg_t));
    ESP_ERROR_CHECK(s_rx_queue ? ESP_OK : ESP_ERR_NO_MEM);
    mesh_mac_set_rx_cb(on_mesh_rx);

    ESP_ERROR_CHECK(bt_classic_init());
    session_fsm_init();
    ui_init();

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

    /* app_main returns; FreeRTOS continues to schedule the tasks. */
}
