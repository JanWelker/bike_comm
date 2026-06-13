/*
 * bt_audio — SCO data plane. See bt_audio.h for the design.
 *
 * Thread/task layout:
 *   - bt_audio_init: app_main task, once.
 *   - sco_rx_cb:     Bluedroid BTC task. Producer of s_rx_ring.
 *   - bt_audio_on_sco_*:  Bluedroid BTC task (called from bt_classic
 *                         HF callback). Only mutates s_sco_active,
 *                         s_sco_handle, s_tx_frame_bytes, stats.
 *   - bt_audio_sco_pull_to_mixer / push_mic:  audio_pipeline
 *                         loopback_task (Core 1). Consumer of s_rx_ring,
 *                         both producer and consumer of s_tx_ring.
 *
 * The FreeRTOS ring buffers are SPSC-thread-safe internally; the only
 * cross-task scalar is s_sco_active which is a one-word volatile flag
 * read by the audio task and written by the BT task. Word-store on
 * the LX6 is atomic.
 *
 * The TX-pending byte counter s_tx_pending is touched only from the
 * audio task, so no atomicity concern.
 */

#include "bt_audio.h"
#include "mixer.h"

#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_hf_client_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

static const char *TAG = "bt_audio";

/* Worst-case SCO frame: mSBC 16 kHz PCM = 120 samples * 2 B = 240 B.
 * CVSD is half. Sizing the on-stack staging buffer at 240 covers both. */
#define SCO_FRAME_BYTES_MAX     240

/* 8-frame rings ~ 60 ms each way. Enough to absorb a tick of audio_io
 * being preempted (mesh_tx_task spike, NS Analyze cliff) without
 * audible glitch; not so big that latency goes audible on the bench. */
#define RX_RING_BYTES           (8 * SCO_FRAME_BYTES_MAX)
#define TX_RING_BYTES           (8 * SCO_FRAME_BYTES_MAX)

/* 10 ms @ 16 kHz mono S16 = 320 B per audio_pipeline tick. */
#define PCM_FRAME_BYTES         (160 * 2)

/* Ring storage lives in PSRAM (heap_caps_malloc MALLOC_CAP_SPIRAM); the
 * metadata struct is small enough to keep in BSS. Going PSRAM frees
 * ~3.8 KB of internal DRAM that audio_io's 7 KB stack would otherwise
 * fight mesh_tx_task for. The 240 B transfers at 7.5 ms cadence cost
 * negligible PSRAM bandwidth and the consumer/producer tasks tolerate
 * the small latency hit. ISR routes are NOT used here (BTC task + Core 1
 * audio task only), which is the safety constraint for PSRAM ringbufs. */
static StaticRingbuffer_t s_rx_ringbuf_meta;
static StaticRingbuffer_t s_tx_ringbuf_meta;
static RingbufHandle_t    s_rx_ring = NULL;
static RingbufHandle_t    s_tx_ring = NULL;

static esp_hf_sync_conn_hdl_t s_sco_handle     = 0;
static uint16_t               s_tx_frame_bytes = SCO_FRAME_BYTES_MAX;
static volatile bool          s_sco_active     = false;

static size_t   s_tx_pending = 0;   /* bytes currently waiting in s_tx_ring */

static uint32_t s_rx_frames    = 0;
static uint32_t s_rx_underruns = 0;
static uint32_t s_tx_frames    = 0;
static uint32_t s_tx_drops     = 0;

/* Drain any stale bytes from a ring without consuming them in-order;
 * fine for the on-up reset where we just want a clean slate. */
static void flush_ring(RingbufHandle_t r)
{
    if (!r) return;
    while (1) {
        size_t got = 0;
        void *p = xRingbufferReceiveUpTo(r, &got, 0, RX_RING_BYTES);
        if (!p) break;
        vRingbufferReturnItem(r, p);
    }
}

static void sco_rx_cb(esp_hf_sync_conn_hdl_t hdl,
                      esp_hf_audio_buff_t *buf,
                      bool bad)
{
    (void)hdl;
    if (!buf) return;
    if (!bad && s_sco_active && s_rx_ring &&
        buf->data && buf->data_len > 0) {
        /* Non-blocking: if the ring is full, drop. audio_pipeline will
         * pad with silence; better one glitch than back-pressuring the
         * BT task and stalling SCO HCI. */
        if (xRingbufferSend(s_rx_ring, buf->data,
                            buf->data_len, 0) == pdTRUE) {
            /* Log the first SCO RX frame per session so the bench can
             * see Bluedroid is actually delivering audio data. The
             * Stage 3 first-bench symptom was rx=0 — silence here for
             * the whole call window is the key signal. */
            if (s_rx_frames == 0) {
                ESP_LOGI(TAG, "first SCO RX frame (%u B)",
                         (unsigned)buf->data_len);
            }
            s_rx_frames++;
        }
    }
    esp_hf_client_audio_buff_free(buf);
}

esp_err_t bt_audio_init(void)
{
    uint8_t *rx_storage = heap_caps_malloc(RX_RING_BYTES,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *tx_storage = heap_caps_malloc(TX_RING_BYTES,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rx_storage || !tx_storage) {
        ESP_LOGE(TAG, "ringbuf PSRAM storage alloc failed");
        return ESP_ERR_NO_MEM;
    }
    s_rx_ring = xRingbufferCreateStatic(RX_RING_BYTES, RINGBUF_TYPE_BYTEBUF,
                                        rx_storage, &s_rx_ringbuf_meta);
    s_tx_ring = xRingbufferCreateStatic(TX_RING_BYTES, RINGBUF_TYPE_BYTEBUF,
                                        tx_storage, &s_tx_ringbuf_meta);
    if (!s_rx_ring || !s_tx_ring) {
        ESP_LOGE(TAG, "ringbuf create failed");
        return ESP_ERR_NO_MEM;
    }
    /* Register the data callback once at init. It self-gates on
     * s_sco_active so registering before any SCO has come up is fine —
     * the controller won't deliver SCO frames until then anyway. */
    ESP_ERROR_CHECK(esp_hf_client_register_audio_data_callback(sco_rx_cb));
    ESP_LOGI(TAG, "init: rx_ring=%dB tx_ring=%dB (PSRAM)",
             RX_RING_BYTES, TX_RING_BYTES);
    return ESP_OK;
}

void bt_audio_on_sco_up(esp_hf_sync_conn_hdl_t handle,
                        uint16_t preferred_frame_bytes)
{
    s_sco_handle     = handle;
    s_tx_frame_bytes = preferred_frame_bytes ? preferred_frame_bytes
                                             : SCO_FRAME_BYTES_MAX;
    if (s_tx_frame_bytes > SCO_FRAME_BYTES_MAX) {
        ESP_LOGW(TAG, "AG asked for %u B/frame, capping to %u",
                 (unsigned)s_tx_frame_bytes, (unsigned)SCO_FRAME_BYTES_MAX);
        s_tx_frame_bytes = SCO_FRAME_BYTES_MAX;
    }
    /* A previous call's bytes would shift the cadence and produce a
     * burst of garbage on the speaker. Flush both rings before
     * enabling the data path. */
    flush_ring(s_rx_ring);
    flush_ring(s_tx_ring);
    s_tx_pending   = 0;
    s_rx_frames    = 0;
    s_rx_underruns = 0;
    s_tx_frames    = 0;
    s_tx_drops     = 0;
    s_sco_active   = true;
    ESP_LOGI(TAG, "SCO up: handle=%u, AG prefers %u B/frame",
             (unsigned)handle, (unsigned)s_tx_frame_bytes);
}

void bt_audio_on_sco_down(void)
{
    s_sco_active = false;
    s_sco_handle = 0;
    ESP_LOGI(TAG, "SCO down: rx=%lu under=%lu tx=%lu drop=%lu",
             (unsigned long)s_rx_frames,
             (unsigned long)s_rx_underruns,
             (unsigned long)s_tx_frames,
             (unsigned long)s_tx_drops);
}

bool bt_audio_sco_active(void) { return s_sco_active; }

void bt_audio_sco_pull_to_mixer(void)
{
    if (!s_sco_active || !s_rx_ring) return;

    int16_t  pcm[160];
    uint8_t *dst  = (uint8_t *)pcm;
    size_t   need = PCM_FRAME_BYTES;

    /* xRingbufferReceiveUpTo may split across the ring's wrap, so loop
     * until we've collected the full 10 ms tick or the ring is dry. */
    while (need > 0) {
        size_t got = 0;
        void *p = xRingbufferReceiveUpTo(s_rx_ring, &got, 0, need);
        if (!p || got == 0) {
            memset(dst, 0, need);
            s_rx_underruns++;
            break;
        }
        memcpy(dst, p, got);
        vRingbufferReturnItem(s_rx_ring, p);
        dst  += got;
        need -= got;
    }
    mixer_push_phone_pcm(pcm);
}

void bt_audio_sco_push_mic(const int16_t pcm[160])
{
    if (!s_sco_active || !s_tx_ring) return;

    /* Log the first mic push per session so the bench can see audio_io
     * is actually feeding SCO TX. Stage 3 first-bench showed tx=0 —
     * silence here pinpoints the loopback_task as dead. */
    if (s_tx_frames == 0 && s_tx_drops == 0) {
        ESP_LOGI(TAG, "first SCO TX push (loopback alive, sco_active=%d)",
                 (int)s_sco_active);
    }

    /* Append this tick's 10 ms PCM. Drop the tick rather than block
     * the audio loop if the AG isn't draining fast enough. */
    if (xRingbufferSend(s_tx_ring, pcm, PCM_FRAME_BYTES, 0) != pdTRUE) {
        s_tx_drops++;
        return;
    }
    s_tx_pending += PCM_FRAME_BYTES;

    /* Drain whole preferred-frame chunks to the AG. The 10 ms / 7.5 ms
     * cadence mismatch (10 ms in, 7.5 ms out for mSBC) means we send
     * either 1 or 2 chunks per tick depending on residual. */
    while (s_tx_pending >= s_tx_frame_bytes) {
        uint8_t chunk[SCO_FRAME_BYTES_MAX];
        size_t  filled = 0;
        while (filled < s_tx_frame_bytes) {
            size_t got = 0;
            void *p = xRingbufferReceiveUpTo(
                s_tx_ring, &got, 0, s_tx_frame_bytes - filled);
            if (!p) break;
            memcpy(chunk + filled, p, got);
            vRingbufferReturnItem(s_tx_ring, p);
            filled += got;
        }
        if (filled < s_tx_frame_bytes) {
            /* Shouldn't happen — pending said the bytes were there. */
            s_tx_drops++;
            break;
        }
        s_tx_pending -= filled;

        esp_hf_audio_buff_t *buf =
            esp_hf_client_audio_buff_alloc((uint16_t)filled);
        if (!buf) { s_tx_drops++; break; }
        memcpy(buf->data, chunk, filled);
        buf->data_len = (uint16_t)filled;
        if (esp_hf_client_audio_data_send(s_sco_handle, buf) == ESP_OK) {
            s_tx_frames++;
        } else {
            esp_hf_client_audio_buff_free(buf);
            s_tx_drops++;
        }
    }
}
