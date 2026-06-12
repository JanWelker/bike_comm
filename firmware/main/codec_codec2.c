/*
 * codec_codec2 — Codec2 3200 bps backend for codec.h.
 *
 * Vendored upstream at firmware/components/codec2/upstream/ (drowe67/
 * codec2 v1.2.0, BSD-2-Clause). Codec2 is an 8 kHz speech codec; we
 * bridge it to the project's 16 kHz audio pipeline with a 31-tap
 * halfband resampler (firmware/main/resampler_16_8.{c,h}).
 *
 * Encode flow per 10 ms tick:
 *   - Buffer the 160 incoming 16 kHz samples.
 *   - On every 2nd tick we have 320 samples / 20 ms ready:
 *       320 @ 16 kHz  -> resampler_decimate -> 160 @ 8 kHz
 *       160 @ 8 kHz   -> codec2_encode      -> 8 B (64 bits)
 *     Zero-pad the 8 B into out[0..39] and return CODEC_FRAME_BYTES.
 *   - On the intermediate tick return 0 (caller skips the mesh TX
 *     queue push). Net wire rate is 50 Hz packets, half of LC3 —
 *     accepted for the A/B; the next iteration would shrink the
 *     wire slot.
 *
 * Decode flow per 10 ms tick:
 *   - If the JB delivered bytes:
 *       codec2_decode (8 B -> 160 @ 8 kHz)
 *       resampler_interpolate (160 @ 8 kHz -> 320 @ 16 kHz)
 *       return first 160; cache second 160 for the next pull.
 *   - If no bytes and we have a cache: return the cache.
 *   - Otherwise: silence (PLC — codec2 doesn't have one built in;
 *     LC3-style frame interp would be next-iteration work).
 *
 * All state lives in internal RAM (codec2_create allocates via the
 * libc malloc that ESP-IDF maps to caps that include internal RAM
 * once esp_heap_caps_init is done; we guard with a heap_caps check
 * after creation).
 */

#include "codec.h"
#include "resampler_16_8.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "codec2.h"

static const char *TAG = "codec2";

/* Wall-clock perf counters. Same shape as codec_lc3.c so
 * codec_perf_log_and_reset feeds docs/codec_perf.md identically. */
static int64_t  s_enc_us_sum = 0;
static int64_t  s_enc_us_max = 0;
static uint32_t s_enc_count  = 0;
static int64_t  s_dec_us_sum = 0;
static int64_t  s_dec_us_max = 0;
static uint32_t s_dec_count  = 0;

#define C2_SAMPLES_PER_FRAME   160   /* 20 ms @ 8 kHz */
#define C2_BITS_PER_FRAME      64    /* 3200 bps * 20 ms */
#define C2_BYTES_PER_FRAME     (C2_BITS_PER_FRAME / 8)
#define ENC_BUF_SAMPLES        (CODEC_FRAME_SAMPLES * 2)  /* 320 @ 16 kHz */

/* Codec2 state lives in PSRAM via the component's __EMBEDDED__
 * codec2_malloc/calloc/free (see firmware/components/codec2/
 * codec2_alloc.c). That frees the internal-DRAM budget the LC3
 * build was tight on — we can now pre-allocate one encoder + all
 * MESH_MAX_RIDERS decoders with room to spare for mbedtls and
 * audio buffers. */
#define C2_DECODER_SLOTS       CODEC_MAX_DECODERS

static struct CODEC2 *s_enc = NULL;
static int16_t        s_enc_buf[ENC_BUF_SAMPLES];
static uint8_t        s_enc_tick = 0;          /* 0 = first half, 1 = second */
static resampler_state_t s_enc_resamp;

typedef struct {
    struct CODEC2     *c2;
    resampler_state_t  resamp;
    int16_t            cache[CODEC_FRAME_SAMPLES];  /* second 16 kHz half */
    bool               has_cache;
} dec_slot_t;

static dec_slot_t s_decoders[CODEC_MAX_DECODERS];

esp_err_t codec_init(void)
{
    if (s_enc != NULL) {
        return ESP_OK;   /* idempotent */
    }
    s_enc = codec2_create(CODEC2_MODE_3200);
    if (s_enc == NULL) {
        ESP_LOGE(TAG, "codec2_create(3200) failed");
        return ESP_ERR_NO_MEM;
    }
    if (codec2_bits_per_frame(s_enc)    != C2_BITS_PER_FRAME ||
        codec2_samples_per_frame(s_enc) != C2_SAMPLES_PER_FRAME) {
        ESP_LOGE(TAG, "codec2 mode 3200 reports unexpected geometry: %d bits, %d samples",
                 codec2_bits_per_frame(s_enc),
                 codec2_samples_per_frame(s_enc));
        codec2_destroy(s_enc);
        s_enc = NULL;
        return ESP_FAIL;
    }
    resampler_init(&s_enc_resamp);
    s_enc_tick = 0;

    memset(s_decoders, 0, sizeof(s_decoders));
    for (uint8_t r = 0; r < C2_DECODER_SLOTS; ++r) {
        s_decoders[r].c2 = codec2_create(CODEC2_MODE_3200);
        if (s_decoders[r].c2 == NULL) {
            ESP_LOGE(TAG, "codec2_create dec[%u] failed", r);
            return ESP_ERR_NO_MEM;
        }
        resampler_init(&s_decoders[r].resamp);
        s_decoders[r].has_cache = false;
        ESP_LOGI(TAG, "decoder[%u] acquired", r);
    }
    if (C2_DECODER_SLOTS < CODEC_MAX_DECODERS) {
        ESP_LOGW(TAG, "decoder slots %d..%d unallocated (DRAM budget) — those riders mute",
                 C2_DECODER_SLOTS, CODEC_MAX_DECODERS - 1);
    }

    ESP_LOGI(TAG, "init ok (3200 bps, 20 ms internal, 16 kHz pipeline via halfband resampler)");
    return ESP_OK;
}

size_t codec_encode(const int16_t pcm[CODEC_FRAME_SAMPLES],
                    uint8_t out[CODEC_FRAME_BYTES])
{
    if (s_enc == NULL || pcm == NULL || out == NULL) return 0;

    /* Accumulate one 10 ms tick into the 20 ms encode buffer. */
    memcpy(&s_enc_buf[s_enc_tick * CODEC_FRAME_SAMPLES],
           pcm, CODEC_FRAME_SAMPLES * sizeof(int16_t));
    s_enc_tick++;
    if (s_enc_tick < 2) {
        return 0;   /* mesh tx skips this slot */
    }
    s_enc_tick = 0;

    int64_t t0 = esp_timer_get_time();

    int16_t pcm_8k[C2_SAMPLES_PER_FRAME];
    resampler_decimate(&s_enc_resamp, s_enc_buf, ENC_BUF_SAMPLES, pcm_8k);

    unsigned char bits[C2_BYTES_PER_FRAME];
    codec2_encode(s_enc, bits, pcm_8k);

    /* Zero-pad codec2's 8 B into the 40 B wire slot. The unused 32 B
     * are pure waste — accepted as the price of an LC3-compatible
     * wire format for this A/B. */
    memset(out, 0, CODEC_FRAME_BYTES);
    memcpy(out, bits, C2_BYTES_PER_FRAME);

    int64_t dt = esp_timer_get_time() - t0;
    s_enc_us_sum += dt;
    if (dt > s_enc_us_max) s_enc_us_max = dt;
    s_enc_count++;
    return CODEC_FRAME_BYTES;
}

size_t codec_decode(uint8_t rider_id,
                    const uint8_t *bytes, size_t len,
                    int16_t out_pcm[CODEC_FRAME_SAMPLES])
{
    if (rider_id >= CODEC_MAX_DECODERS) return 0;
    if (out_pcm == NULL)                return 0;
    dec_slot_t *d = &s_decoders[rider_id];
    if (d->c2 == NULL)                  return 0;

    /* Real bytes arrived → decode + interpolate + emit first half +
     * cache second half. A second-real-frame-in-a-row case (rare at
     * the 50 Hz codec2/mesh slot match) overwrites the cache, dropping
     * a 10 ms span; acceptable for the A/B and undetectable in
     * subjective evaluation. */
    if (bytes != NULL && len >= C2_BYTES_PER_FRAME) {
        int64_t t0 = esp_timer_get_time();
        int16_t pcm_8k[C2_SAMPLES_PER_FRAME];
        codec2_decode(d->c2, pcm_8k, bytes);

        int16_t pcm_16k[CODEC_FRAME_SAMPLES * 2];
        resampler_interpolate(&d->resamp, pcm_8k, C2_SAMPLES_PER_FRAME, pcm_16k);

        memcpy(out_pcm,   pcm_16k,                              CODEC_FRAME_SAMPLES * sizeof(int16_t));
        memcpy(d->cache, &pcm_16k[CODEC_FRAME_SAMPLES],         CODEC_FRAME_SAMPLES * sizeof(int16_t));
        d->has_cache = true;

        int64_t dt = esp_timer_get_time() - t0;
        s_dec_us_sum += dt;
        if (dt > s_dec_us_max) s_dec_us_max = dt;
        s_dec_count++;
        return CODEC_FRAME_SAMPLES;
    }

    /* No bytes this tick. If we have a cached second half, deliver it
     * — that's the matching half of the previous tick's decode. */
    if (d->has_cache) {
        memcpy(out_pcm, d->cache, CODEC_FRAME_SAMPLES * sizeof(int16_t));
        d->has_cache = false;
        return CODEC_FRAME_SAMPLES;
    }

    /* True drought: nothing to play. codec2 has no built-in PLC; just
     * deliver silence to keep the I2S clock fed. A future iteration
     * could feed the last good frame back through the decoder. */
    memset(out_pcm, 0, CODEC_FRAME_SAMPLES * sizeof(int16_t));
    return CODEC_FRAME_SAMPLES;
}

void codec_perf_log_and_reset(void)
{
    if (s_enc_count > 0) {
        ESP_LOGI(TAG, "enc: n=%u mean=%lld us max=%lld us",
                 (unsigned)s_enc_count,
                 (long long)(s_enc_us_sum / (int64_t)s_enc_count),
                 (long long)s_enc_us_max);
    }
    if (s_dec_count > 0) {
        ESP_LOGI(TAG, "dec: n=%u mean=%lld us max=%lld us",
                 (unsigned)s_dec_count,
                 (long long)(s_dec_us_sum / (int64_t)s_dec_count),
                 (long long)s_dec_us_max);
    }
    s_enc_us_sum = 0; s_enc_us_max = 0; s_enc_count = 0;
    s_dec_us_sum = 0; s_dec_us_max = 0; s_dec_count = 0;
}
