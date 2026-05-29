/*
 * audio_pipeline — I2S DMA + ESP-SR AFE (AEC/NS/VAD)
 *
 * Owns:
 *   - I2S0 input from INMP441 MEMS mic (16 kHz mono)
 *   - I2S1 output to ES8388 codec -> MAX98357A amp (16 kHz mono/stereo)
 *   - ESP-SR AFE pipeline: AEC reference = mixer output (loopback)
 *
 * Frame model: 10 ms = 160 samples @ 16 kHz, int16_t.
 *
 * Threading:
 *   - i2s_mic_rx task drains DMA into ring buffer (Core 1, prio 22)
 *   - afe_task runs AEC + NS + VAD per frame  (Core 1, prio 21)
 *   - mic frames are delivered to the encoder via afe_get_frame()
 *   - speaker frames come back via spk_push_frame() from the mixer
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#define AUDIO_SR_HZ          16000
#define AUDIO_FRAME_SAMPLES  160   /* 10 ms @ 16 kHz */

typedef struct {
    int16_t  samples[AUDIO_FRAME_SAMPLES];
    bool     vad_active;
    uint32_t timestamp_us;
} audio_frame_t;

esp_err_t audio_pipeline_init(void);
void      audio_pipeline_start(void);

/* Blocks until a post-AFE mic frame is available. */
esp_err_t audio_pipeline_get_mic_frame(audio_frame_t *out, TickType_t timeout);

/* Push a frame to the speaker output (also used as the AEC reference). */
esp_err_t audio_pipeline_push_speaker_frame(const audio_frame_t *in);
