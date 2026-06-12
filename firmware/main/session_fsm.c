/*
 * session_fsm — intercom ↔ phone-call arbitration.
 *
 * The policy lives here, not in the audio modules; the audio modules
 * just expose levers (mesh_duck_db, mic routing, etc).
 */

#include "session_fsm.h"
#include "mixer.h"
#include "bt_classic.h"
#include "mesh_mac.h"
#include "coex.h"
#include "ui.h"
#include "audio_pipeline.h"

#include "esp_log.h"

static const char *TAG = "session";

static session_mode_t s_mode = SESSION_IDLE;

static void apply_mode(session_mode_t next)
{
    switch (next) {
    case SESSION_IDLE:
        ui_set_led(LED_IDLE);
        mixer_set_mesh_duck_db(0);
        coex_prefer_balanced();
        break;
    case SESSION_MESH_ONLY:
        ui_set_led(LED_MESH_JOINED);
        mixer_set_mesh_duck_db(0);
        coex_prefer_wifi();
        break;
    case SESSION_PHONE_CALL:
    case SESSION_PHONE_CALL_WITH_MESH:
        ui_set_led(LED_PHONE_CONNECTED);
        /* Phone call DOES NOT mute mesh — see plan. Duck -12 dB. */
        mixer_set_mesh_duck_db(12);
        coex_prefer_bt_call();
        break;
    case SESSION_A2DP_ONLY:
    case SESSION_A2DP_WITH_MESH:
        ui_set_led(LED_PHONE_CONNECTED);
        mixer_set_mesh_duck_db(6);
        coex_prefer_balanced();
        break;
    }
}

static void enter(session_mode_t next)
{
    if (next == s_mode) return;
    ESP_LOGI(TAG, "%d -> %d", s_mode, next);
    s_mode = next;
    apply_mode(next);
}

void session_fsm_init(void)
{
    /* Apply the IDLE side effects unconditionally — s_mode already
     * equals SESSION_IDLE, so enter() would no-op and the initial LED
     * state / coex preference would never be set. */
    s_mode = SESSION_IDLE;
    apply_mode(SESSION_IDLE);
}

session_mode_t session_fsm_get_mode(void) { return s_mode; }

void session_fsm_on_button(button_event_t evt)
{
    switch (evt) {
    case BTN_EVT_MODE_SHORT:
        /* Accept an incoming call if one is ringing. bt_classic owns
         * the underlying state; we just forward the user intent. */
        if (bt_classic_get_state() == BT_STATE_INCOMING_CALL) {
            bt_classic_answer_call();
        }
        break;
    case BTN_EVT_MODE_LONG:
        bt_classic_end_call();
        break;
    case BTN_EVT_VOL_UP:
        audio_pipeline_vol_step(+5);
        break;
    case BTN_EVT_VOL_DOWN:
        audio_pipeline_vol_step(-5);
        break;
    case BTN_EVT_ALL_HELD:
        /* Long combo: reset pairing / regenerate group PSK. */
        break;
    default:
        break;
    }
}

void session_fsm_on_bt(bt_event_t evt)
{
    switch (evt) {
    case BT_EVT_CONNECTED:
        enter((s_mode == SESSION_MESH_ONLY) ?
              SESSION_PHONE_CALL_WITH_MESH : SESSION_IDLE);
        break;
    case BT_EVT_INCOMING_CALL:
        /* UI flashes; user presses MODE_SHORT to accept. */
        break;
    case BT_EVT_CALL_ESTABLISHED:
        enter((s_mode == SESSION_MESH_ONLY) ?
              SESSION_PHONE_CALL_WITH_MESH : SESSION_PHONE_CALL);
        break;
    case BT_EVT_CALL_ENDED:
        enter((s_mode == SESSION_PHONE_CALL_WITH_MESH) ?
              SESSION_MESH_ONLY : SESSION_IDLE);
        break;
    case BT_EVT_A2DP_START:
        enter((s_mode == SESSION_MESH_ONLY) ?
              SESSION_A2DP_WITH_MESH : SESSION_A2DP_ONLY);
        break;
    case BT_EVT_A2DP_STOP:
        enter((s_mode == SESSION_A2DP_WITH_MESH) ?
              SESSION_MESH_ONLY : SESSION_IDLE);
        break;
    default:
        break;
    }
}

void session_fsm_on_mesh(mesh_event_t evt, uint8_t rider_id)
{
    (void)rider_id;
    switch (evt) {
    case MESH_EVT_JOINED:
        if (s_mode == SESSION_IDLE)        enter(SESSION_MESH_ONLY);
        if (s_mode == SESSION_PHONE_CALL)  enter(SESSION_PHONE_CALL_WITH_MESH);
        if (s_mode == SESSION_A2DP_ONLY)   enter(SESSION_A2DP_WITH_MESH);
        break;
    case MESH_EVT_LEFT:
        if (s_mode == SESSION_MESH_ONLY)            enter(SESSION_IDLE);
        if (s_mode == SESSION_PHONE_CALL_WITH_MESH) enter(SESSION_PHONE_CALL);
        if (s_mode == SESSION_A2DP_WITH_MESH)       enter(SESSION_A2DP_ONLY);
        break;
    default:
        break;
    }
}
