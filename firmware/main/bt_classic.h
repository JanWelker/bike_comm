/*
 * bt_classic — Bluedroid HFP-HF + A2DP-sink to the rider's phone.
 *
 * We act as a Hands-Free unit (mic + speaker for calls) and an A2DP sink
 * (music receiver). AVRCP controller for transport keys.
 *
 * SCO audio path: HCI (CONFIG_BT_HFP_AUDIO_DATA_PATH_HCI=y) so PCM
 * arrives in firmware where we can route it through the mixer + AEC.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_bt_defs.h"

typedef enum {
    BT_STATE_OFF,
    BT_STATE_DISCOVERABLE,    /* waiting for a phone to pair */
    BT_STATE_CONNECTED,       /* paired and connected, idle  */
    BT_STATE_INCOMING_CALL,
    BT_STATE_IN_CALL,
    BT_STATE_A2DP_STREAMING,
} bt_state_t;

typedef enum {
    BT_EVT_CONNECTED,
    BT_EVT_DISCONNECTED,
    BT_EVT_INCOMING_CALL,
    BT_EVT_CALL_ESTABLISHED,
    BT_EVT_CALL_ENDED,
    BT_EVT_A2DP_START,
    BT_EVT_A2DP_STOP,
} bt_event_t;

typedef void (*bt_event_cb_t)(bt_event_t evt);

esp_err_t bt_classic_init(void);

/* Enter discoverable mode for `seconds` (0 = until canceled). */
esp_err_t bt_classic_pair(uint16_t seconds);

esp_err_t bt_classic_answer_call(void);
esp_err_t bt_classic_end_call(void);

bt_state_t bt_classic_get_state(void);

void       bt_classic_set_event_cb(bt_event_cb_t cb);
