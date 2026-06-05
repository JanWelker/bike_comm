/*
 * bt_classic — Bluedroid HFP-HF + A2DP-sink.
 *
 * Status: skeleton. Real impl follows ESP-IDF samples at
 *   examples/bluetooth/bluedroid/classic_bt/{hfp_hf,a2dp_sink}.
 */

#include "bt_classic.h"

#include <string.h>
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"

static const char *TAG = "bt";

static bt_state_t    s_state    = BT_STATE_OFF;
static bt_event_cb_t s_event_cb = NULL;

static void emit(bt_event_t evt)
{
    if (s_event_cb) s_event_cb(evt);
}

esp_err_t bt_classic_init(void)
{
    ESP_LOGI(TAG, "init (HFP-HF + A2DP-sink)");

    esp_bt_gap_set_device_name("bike_comm");
    /* TODO:
     *   - esp_bt_gap_register_callback(gap_cb)
     *   - esp_hf_client_register_callback / esp_hf_client_init
     *   - esp_a2d_register_callback / esp_a2d_sink_init
     *   - esp_avrc_ct_register_callback / esp_avrc_ct_init
     *   - configure SSP, IO cap, COD
     */

    s_state = BT_STATE_DISCOVERABLE;
    return ESP_OK;
}

esp_err_t bt_classic_pair(uint16_t seconds)
{
    (void)seconds;
    /* esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE); */
    s_state = BT_STATE_DISCOVERABLE;
    return ESP_OK;
}

esp_err_t bt_classic_answer_call(void)
{
    /* esp_hf_client_answer_call(...) */
    s_state = BT_STATE_IN_CALL;
    emit(BT_EVT_CALL_ESTABLISHED);
    return ESP_OK;
}

esp_err_t bt_classic_end_call(void)
{
    /* esp_hf_client_reject_call / hangup */
    s_state = BT_STATE_CONNECTED;
    emit(BT_EVT_CALL_ENDED);
    return ESP_OK;
}

bt_state_t bt_classic_get_state(void)        { return s_state; }
void bt_classic_set_event_cb(bt_event_cb_t cb){ s_event_cb = cb; }
