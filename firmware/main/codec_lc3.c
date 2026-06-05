/*
 * codec_lc3 — thin wrapper over google/liblc3 (vendored at
 * firmware/components/liblc3/upstream).
 *
 * Operating point: 16 kHz mono, 10 ms frames, 32 kbps → 40 B on air.
 * One shared encoder (TX); one decoder per active remote rider (RX),
 * allocated on demand into a fixed-size pool.
 *
 * Memory: codec state is placed in internal RAM via heap_caps_malloc so
 * the hot encode/decode loops never touch PSRAM (which would crater
 * throughput on the original ESP32).
 */

#include "codec_lc3.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lc3.h"

static const char *TAG = "lc3";

/* Wall-clock perf counters around each lc3_encode / lc3_decode call.
 * Reset on every codec_lc3_perf_log_and_reset() call. esp_audio_codec
 * only publishes S3R8 (LX7+PIE) numbers — these counters give us
 * actual LX6 figures from the LyraT-Mini bench. See docs/codec_perf.md. */
static int64_t  s_enc_us_sum = 0;
static int64_t  s_enc_us_max = 0;
static uint32_t s_enc_count  = 0;
static int64_t  s_dec_us_sum = 0;
static int64_t  s_dec_us_max = 0;
static uint32_t s_dec_count  = 0;

typedef struct {
    bool           in_use;
    lc3_decoder_t  dec;     /* handle == base of mem buffer (per liblc3 docs) */
    void          *mem;     /* keep a copy for free() */
} lc3_dec_slot_t;

static lc3_dec_slot_t s_decoders[LC3_MAX_DECODERS];
static lc3_encoder_t  s_encoder;
static void          *s_encoder_mem;

static void *codec_alloc(size_t n)
{
    return heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

esp_err_t codec_lc3_init(void)
{
    memset(s_decoders, 0, sizeof(s_decoders));

    if (s_encoder != NULL) {
        /* idempotent — already initialised */
        return ESP_OK;
    }

    unsigned sz = lc3_encoder_size(LC3_FRAME_DUR_US, LC3_SAMPLE_RATE_HZ);
    if (sz == 0) {
        ESP_LOGE(TAG, "lc3_encoder_size returned 0 (bad params)");
        return ESP_ERR_INVALID_ARG;
    }

    s_encoder_mem = codec_alloc(sz);
    if (s_encoder_mem == NULL) {
        ESP_LOGE(TAG, "encoder alloc failed (%u B)", sz);
        return ESP_ERR_NO_MEM;
    }

    s_encoder = lc3_setup_encoder(LC3_FRAME_DUR_US,
                                  LC3_SAMPLE_RATE_HZ,
                                  0, /* sr_pcm_hz=0 → same as sr_hz */
                                  s_encoder_mem);
    if (s_encoder == NULL) {
        ESP_LOGE(TAG, "lc3_setup_encoder failed");
        heap_caps_free(s_encoder_mem);
        s_encoder_mem = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "init ok (32 kbps, 10 ms, 16 kHz, enc state %u B)", sz);

    /* Pre-allocate all decoder slots up front. The original design
     * was lazy (acquire on first frame from a rider) but that puts a
     * heap_caps_malloc inside the ESP-NOW receive callback, which
     * stalls the wifi task long enough to drop beacons and trip the
     * coordinator-lost timer on the other end. Doing it here trades
     * ~8 * lc3_decoder_size() = ~8 KB of internal RAM for a recv-path
     * that's free of allocations and any per-rider first-time cost. */
    for (uint8_t r = 0; r < LC3_MAX_DECODERS; r++) {
        esp_err_t err = codec_lc3_decoder_acquire(r);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "pre-acquire decoder[%u] failed: %s", r,
                     esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}

size_t codec_lc3_encode(const int16_t pcm[LC3_PCM_SAMPLES],
                        uint8_t out[LC3_FRAME_BYTES])
{
    if (s_encoder == NULL || pcm == NULL || out == NULL) {
        return 0;
    }
    int64_t t0 = esp_timer_get_time();
    int rc = lc3_encode(s_encoder, LC3_PCM_FORMAT_S16,
                        pcm, 1 /* stride */, LC3_FRAME_BYTES, out);
    int64_t dt = esp_timer_get_time() - t0;
    s_enc_us_sum += dt;
    if (dt > s_enc_us_max) s_enc_us_max = dt;
    s_enc_count++;
    if (rc != 0) {
        ESP_LOGW(TAG, "lc3_encode rc=%d", rc);
        return 0;
    }
    return LC3_FRAME_BYTES;
}

esp_err_t codec_lc3_decoder_acquire(uint8_t rider_id)
{
    if (rider_id >= LC3_MAX_DECODERS) return ESP_ERR_INVALID_ARG;
    if (s_decoders[rider_id].in_use)  return ESP_OK;  /* idempotent */

    unsigned sz = lc3_decoder_size(LC3_FRAME_DUR_US, LC3_SAMPLE_RATE_HZ);
    if (sz == 0) return ESP_ERR_INVALID_ARG;

    void *mem = codec_alloc(sz);
    if (mem == NULL) {
        ESP_LOGE(TAG, "decoder[%u] alloc failed (%u B)", rider_id, sz);
        return ESP_ERR_NO_MEM;
    }

    lc3_decoder_t dec = lc3_setup_decoder(LC3_FRAME_DUR_US,
                                          LC3_SAMPLE_RATE_HZ,
                                          0, /* sr_pcm_hz=0 → same as sr_hz */
                                          mem);
    if (dec == NULL) {
        heap_caps_free(mem);
        return ESP_FAIL;
    }

    s_decoders[rider_id].in_use = true;
    s_decoders[rider_id].dec    = dec;
    s_decoders[rider_id].mem    = mem;
    ESP_LOGI(TAG, "decoder[%u] acquired (%u B)", rider_id, sz);
    return ESP_OK;
}

void codec_lc3_decoder_release(uint8_t rider_id)
{
    if (rider_id >= LC3_MAX_DECODERS) return;
    if (!s_decoders[rider_id].in_use) return;

    heap_caps_free(s_decoders[rider_id].mem);
    s_decoders[rider_id].mem    = NULL;
    s_decoders[rider_id].dec    = NULL;
    s_decoders[rider_id].in_use = false;
    ESP_LOGI(TAG, "decoder[%u] released", rider_id);
}

size_t codec_lc3_decode(uint8_t rider_id,
                        const uint8_t *bytes, size_t len,
                        int16_t out_pcm[LC3_PCM_SAMPLES])
{
    if (rider_id >= LC3_MAX_DECODERS) return 0;
    if (!s_decoders[rider_id].in_use) return 0;
    if (out_pcm == NULL)              return 0;

    /* Per liblc3: in=NULL triggers PLC; lc3_decode returns
     * 0 on success, 1 if PLC was operated, -1 on bad params. */
    int64_t t0 = esp_timer_get_time();
    int rc = lc3_decode(s_decoders[rider_id].dec,
                        bytes, (int)len,
                        LC3_PCM_FORMAT_S16, out_pcm, 1 /* stride */);
    int64_t dt = esp_timer_get_time() - t0;
    s_dec_us_sum += dt;
    if (dt > s_dec_us_max) s_dec_us_max = dt;
    s_dec_count++;
    if (rc < 0) {
        ESP_LOGW(TAG, "lc3_decode rider=%u rc=%d", rider_id, rc);
        return 0;
    }
    return LC3_PCM_SAMPLES;
}

void codec_lc3_perf_log_and_reset(void)
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
