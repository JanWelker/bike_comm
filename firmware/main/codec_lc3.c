/*
 * codec_lc3 — thin wrapper over google/liblc3.
 *
 * Status: skeleton. liblc3 needs vendoring into firmware/components/liblc3
 * (see that directory's README). Once present, fill in encode/decode bodies.
 */

#include "codec_lc3.h"

#include <string.h>
#include "esp_log.h"

static const char *TAG = "lc3";

/* Decoder pool — index = rider_id (0..7). NULL means unallocated. */
typedef struct {
    bool     in_use;
    /* lc3_decoder_t *dec;  // becomes real once liblc3 lands */
} lc3_dec_slot_t;

static lc3_dec_slot_t decoders[LC3_MAX_DECODERS];
/* static lc3_encoder_t *encoder; */

esp_err_t codec_lc3_init(void)
{
    ESP_LOGI(TAG, "init (24 kbps, 10 ms, 16 kHz)");
    memset(decoders, 0, sizeof(decoders));
    /* TODO: lc3_encoder_create(10000, 16000, 0, malloc(lc3_encoder_size(...))); */
    return ESP_OK;
}

size_t codec_lc3_encode(const int16_t pcm[160], uint8_t out[LC3_FRAME_BYTES])
{
    (void)pcm; (void)out;
    /* TODO: lc3_encode(encoder, LC3_PCM_FORMAT_S16, pcm, 1, LC3_FRAME_BYTES, out); */
    return LC3_FRAME_BYTES;
}

esp_err_t codec_lc3_decoder_acquire(uint8_t rider_id)
{
    if (rider_id >= LC3_MAX_DECODERS) return ESP_ERR_INVALID_ARG;
    if (decoders[rider_id].in_use)    return ESP_OK;
    decoders[rider_id].in_use = true;
    /* TODO: lc3_decoder_create(...); store handle. */
    return ESP_OK;
}

void codec_lc3_decoder_release(uint8_t rider_id)
{
    if (rider_id >= LC3_MAX_DECODERS) return;
    decoders[rider_id].in_use = false;
    /* TODO: free decoder handle. */
}

size_t codec_lc3_decode(uint8_t rider_id,
                        const uint8_t *bytes, size_t len,
                        int16_t out_pcm[160])
{
    (void)rider_id; (void)bytes; (void)len; (void)out_pcm;
    /* TODO: lc3_decode(dec, bytes, len, LC3_PCM_FORMAT_S16, out_pcm, 1);
     *       bytes==NULL triggers PLC. */
    return 160;
}
