/*
 * bt_classic — Bluedroid GAP + HFP-HF (Stages 1-2). A2DP-sink + AVRCP
 * land in Stage 5; SCO audio routing in Stage 3.
 *
 * This file:
 *   - device name + Class of Device (Wearable Headset, audio service)
 *   - SSP Just-Works pairing (IO cap = NoInputNoOutput; a helmet has
 *     no display and no PIN entry, so Just Works is the only viable
 *     pairing model and matches what Bluetooth headsets ship with)
 *   - HFP-HF service-level connection: passes AT/+CIEV events through
 *     and surfaces incoming/established/ended calls into the session FSM
 *   - first-boot discoverable, subsequent boots connectable-only +
 *     proactive esp_hf_client_connect to the persisted phone BDA
 *
 * GAP/HFP callbacks run on the Bluedroid BTC task. They can log and
 * touch in-RAM state, but must NOT block on anything that waits for
 * another BT event — that deadlocks the stack. emit() is fine: the
 * registered listener is session_fsm_on_bt which only mutates s_mode
 * + calls into other modules' non-blocking setters.
 */

#include "bt_classic.h"
#include "bt_audio.h"
#include "nvs_cfg.h"

#include <string.h>
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_hf_client_api.h"
#include "esp_hf_defs.h"

static const char *TAG = "bt";

#define BIKE_COMM_NAME "bike_comm"

/* Class of Device — phones use this to pick the icon and to know we
 * accept SCO + A2DP:
 *   bits 23-13: Service class. 0x100 (audio) set.
 *   bits 12-8 : Major device class. 0x04 = Audio/Video.
 *   bits 7-2  : Minor device class. 0x01 = Wearable Headset Device.
 * Composed value: 0x240404. Matches what commercial moto intercoms
 * advertise so phones already know how to treat us. */
#define BIKE_COMM_COD  0x240404u

static bt_state_t    s_state    = BT_STATE_OFF;
static bt_event_cb_t s_event_cb = NULL;

/* HFP indicator cache. The two CIND events arrive separately and the
 * "established / ended" edge is only visible when both are read
 * together (an INCOMING -> NO_SETUP transition with CALL=1 means
 * "answered", while INCOMING -> NO_SETUP with CALL=0 means "rejected
 * before pickup"). Default both to 0 = idle. */
static esp_hf_call_status_t       s_call       = ESP_HF_CALL_STATUS_NO_CALLS;
static esp_hf_call_setup_status_t s_call_setup = ESP_HF_CALL_SETUP_STATUS_IDLE;

/* Paired phone BDA cached so reconnect_paired_phone() and forget() can
 * use it without re-reading NVS. Loaded in bt_classic_init. */
static esp_bd_addr_t s_phone_bda;
static bool          s_phone_bda_valid = false;

static void emit(bt_event_t evt)
{
    if (s_event_cb) s_event_cb(evt);
}

static void set_state(bt_state_t next)
{
    if (next == s_state) return;
    ESP_LOGI(TAG, "state %d -> %d", s_state, next);
    s_state = next;
}

static void log_bda(const char *what, const esp_bd_addr_t bda)
{
    ESP_LOGI(TAG, "%s %02x:%02x:%02x:%02x:%02x:%02x",
             what, bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

/* Become discoverable + connectable (pairing mode) or connectable-only
 * (idle / paired). Phones initiate the reconnect on subsequent boots.
 *
 * Bluedroid sometimes returns ESP_ERR_INVALID_STATE for the first
 * set_scan_mode call right after enable() — log and continue rather
 * than abort; the next state change (or the AUTH_CMPL handler) will
 * reapply. */
static void scan_mode(bool discoverable)
{
    esp_bt_discovery_mode_t disc = discoverable
        ? ESP_BT_GENERAL_DISCOVERABLE
        : ESP_BT_NON_DISCOVERABLE;
    esp_err_t err = esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, disc);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_scan_mode(%s) -> %s",
                 discoverable ? "DISC" : "CONN",
                 esp_err_to_name(err));
    }
}

static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT: {
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            log_bda("paired", param->auth_cmpl.bda);
            esp_err_t err = nvs_cfg_set_phone_addr(&param->auth_cmpl.bda);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "nvs_cfg_set_phone_addr: %s",
                         esp_err_to_name(err));
            }
            memcpy(s_phone_bda, param->auth_cmpl.bda, sizeof(s_phone_bda));
            s_phone_bda_valid = true;
            scan_mode(false);
            /* Don't emit BT_EVT_CONNECTED here — pairing complete is not
             * the same as "phone is usable for a call". HFP SLC will
             * fire shortly (the phone initiates it post-pairing) and
             * the HF callback emits CONNECTED then. */
        } else {
            ESP_LOGW(TAG, "pairing failed, stat=%d",
                     param->auth_cmpl.stat);
        }
        break;
    }

    case ESP_BT_GAP_PIN_REQ_EVT:
        /* Legacy pairing fallback — modern phones use SSP so this
         * rarely fires, but if it does, "0000" is the universal
         * factory PIN. */
        ESP_LOGI(TAG, "PIN req (legacy) — replying 0000");
        esp_bt_pin_code_t pin = { '0', '0', '0', '0' };
        esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin);
        break;

    case ESP_BT_GAP_CFM_REQ_EVT:
        /* SSP Numeric Comparison — with IO cap = NoInputNoOutput on
         * both sides this collapses to Just Works; the stack still
         * raises this event and expects an explicit confirm. */
        ESP_LOGI(TAG, "SSP confirm req, num=%lu — accepting (Just Works)",
                 (unsigned long)param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;

    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(TAG, "SSP passkey notif: %lu",
                 (unsigned long)param->key_notif.passkey);
        break;

    case ESP_BT_GAP_KEY_REQ_EVT:
        ESP_LOGW(TAG, "SSP passkey request — no input, can't satisfy");
        break;

    case ESP_BT_GAP_MODE_CHG_EVT:
        /* Sniff transitions matter for coex: a sniff'd ACL frees
         * radio slots for ESP-NOW. Log so we can correlate with
         * mesh drain rates during the soak. */
        ESP_LOGI(TAG, "ACL mode change: %d",
                 param->mode_chg.mode);
        break;

    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
    case ESP_BT_GAP_RMT_SRVCS_EVT:
    case ESP_BT_GAP_RMT_SRVC_REC_EVT:
        /* Not interesting at this stage. */
        break;

    default:
        ESP_LOGD(TAG, "gap evt %d", event);
        break;
    }
}

static void hf_cb(esp_hf_client_cb_event_t event,
                  esp_hf_client_cb_param_t *param)
{
    switch (event) {
    case ESP_HF_CLIENT_CONNECTION_STATE_EVT:
        ESP_LOGI(TAG, "HF conn state: %d", param->conn_stat.state);
        switch (param->conn_stat.state) {
        case ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED:
            /* Service Level Connection up — phone is fully usable now.
             * This is the real "connected" edge for the session FSM. */
            ESP_LOGI(TAG, "HF SLC up, peer_feat=0x%lx",
                     (unsigned long)param->conn_stat.peer_feat);
            set_state(BT_STATE_CONNECTED);
            emit(BT_EVT_CONNECTED);
            break;
        case ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED:
            ESP_LOGI(TAG, "HF disconnected");
            s_call       = ESP_HF_CALL_STATUS_NO_CALLS;
            s_call_setup = ESP_HF_CALL_SETUP_STATUS_IDLE;
            /* Stay connectable so the phone can come back; only become
             * discoverable if the user explicitly forgets the pair. */
            scan_mode(false);
            set_state(s_phone_bda_valid ? BT_STATE_CONNECTED
                                        : BT_STATE_DISCOVERABLE);
            emit(BT_EVT_DISCONNECTED);
            break;
        default:
            break;
        }
        break;

    case ESP_HF_CLIENT_AUDIO_STATE_EVT:
        ESP_LOGI(TAG, "HF audio state: %d (%s) frame=%uB",
                 param->audio_stat.state,
                 param->audio_stat.state ==
                     ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC ? "mSBC WB"
                 : param->audio_stat.state ==
                     ESP_HF_CLIENT_AUDIO_STATE_CONNECTED ? "CVSD NB"
                 : "down",
                 (unsigned)param->audio_stat.preferred_frame_size);
        if (param->audio_stat.state ==
                ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC ||
            param->audio_stat.state ==
                ESP_HF_CLIENT_AUDIO_STATE_CONNECTED) {
            bt_audio_on_sco_up(param->audio_stat.sync_conn_handle,
                               param->audio_stat.preferred_frame_size);
        } else {
            bt_audio_on_sco_down();
        }
        break;

    case ESP_HF_CLIENT_CIND_CALL_EVT:
        ESP_LOGI(TAG, "HF +CIEV call: %d", param->call.status);
        /* The CALL_ENDED edge is "was IN_CALL or had pending setup,
         * now neither" — read both indicators together. */
        if (param->call.status == ESP_HF_CALL_STATUS_CALL_IN_PROGRESS &&
            s_call != ESP_HF_CALL_STATUS_CALL_IN_PROGRESS) {
            set_state(BT_STATE_IN_CALL);
            emit(BT_EVT_CALL_ESTABLISHED);
        } else if (param->call.status == ESP_HF_CALL_STATUS_NO_CALLS &&
                   (s_call == ESP_HF_CALL_STATUS_CALL_IN_PROGRESS ||
                    s_state == BT_STATE_INCOMING_CALL)) {
            set_state(BT_STATE_CONNECTED);
            emit(BT_EVT_CALL_ENDED);
        }
        s_call = param->call.status;
        break;

    case ESP_HF_CLIENT_CIND_CALL_SETUP_EVT:
        ESP_LOGI(TAG, "HF +CIEV setup: %d", param->call_setup.status);
        if (param->call_setup.status ==
                ESP_HF_CALL_SETUP_STATUS_INCOMING &&
            s_state != BT_STATE_INCOMING_CALL &&
            s_state != BT_STATE_IN_CALL) {
            set_state(BT_STATE_INCOMING_CALL);
            emit(BT_EVT_INCOMING_CALL);
        }
        s_call_setup = param->call_setup.status;
        break;

    case ESP_HF_CLIENT_CLIP_EVT:
        /* Calling Line ID — the caller's number. Logged for debug;
         * routing to a "who's calling" TTS or UI is out of scope. */
        ESP_LOGI(TAG, "HF CLIP: %s",
                 param->clip.number ? param->clip.number : "(blocked)");
        break;

    case ESP_HF_CLIENT_RING_IND_EVT:
        /* RING tone from the AG. Useful as a heartbeat that the link
         * is alive; the actual ring audio comes via SCO. */
        ESP_LOGD(TAG, "HF RING");
        break;

    case ESP_HF_CLIENT_VOLUME_CONTROL_EVT:
        /* AG-driven volume sync. Forwarding to audio_pipeline_vol_*
         * lands in Stage 3 once SCO audio is flowing — until then,
         * adjusting speaker gain on a silent path is just confusing. */
        ESP_LOGI(TAG, "HF volume: type=%d val=%d",
                 param->volume_control.type, param->volume_control.volume);
        break;

    case ESP_HF_CLIENT_CIND_SERVICE_AVAILABILITY_EVT:
    case ESP_HF_CLIENT_CIND_SIGNAL_STRENGTH_EVT:
    case ESP_HF_CLIENT_CIND_ROAMING_STATUS_EVT:
    case ESP_HF_CLIENT_CIND_BATTERY_LEVEL_EVT:
    case ESP_HF_CLIENT_COPS_CURRENT_OPERATOR_EVT:
    case ESP_HF_CLIENT_BSIR_EVT:
    case ESP_HF_CLIENT_AT_RESPONSE_EVT:
    case ESP_HF_CLIENT_PROF_STATE_EVT:
        /* Network / status / profile-state chatter — not actionable
         * for v0.5. Log at DEBUG so it's available with verbose. */
        ESP_LOGD(TAG, "HF evt %d", event);
        break;

    default:
        ESP_LOGD(TAG, "HF evt %d (unhandled)", event);
        break;
    }
}

/* Attempt SLC to the persisted phone. Bluedroid returns FAIL if the
 * phone is out of range; logged and retried implicitly on the phone's
 * next reconnect attempt, which most phones initiate periodically. */
static void reconnect_paired_phone(void)
{
    if (!s_phone_bda_valid) return;
    esp_err_t err = esp_hf_client_connect(s_phone_bda);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "hf_client_connect: %s (will wait for phone)",
                 esp_err_to_name(err));
    }
}

esp_err_t bt_classic_init(void)
{
    ESP_LOGI(TAG, "init (GAP + HFP-HF; A2DP/AVRCP land in stage 5)");

    ESP_ERROR_CHECK(esp_bt_gap_register_callback(gap_cb));
    ESP_ERROR_CHECK(esp_bt_gap_set_device_name(BIKE_COMM_NAME));
    ESP_ERROR_CHECK(esp_bt_gap_set_cod(
        (esp_bt_cod_t){ .reserved_2 = 0,
                        .minor      = (BIKE_COMM_COD >> 2) & 0x3f,
                        .major      = (BIKE_COMM_COD >> 8) & 0x1f,
                        .service    = (BIKE_COMM_COD >> 13) & 0x7ff },
        ESP_BT_INIT_COD));

    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    ESP_ERROR_CHECK(esp_bt_gap_set_security_param(
        ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap)));

    ESP_ERROR_CHECK(esp_hf_client_register_callback(hf_cb));
    ESP_ERROR_CHECK(esp_hf_client_init());

    if (nvs_cfg_get_phone_addr(&s_phone_bda) == ESP_OK) {
        s_phone_bda_valid = true;
        log_bda("known phone:", s_phone_bda);
        /* Stay non-discoverable but connectable. We proactively try
         * SLC; the phone will accept it if in range and reject (or
         * silently drop) if not. The BT_STATE here is the placeholder
         * "we have a bond" — real CONNECTED edge comes from HF SLC up. */
        scan_mode(false);
        set_state(BT_STATE_CONNECTED);
        reconnect_paired_phone();
    } else {
        ESP_LOGI(TAG, "no paired phone — discoverable");
        s_phone_bda_valid = false;
        scan_mode(true);
        set_state(BT_STATE_DISCOVERABLE);
    }
    return ESP_OK;
}

esp_err_t bt_classic_pair(uint16_t seconds)
{
    (void)seconds;  /* TODO: arm a timer to drop back to non-discoverable */
    scan_mode(true);
    set_state(BT_STATE_DISCOVERABLE);
    return ESP_OK;
}

esp_err_t bt_classic_answer_call(void)
{
    /* esp_hf_client_answer_call sends ATA. The AG responds with a
     * +CIEV update which flips call_status -> IN_PROGRESS, and the
     * resulting CIND_CALL_EVT emits BT_EVT_CALL_ESTABLISHED. We don't
     * pre-emptively transition the state machine here — the AG might
     * reject or fail to set up SCO, and we'd otherwise lie to the FSM. */
    return esp_hf_client_answer_call();
}

esp_err_t bt_classic_end_call(void)
{
    /* AT+CHUP — works for both "reject incoming" and "hang up active". */
    return esp_hf_client_reject_call();
}

esp_err_t bt_classic_forget_phone(void)
{
    if (s_phone_bda_valid) {
        /* Tear down the HFP SLC first; Bluedroid otherwise keeps the
         * RFCOMM session open and the bond-remove silently no-ops on
         * an active link. */
        esp_hf_client_disconnect(s_phone_bda);
        /* Drop the bond too, otherwise the stack would silently
         * re-accept the old phone on its next pairing attempt. */
        esp_bt_gap_remove_bond_device(s_phone_bda);
    }
    nvs_cfg_clear_phone_addr();
    s_phone_bda_valid = false;
    scan_mode(true);
    set_state(BT_STATE_DISCOVERABLE);
    ESP_LOGI(TAG, "forgot phone — discoverable");
    return ESP_OK;
}

bt_state_t bt_classic_get_state(void)        { return s_state; }
void bt_classic_set_event_cb(bt_event_cb_t cb){ s_event_cb = cb; }
void bt_classic_external_emit(bt_event_t evt) { emit(evt); }
