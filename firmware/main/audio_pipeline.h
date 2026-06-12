/*
 * audio_pipeline — owns the LyraT-Mini v1.2 I2S + codec wiring and the
 * single audio_io task that drives the mic -> mesh and mesh -> speaker
 * path end-to-end.
 *
 * Wiring (full pin map in the comment block at the top of the .c):
 *   - ES8311 on I2S0 -> playback / headphone jack
 *   - ES7243 on I2S1 -> mic capture (onboard MEMS on right channel)
 *
 * Frame model: 10 ms = 160 samples @ 16 kHz, int16 mono.
 *
 * Threading:
 *   - audio_pipeline_init()  allocates I2C + I2S channels, opens codecs.
 *     Channels left disabled until start().
 *   - audio_pipeline_start() enables I2S, drives PA-enable high, spawns
 *     loopback_task (Core 1, prio 22). The task is the whole hot path:
 *     mic read -> LC3 encode -> mesh_mac_queue_tx ->
 *     mesh_rx_drain_to_mixer -> mixer_pull -> speaker write. No
 *     intermediate queues, no separate AFE task — see CLAUDE.md for
 *     why ESP-SR AFE doesn't fit on the LX6.
 */

#pragma once

#include "esp_err.h"

#define AUDIO_SR_HZ          16000
#define AUDIO_FRAME_SAMPLES  160   /* 10 ms @ 16 kHz */

esp_err_t audio_pipeline_init(void);
void      audio_pipeline_start(void);

/* Speaker output volume, in percent (0..100). Initial value is 100,
 * set in audio_pipeline_start(). vol_step adds delta_pct, clamps into
 * range, and pushes the new value to the ES8311 via esp_codec_dev.
 * Safe to call from any task — the codec I/O is I2C, not in the audio
 * hot loop. No-op before audio_pipeline_start() runs. */
void audio_pipeline_vol_step(int delta_pct);
int  audio_pipeline_get_vol_pct(void);
