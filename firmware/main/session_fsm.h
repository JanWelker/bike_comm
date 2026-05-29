/*
 * session_fsm — intercom ↔ phone-call arbitration.
 *
 * Audio routing matrix (see plan):
 *   - Idle              -> mic off, speaker off
 *   - Mesh only         -> mic -> mesh TX, mesh streams -> speaker
 *   - Phone call        -> mic -> HFP, mesh ducked into speaker
 *   - A2DP music        -> mesh into speaker on top of music
 *   - Call + mesh       -> mic to BOTH, mesh ducked -12 dB
 *
 * Policy: phone call does NOT mute mesh. Riders need safety yells through.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "bt_classic.h"
#include "mesh_mac.h"
#include "ui.h"

typedef enum {
    SESSION_IDLE,
    SESSION_MESH_ONLY,
    SESSION_PHONE_CALL,
    SESSION_PHONE_CALL_WITH_MESH,
    SESSION_A2DP_ONLY,
    SESSION_A2DP_WITH_MESH,
} session_mode_t;

void           session_fsm_init(void);
session_mode_t session_fsm_get_mode(void);

void session_fsm_on_button(button_event_t evt);
void session_fsm_on_bt(bt_event_t evt);
void session_fsm_on_mesh(mesh_event_t evt, uint8_t rider_id);
