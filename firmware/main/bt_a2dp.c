/*
 * bt_a2dp — see bt_a2dp.h.
 *
 * Threading:
 *   - bt_a2dp_init runs from app_main once.
 *   - a2d_cb / avrc_cb / sink_data_cb fire on the Bluedroid BTC task.
 *     They produce into the ring buffer + emit session_fsm events;
 *     never block.
 *   - bt_a2dp_drain_to_mixer is the consumer, called from loopback_task
 *     on Core 1.
 *
 * The ring buffer lives in PSRAM (xRingbufferCreateStatic with PSRAM
 * storage). Same DRAM-relief pattern as SCO; see bt_audio.c for the
 * rationale.
 */

#include "bt_a2dp.h"
#include "bt_classic.h"   /* for the BT event types + emit hook */
#include "mixer.h"
#include "resampler_48_16.h"

#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

static const char *TAG = "bt_a2dp";

/* 16 kHz mono PCM ring, 1 second deep. Music callers tolerate jitter
 * better than calls do; A2DP frames can arrive in 100+ ms bursts. */
#define A2DP_RING_BYTES         (16000 * 2 * 1)   /* 1 s = 32000 B */
#define PCM_TICK_BYTES          (160 * 2)         /* 10 ms / tick */

static StaticRingbuffer_t s_ring_meta;
static RingbufHandle_t    s_ring = NULL;
static volatile bool      s_streaming = false;
static uint16_t           s_sample_rate = 48000;  /* set on AUDIO_CFG */

/* 48 -> 16 kHz mono decimator state.
 *
 * The sink data callback delivers interleaved S16 stereo at 48 kHz.
 * We mix L+R into mono and feed into resampler_48_16: 31-tap Hamming-
 * windowed sinc with ~57 dB stopband (host-tested). The resampler
 * carries its own input-phase state, so the caller passes any length
 * and gets ~n_in/3 outputs back. */
static resampler_48_16_state_t s_resampler;

static uint32_t s_rx_pcm_bytes = 0;    /* incoming bytes from BTC task */
static uint32_t s_rx_drops     = 0;
static uint32_t s_pull_count   = 0;
static uint32_t s_pull_under   = 0;

static void emit_bt(bt_event_t evt);

static int16_t stereo_to_mono(int16_t l, int16_t r)
{
    int32_t s = (int32_t)l + (int32_t)r;
    return (int16_t)((s + 1) >> 1);
}

/* Sink PCM callback (legacy API). buf is interleaved S16 stereo at
 * s_sample_rate (we forced 48 kHz via the SEP capability).
 *
 * Path: stereo S16 -> mono S16 (stack buf) -> resampler_48_16 ->
 * 16 kHz mono S16 (stack buf) -> PSRAM ring. The mono buffer is
 * sized for a typical A2DP MTU (~1024 stereo pairs at most). */
static void sink_data_cb(const uint8_t *buf, uint32_t len)
{
    if (!s_streaming || !s_ring || !buf || len < 4) return;

    const int16_t *pcm     = (const int16_t *)buf;
    uint32_t       n_pairs = len / 4;

    int16_t mono_buf[512];
    size_t  mono_n = 0;
    for (uint32_t i = 0; i < n_pairs && mono_n < sizeof(mono_buf) /
                                                  sizeof(mono_buf[0]); i++) {
        mono_buf[mono_n++] = stereo_to_mono(pcm[2*i + 0], pcm[2*i + 1]);
    }
    if (mono_n == 0) return;

    /* Worst-case output: every input sample triggers (very high phase
     * starting state) → cap at mono_n/3 + 1. */
    int16_t out[sizeof(mono_buf) / sizeof(mono_buf[0]) / 3 + 1];
    size_t  out_n = resampler_48_16_decimate(&s_resampler,
                                              mono_buf, mono_n, out);
    if (out_n == 0) return;

    size_t out_bytes = out_n * sizeof(int16_t);
    if (xRingbufferSend(s_ring, out, out_bytes, 0) == pdTRUE) {
        s_rx_pcm_bytes += out_bytes;
    } else {
        s_rx_drops++;
    }
}

static void a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
        ESP_LOGI(TAG, "A2DP conn: state=%d", param->conn_stat.state);
        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            emit_bt(BT_EVT_CONNECTED);
        } else if (param->conn_stat.state ==
                       ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            s_streaming = false;
            emit_bt(BT_EVT_A2DP_STOP);
        }
        break;

    case ESP_A2D_AUDIO_STATE_EVT:
        ESP_LOGI(TAG, "A2DP audio state: %d",
                 param->audio_stat.state);
        if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
            /* Flush stale frames from any previous stream. */
            if (s_ring) {
                size_t n; void *p = NULL;
                while ((p = xRingbufferReceiveUpTo(s_ring, &n, 0,
                                                    A2DP_RING_BYTES))) {
                    vRingbufferReturnItem(s_ring, p);
                }
            }
            memset(&s_resampler, 0, sizeof(s_resampler));
            s_rx_pcm_bytes = 0;
            s_rx_drops     = 0;
            s_pull_count   = 0;
            s_pull_under   = 0;
            s_streaming    = true;
            emit_bt(BT_EVT_A2DP_START);
        } else {
            /* SUSPEND or STOP */
            ESP_LOGI(TAG, "A2DP stats: rx=%lu B drops=%lu pull=%lu under=%lu",
                     (unsigned long)s_rx_pcm_bytes,
                     (unsigned long)s_rx_drops,
                     (unsigned long)s_pull_count,
                     (unsigned long)s_pull_under);
            s_streaming = false;
            emit_bt(BT_EVT_A2DP_STOP);
        }
        break;

    case ESP_A2D_AUDIO_CFG_EVT: {
        esp_a2d_mcc_t *m = &param->audio_cfg.mcc;
        if (m->type == ESP_A2D_MCT_SBC) {
            /* Decode sample rate from samp_freq mask (one bit set). */
            uint8_t f = m->cie.sbc_info.samp_freq;
            uint16_t hz = 0;
            if (f & ESP_A2D_SBC_CIE_SF_48K) hz = 48000;
            else if (f & ESP_A2D_SBC_CIE_SF_44K) hz = 44100;
            else if (f & ESP_A2D_SBC_CIE_SF_32K) hz = 32000;
            else if (f & ESP_A2D_SBC_CIE_SF_16K) hz = 16000;
            if (hz > 0) s_sample_rate = hz;
            ESP_LOGI(TAG, "A2DP SBC cfg: %u Hz, ch_mode=0x%x",
                     (unsigned)hz, m->cie.sbc_info.ch_mode);
            if (hz != 48000) {
                ESP_LOGW(TAG, "A2DP rate %u Hz != 48000; decimator "
                              "produces wrong cadence — music will play "
                              "at altered pitch", (unsigned)hz);
            }
        } else {
            ESP_LOGW(TAG, "A2DP codec type %d unsupported (SBC only)",
                     m->type);
        }
        break;
    }

    case ESP_A2D_PROF_STATE_EVT:
        ESP_LOGI(TAG, "A2DP prof state: %d",
                 param->a2d_prof_stat.init_state);
        break;

    default:
        ESP_LOGD(TAG, "A2DP evt %d", event);
        break;
    }
}

static void avrc_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
        ESP_LOGI(TAG, "AVRC conn: %d", param->conn_stat.connected);
        break;
    case ESP_AVRC_CT_METADATA_RSP_EVT:
        ESP_LOGI(TAG, "AVRC metadata attr=0x%x: %.*s",
                 param->meta_rsp.attr_id,
                 param->meta_rsp.attr_length,
                 param->meta_rsp.attr_text);
        break;
    case ESP_AVRC_CT_PLAY_STATUS_RSP_EVT:
        ESP_LOGI(TAG, "AVRC play status: state=%d pos=%lu/%lu ms",
                 param->play_status_rsp.play_status,
                 (unsigned long)param->play_status_rsp.song_position,
                 (unsigned long)param->play_status_rsp.song_length);
        break;
    case ESP_AVRC_CT_REMOTE_FEATURES_EVT:
        ESP_LOGI(TAG, "AVRC remote feats: 0x%lx",
                 (unsigned long)param->rmt_feats.feat_mask);
        break;
    default:
        ESP_LOGD(TAG, "AVRC evt %d", event);
        break;
    }
}

/* Forward declaration weak hook implemented in bt_classic.c to feed
 * BT events into session_fsm. Defining it here avoids a tight coupling
 * back into bt_classic.c's emit() symbol. */
extern void bt_classic_external_emit(bt_event_t evt);

static void emit_bt(bt_event_t evt)
{
    bt_classic_external_emit(evt);
}

esp_err_t bt_a2dp_init(void)
{
    uint8_t *storage = heap_caps_malloc(A2DP_RING_BYTES,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!storage) {
        ESP_LOGE(TAG, "ring PSRAM alloc failed");
        return ESP_ERR_NO_MEM;
    }
    s_ring = xRingbufferCreateStatic(A2DP_RING_BYTES, RINGBUF_TYPE_BYTEBUF,
                                     storage, &s_ring_meta);
    if (!s_ring) {
        ESP_LOGE(TAG, "ring create failed");
        return ESP_ERR_NO_MEM;
    }
    memset(&s_resampler, 0, sizeof(s_resampler));

    ESP_ERROR_CHECK(esp_a2d_register_callback(a2d_cb));
    ESP_ERROR_CHECK(esp_a2d_sink_register_data_callback(sink_data_cb));
    ESP_ERROR_CHECK(esp_a2d_sink_init());

    /* AVRCP-CT before A2DP sink connection per IDF doc note ("if you
     * want to use AVRC together, initiate AVRC first"). In practice
     * both come up before any phone connects, so order here is fine. */
    ESP_ERROR_CHECK(esp_avrc_ct_init());
    ESP_ERROR_CHECK(esp_avrc_ct_register_callback(avrc_cb));

    ESP_LOGI(TAG, "init: ring=%dB (PSRAM), 48k stereo SBC sink", A2DP_RING_BYTES);
    return ESP_OK;
}

bool bt_a2dp_streaming(void) { return s_streaming; }

void bt_a2dp_drain_to_mixer(void)
{
    if (!s_streaming || !s_ring) return;

    int16_t  pcm[160];
    uint8_t *dst  = (uint8_t *)pcm;
    size_t   need = PCM_TICK_BYTES;
    s_pull_count++;

    while (need > 0) {
        size_t got = 0;
        void *p = xRingbufferReceiveUpTo(s_ring, &got, 0, need);
        if (!p || got == 0) {
            memset(dst, 0, need);
            s_pull_under++;
            break;
        }
        memcpy(dst, p, got);
        vRingbufferReturnItem(s_ring, p);
        dst  += got;
        need -= got;
    }
    mixer_push_phone_pcm(pcm);
}
