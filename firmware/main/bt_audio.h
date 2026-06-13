/*
 * bt_audio — SCO PCM data plane for HFP-HF.
 *
 * Bridges the 7.5 ms / 120-sample mSBC SCO cadence (or 7.5 ms / 60
 * sample CVSD) to the audio_pipeline's 10 ms / 160-sample tick. Two
 * small ring buffers absorb the cadence mismatch in both directions.
 *
 * Voice Over HCI is enabled (CONFIG_BT_HFP_AUDIO_DATA_PATH_HCI=y), so
 * the controller does mSBC encode/decode and we deal in raw PCM:
 *   16 kHz, 16-bit signed, mono.
 *
 * The control plane (HFP SLC, call state, audio state notifications)
 * lives in bt_classic.c; this module is the pure data path. They
 * communicate via bt_audio_on_sco_up / bt_audio_on_sco_down which
 * bt_classic calls from its HF callback when SCO comes up or down.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_hf_defs.h"

esp_err_t bt_audio_init(void);

/* Called from bt_classic.c's HF callback on AUDIO_STATE_EVT. `handle`
 * is the (e)SCO connection handle to address with audio_data_send;
 * `preferred_frame_bytes` is the AG's recommended TX chunk size (240 B
 * for mSBC, 120 B for CVSD). 0 falls back to mSBC. */
void bt_audio_on_sco_up(esp_hf_sync_conn_hdl_t handle,
                        uint16_t preferred_frame_bytes);
void bt_audio_on_sco_down(void);

bool bt_audio_sco_active(void);

/* Called from audio_pipeline's loopback_task once per 10 ms tick.
 * Both no-op when SCO is down — safe to call unconditionally.
 *
 *   pull_to_mixer: drain 160 samples from the RX ring (pad with
 *                  silence on underrun) and hand to the mixer's
 *                  phone slot.
 *   push_mic:      append 160 samples of post-NS mic PCM to the TX
 *                  ring and flush whole preferred-frame chunks to
 *                  the AG via esp_hf_client_audio_data_send. */
void bt_audio_sco_pull_to_mixer(void);
void bt_audio_sco_push_mic(const int16_t pcm[160]);
