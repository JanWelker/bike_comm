/*
 * codec_codec2 — Codec2 3200 bps backend stub.
 *
 * Selected by CONFIG_BIKE_CODEC_CODEC2. Upstream drowe67/codec2 source
 * is NOT yet vendored — this file is a build-wire-up stub so the
 * Kconfig CHOICE and the codec.h abstraction can be exercised end-to-
 * end before the source-vendoring step (which lands hundreds of files
 * and warrants its own diff).
 *
 * Real implementation outline (when this file grows up):
 *   - codec_init:    one CODEC2 *encoder + 8 CODEC2 *decoders, all
 *                    pre-allocated in internal RAM (decoder alloc in
 *                    the recv path stalls the wifi task — see CLAUDE.md).
 *   - codec_encode:  buffer two CODEC_FRAME_SAMPLES (= 320 samples =
 *                    20 ms) ticks, then codec2_encode → 8 B; zero-pad
 *                    into out[0..39] every other tick, return 0 on
 *                    the intermediate tick (caller treats as "no audio
 *                    this tick").
 *   - codec_decode:  read 8 B from bytes[0..7], codec2_decode → 320
 *                    samples; buffer for the next 10 ms pull (caller
 *                    is asking for 160 samples at a time).
 *   - codec_perf_log_and_reset: same shape as the LC3 version, so
 *                    docs/codec_perf.md can grow a codec2 column.
 *
 * Wire layout stays the codec.h 40 B / 10 ms / 16 kHz contract so the
 * mesh protocol (mesh_proto.h, mesh_mac.c) needs no changes — the
 * codec2 backend wastes ~64 B / packet of slot space in exchange for a
 * zero-protocol-churn A/B. A future iteration would shrink the wire
 * slot when this codec graduates from "alternate" to "primary."
 */

#include "codec.h"

#include "esp_log.h"

static const char *TAG = "codec2";

esp_err_t codec_init(void)
{
    ESP_LOGE(TAG, "codec2 backend selected but upstream not yet vendored");
    ESP_LOGE(TAG, "see firmware/components/codec2/upstream/ (missing) and"
                  " firmware/main/codec_codec2.c implementation outline");
    abort();
    return ESP_FAIL;  /* unreachable */
}

size_t codec_encode(const int16_t pcm[CODEC_FRAME_SAMPLES],
                    uint8_t out[CODEC_FRAME_BYTES])
{
    (void)pcm; (void)out;
    return 0;
}

size_t codec_decode(uint8_t rider_id,
                    const uint8_t *bytes, size_t len,
                    int16_t out_pcm[CODEC_FRAME_SAMPLES])
{
    (void)rider_id; (void)bytes; (void)len; (void)out_pcm;
    return 0;
}

void codec_perf_log_and_reset(void)
{
}
