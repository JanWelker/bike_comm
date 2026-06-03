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
#include "esp_err.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio_pipeline.h"
#include "codec_lc3.h"
#include "mesh_mac.h"
#include "mixer.h"
#include "bt_classic.h"
#include "session_fsm.h"
#include "ui.h"
#include "nvs_cfg.h"
#include "coex.h"

static const char *TAG = "main";

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
