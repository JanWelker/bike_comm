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
#include <stdbool.h>
#include "esp_err.h"

#define MIXER_JITTER_FRAMES  3    /* 30 ms of headroom */
#define MIXER_PCM_SAMPLES    160  /* 10 ms @ 16 kHz */

esp_err_t mixer_init(void);

/* Push an LC3 frame received for a given rider into their jitter buffer.
 * Called from mesh_mac's RX callback.
 *
 * Parameters:
 *   rider_id    — 0..7
 *   seq         — frame sequence number from the mesh transport (16-bit
 *                 wrap-aware). Used for ordering, dedup, and stale-drop.
 *   vad_active  — whether the transmitter flagged this frame as voice-
 *                 active. When false, the mixer skips LC3 decode for
 *                 this slot (treats it as silence) to save CPU.
 *   lc3         — payload bytes
 *   len         — payload length (expected = CODEC_FRAME_BYTES = 40)
 *
 * NOTE: signature changed in v0.x — was `(rider_id, lc3, len)`. The
 * mesh RX callback in mesh_mac must pass seq + vad through. */
void mixer_push_remote_frame(uint8_t rider_id,
                             uint16_t seq,
                             bool vad_active,
                             const uint8_t *lc3,
                             uint16_t len);

/* Push PCM audio from the phone path (HFP RX or A2DP). */
void mixer_push_phone_pcm(const int16_t pcm[MIXER_PCM_SAMPLES]);

/* Reset one rider's mixer state: clears the jitter buffer (including
 * its last-pulled seq watermark) and the in_use flag. Call on
 * MESH_EVT_PEER_LEFT / MESH_EVT_PEER_JOINED so (a) a departed rider
 * stops costing a PLC decode per tick and (b) a rebooted rider's fresh
 * seq numbers aren't rejected against the pre-reboot watermark. */
void mixer_rider_reset(uint8_t rider_id);

/* Pull the next mixed 10 ms frame.
 *
 * `speaker` is the signal that goes to the codec.
 * `aec_ref`, when non-NULL, receives the same samples as the future
 * AEC reference (SpeexDSP AEC is v0.5 work; pass NULL until a consumer
 * exists). Delay compensation is a v0.5 AEC-tuning concern. */
void mixer_pull(int16_t speaker[MIXER_PCM_SAMPLES],
                int16_t aec_ref[MIXER_PCM_SAMPLES]);

/* Set the duck attenuation applied to mesh streams when a phone call
 * is in progress. Argument is positive dB of attenuation (0 = no duck,
 * 24 = maximum). Quantized to the nearest entry in the Q15 duck table
 * (0, 3, 6, 9, 12, 18, 24 dB). */
void mixer_set_mesh_duck_db(int8_t duck_db);
