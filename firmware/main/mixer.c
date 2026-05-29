/*
 * mixer — per-rider jitter buffers, decode pool, summing mixer.
 *
 * Status: skeleton. Real decoding happens once codec_lc3 is live.
 */

#include "mixer.h"
#include "codec_lc3.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "mixer";

#define MIXER_RIDERS  8

typedef struct {
    uint8_t  lc3[MIXER_JITTER_FRAMES][30];
    uint8_t  len[MIXER_JITTER_FRAMES];
    uint8_t  head, tail, count;
    bool     in_use;
} rider_jb_t;

static rider_jb_t s_jb[MIXER_RIDERS];
static int16_t    s_phone_pcm[160];
static bool       s_phone_has_audio = false;
static int8_t     s_mesh_duck_db    = 0;

static SemaphoreHandle_t s_mtx;

esp_err_t mixer_init(void)
{
    ESP_LOGI(TAG, "init");
    memset(s_jb, 0, sizeof(s_jb));
    s_mtx = xSemaphoreCreateMutex();
    return s_mtx ? ESP_OK : ESP_ERR_NO_MEM;
}

void mixer_push_remote_frame(uint8_t rider_id, const uint8_t *lc3, uint16_t len)
{
    if (rider_id >= MIXER_RIDERS || len > 30) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    rider_jb_t *jb = &s_jb[rider_id];
    if (!jb->in_use) {
        codec_lc3_decoder_acquire(rider_id);
        jb->in_use = true;
    }
    if (jb->count < MIXER_JITTER_FRAMES) {
        memcpy(jb->lc3[jb->tail], lc3, len);
        jb->len[jb->tail] = (uint8_t)len;
        jb->tail = (jb->tail + 1) % MIXER_JITTER_FRAMES;
        jb->count++;
    } else {
        /* Buffer full — overflow means we're late. Drop oldest. */
        jb->head = (jb->head + 1) % MIXER_JITTER_FRAMES;
        memcpy(jb->lc3[jb->tail], lc3, len);
        jb->len[jb->tail] = (uint8_t)len;
        jb->tail = (jb->tail + 1) % MIXER_JITTER_FRAMES;
    }
    xSemaphoreGive(s_mtx);
}

void mixer_push_phone_pcm(const int16_t pcm[160])
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    memcpy(s_phone_pcm, pcm, sizeof(s_phone_pcm));
    s_phone_has_audio = true;
    xSemaphoreGive(s_mtx);
}

void mixer_pull_speaker_frame(int16_t out[160])
{
    int32_t acc[160] = {0};
    int8_t  duck    = s_mesh_duck_db;

    xSemaphoreTake(s_mtx, portMAX_DELAY);

    for (uint8_t r = 0; r < MIXER_RIDERS; r++) {
        rider_jb_t *jb = &s_jb[r];
        if (!jb->in_use) continue;

        int16_t pcm[160];
        if (jb->count > 0) {
            codec_lc3_decode(r, jb->lc3[jb->head], jb->len[jb->head], pcm);
            jb->head = (jb->head + 1) % MIXER_JITTER_FRAMES;
            jb->count--;
        } else {
            /* PLC frame */
            codec_lc3_decode(r, NULL, 0, pcm);
        }

        /* Apply duck (rough integer linear approx: -6 dB ~= /2). */
        int shift = (duck <= 0) ? 0 : (duck >= 12 ? 2 : (duck / 6));
        for (int i = 0; i < 160; i++) acc[i] += pcm[i] >> shift;
    }

    if (s_phone_has_audio) {
        for (int i = 0; i < 160; i++) acc[i] += s_phone_pcm[i];
        s_phone_has_audio = false;
    }

    xSemaphoreGive(s_mtx);

    /* Saturate to int16. */
    for (int i = 0; i < 160; i++) {
        if (acc[i] >  32767) acc[i] =  32767;
        if (acc[i] < -32768) acc[i] = -32768;
        out[i] = (int16_t)acc[i];
    }
}

void mixer_set_mesh_duck_db(int8_t duck_db)
{
    if (duck_db < 0)   duck_db = 0;
    if (duck_db > 24)  duck_db = 24;
    s_mesh_duck_db = duck_db;
}
