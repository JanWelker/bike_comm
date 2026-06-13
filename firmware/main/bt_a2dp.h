/*
 * bt_a2dp — A2DP-sink + AVRCP-CT (Stage 5).
 *
 * Phone streams music; we render it through the mixer's phone slot
 * (same path SCO uses; mutually exclusive via session_fsm).
 *
 * Path: phone -> SBC decode (Bluedroid internal) -> our PCM cb
 *       -> stereo->mono -> 48->16 kHz decimate -> ring buffer (PSRAM)
 *       -> drain_to_mixer (audio_io tick) -> mixer_push_phone_pcm.
 *
 * We register our sink SEP with **48 kHz only**. The legacy data cb is
 * used because the new API hands us encoded SBC frames, not PCM, and
 * vendoring an SBC decoder is out of scope for v0.5. Most modern
 * phones (Android 8+, iOS 12+) negotiate 48 kHz SBC happily; the few
 * that can't will simply not stream music — HFP-HF calls still work.
 *
 * Resampler is intentionally crude (3-tap box filter then 3:1
 * decimate). Aliasing artifacts exist but are inaudible against music
 * dynamic range in a helmet. A proper Hamming-windowed sinc upgrade
 * is queued behind the SpeexDSP vendoring step.
 *
 * AVRCP-CT: registered + bound to a logging callback. Sending
 * passthrough commands (play/pause/next) needs ui.c to surface
 * button events — pending the broader button-event wire-up.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

esp_err_t bt_a2dp_init(void);

bool bt_a2dp_streaming(void);

/* Called from audio_pipeline's loopback_task each 10 ms tick. No-op
 * when A2DP is not streaming. Pulls one 160-sample chunk of 16 kHz
 * mono PCM from the internal ring (pads with silence on underrun) and
 * pushes to mixer_push_phone_pcm. */
void bt_a2dp_drain_to_mixer(void);
