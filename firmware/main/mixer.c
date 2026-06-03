/*
 * mixer — per-rider jitter buffers, decode pool, summing mixer.
 *
 * v0 behaviour:
 *   - Per-rider seq-aware JB (3 slots = 30 ms).
 *   - VAD-gated decode: if the pulled JB slot has VAD inactive, skip
 *     codec_lc3_decode and treat as silence. This is the CPU win that
 *     keeps 8-rider worst-case from blowing through the budget.
 *   - Mesh streams are ducked per s_mesh_duck_db (Q15 multiplier).
 *   - Phone audio (HFP RX / A2DP) is summed at unity gain.
 *   - Speaker output is also the AEC reference (no delay-compensation
 *     yet — that's a v0.5 AEC-tuning concern).
 */

#include "mixer.h"
#include "mixer_jb.h"
#include "codec_lc3.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "mixer";

#define MIXER_RIDERS  LC3_MAX_DECODERS

typedef struct {
    mixer_jb_t jb;
    bool       in_use;
} rider_state_t;

static rider_state_t s_riders[MIXER_RIDERS];
static int16_t       s_phone_pcm[MIXER_PCM_SAMPLES];
static bool          s_phone_has_audio = false;
static int8_t        s_mesh_duck_db    = 0;

static SemaphoreHandle_t s_mtx;

esp_err_t mixer_init(void)
{
    ESP_LOGI(TAG, "init");
    memset(s_riders, 0, sizeof(s_riders));
    for (uint8_t r = 0; r < MIXER_RIDERS; r++) {
        mixer_jb_init(&s_riders[r].jb);
    }
    s_phone_has_audio = false;
    s_mesh_duck_db = 0;
    s_mtx = xSemaphoreCreateMutex();
    return s_mtx ? ESP_OK : ESP_ERR_NO_MEM;
}

void mixer_push_remote_frame(uint8_t rider_id,
                             uint16_t seq,
                             bool vad_active,
                             const uint8_t *lc3,
                             uint16_t len)
{
    if (rider_id >= MIXER_RIDERS) return;
    if (lc3 == NULL || len == 0 || len > LC3_FRAME_BYTES) return;

    /* Called from the audio_rx task (NOT the wifi callback any more —
     * the callback enqueues, this drains). Blocking on the mutex is
     * fine here. */
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    rider_state_t *r = &s_riders[rider_id];
    if (!r->in_use) {
        /* Decoder was pre-allocated in codec_lc3_init; this is just a
         * flag flip. No malloc on the recv path. */
        r->in_use = true;
    }
    mixer_jb_push(&r->jb, seq, vad_active, lc3, (uint8_t)len);
    xSemaphoreGive(s_mtx);
}

void mixer_push_phone_pcm(const int16_t pcm[MIXER_PCM_SAMPLES])
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    memcpy(s_phone_pcm, pcm, sizeof(s_phone_pcm));
    s_phone_has_audio = true;
    xSemaphoreGive(s_mtx);
}

void mixer_pull(int16_t speaker[MIXER_PCM_SAMPLES],
                int16_t aec_ref[MIXER_PCM_SAMPLES])
{
    int32_t acc[MIXER_PCM_SAMPLES] = {0};
    int16_t q15_mult = mixer_jb_duck_q15(s_mesh_duck_db);

    xSemaphoreTake(s_mtx, portMAX_DELAY);

    for (uint8_t i = 0; i < MIXER_RIDERS; i++) {
        rider_state_t *r = &s_riders[i];
        if (!r->in_use) continue;

        uint8_t lc3_buf[LC3_FRAME_BYTES];
        uint8_t lc3_len = 0;
        bool    vad = false;
        bool    have = mixer_jb_pull(&r->jb, lc3_buf, &lc3_len, &vad);

        int16_t pcm[MIXER_PCM_SAMPLES];

        if (!have) {
            /* JB underrun — run PLC. */
            codec_lc3_decode(i, NULL, 0, pcm);
        } else if (!vad) {
            /* VAD-inactive: skip decode, contribute silence. CPU win. */
            continue;
        } else {
            codec_lc3_decode(i, lc3_buf, lc3_len, pcm);
        }

        for (int s = 0; s < MIXER_PCM_SAMPLES; s++) {
            int16_t ducked = mixer_jb_apply_duck(pcm[s], q15_mult);
            acc[s] += ducked;
        }
    }

    if (s_phone_has_audio) {
        for (int s = 0; s < MIXER_PCM_SAMPLES; s++) {
            acc[s] += s_phone_pcm[s];
        }
        s_phone_has_audio = false;
    }

    xSemaphoreGive(s_mtx);

    /* Saturate to int16 and write the speaker buffer. */
    for (int s = 0; s < MIXER_PCM_SAMPLES; s++) {
        int32_t v = acc[s];
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        speaker[s] = (int16_t)v;
    }

    /* AEC reference is (for now) just the speaker output. */
    if (aec_ref) {
        memcpy(aec_ref, speaker, sizeof(int16_t) * MIXER_PCM_SAMPLES);
    }
}

void mixer_pull_speaker_frame(int16_t out[MIXER_PCM_SAMPLES])
{
    int16_t ref[MIXER_PCM_SAMPLES];
    mixer_pull(out, ref);
}

void mixer_set_mesh_duck_db(int8_t duck_db)
{
    if (duck_db < 0)  duck_db = 0;
    if (duck_db > 24) duck_db = 24;
    s_mesh_duck_db = duck_db;
}
