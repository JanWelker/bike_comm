/*
 * mixer — N-stream jitter buffers + decode pool + summing mixer.
 *
 * Each remote rider has a small jitter buffer (3 frames = 30 ms).
 * On every 10 ms tick the mixer pulls one frame per rider (running
 * PLC if the buffer is empty), sums them at unity gain (with per-stream
 * duck), adds the phone audio path, and writes:
 *   - to the speaker output
 *   - to the AEC reference (so other riders' echoes cancel)
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

#define MIXER_JITTER_FRAMES  3   /* 30 ms of headroom */

esp_err_t mixer_init(void);

/* Push an LC3 frame received for a given rider into their jitter buffer.
 * Called from mesh_mac's RX callback. */
void mixer_push_remote_frame(uint8_t rider_id, const uint8_t *lc3, uint16_t len);

/* Push PCM audio from the phone path (HFP RX or A2DP). */
void mixer_push_phone_pcm(const int16_t pcm[160]);

/* Pull the next mixed 10 ms frame for the speaker. Also exposes the
 * same frame as the AEC reference signal. */
void mixer_pull_speaker_frame(int16_t out[160]);

/* Set the duck attenuation applied to mesh streams when a phone call
 * is in progress. Range: 0 dB (no duck) to -24 dB. */
void mixer_set_mesh_duck_db(int8_t duck_db);
