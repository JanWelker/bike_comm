/*
 * audio_pipeline — I2S + ESP-SR AFE wiring.
 *
 * Status: skeleton. Real DMA + AFE wiring happens once a LyraT-Mini
 * is on the bench. The pin map below is the LyraT-Mini reference;
 * for a custom PCB it'll move to a board config header.
 */

#include "audio_pipeline.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "audio";

/* LyraT-Mini pin map — confirm against ESP-ADF board headers on bring-up. */
#define MIC_I2S_BCK_GPIO    27
#define MIC_I2S_WS_GPIO     25
#define MIC_I2S_DI_GPIO     26  /* INMP441 DOUT */

#define SPK_I2S_BCK_GPIO    5
#define SPK_I2S_WS_GPIO     25
#define SPK_I2S_DO_GPIO     26  /* ES8388 SDIN */
#define ES8388_I2C_SDA      18
#define ES8388_I2C_SCL      23

#define MIC_TASK_PRIO       22
#define AFE_TASK_PRIO       21
#define AUDIO_CORE          1

static QueueHandle_t mic_frame_q  = NULL;  /* post-AFE frames out  */
static QueueHandle_t spk_frame_q  = NULL;  /* speaker frames in    */

static void i2s_mic_rx_task(void *arg);
static void afe_task(void *arg);
static void i2s_spk_tx_task(void *arg);

esp_err_t audio_pipeline_init(void)
{
    ESP_LOGI(TAG, "init");

    mic_frame_q = xQueueCreate(4, sizeof(audio_frame_t));
    spk_frame_q = xQueueCreate(4, sizeof(audio_frame_t));
    if (!mic_frame_q || !spk_frame_q) {
        return ESP_ERR_NO_MEM;
    }

    /* TODO bring-up:
     *  - i2s_new_channel() for mic (RX) and codec (TX)
     *  - configure INMP441 (24-bit MSB-first, 32-bit slot, mono left)
     *  - configure ES8388 over I2C: 16 kHz, 16-bit, master mode
     *  - esp_afe_sr_v1_handle_t = afe_handle->create_from_config(...)
     *  - allocate AFE working buffers in PSRAM
     */

    return ESP_OK;
}

void audio_pipeline_start(void)
{
    xTaskCreatePinnedToCore(i2s_mic_rx_task, "mic_rx",   4096, NULL,
                            MIC_TASK_PRIO, NULL, AUDIO_CORE);
    xTaskCreatePinnedToCore(afe_task,        "afe",      8192, NULL,
                            AFE_TASK_PRIO, NULL, AUDIO_CORE);
    xTaskCreatePinnedToCore(i2s_spk_tx_task, "spk_tx",   4096, NULL,
                            MIC_TASK_PRIO, NULL, AUDIO_CORE);
}

esp_err_t audio_pipeline_get_mic_frame(audio_frame_t *out, TickType_t timeout)
{
    if (xQueueReceive(mic_frame_q, out, timeout) == pdTRUE) {
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t audio_pipeline_push_speaker_frame(const audio_frame_t *in)
{
    if (xQueueSend(spk_frame_q, in, 0) == pdTRUE) {
        return ESP_OK;
    }
    return ESP_ERR_NO_MEM;
}

/* ---- task bodies (placeholders) ---- */

static void i2s_mic_rx_task(void *arg)
{
    (void)arg;
    /* TODO: i2s_channel_read into raw buffer, downconvert 32->16 bit,
     *       deliver into AFE input ring. */
    vTaskDelete(NULL);
}

static void afe_task(void *arg)
{
    (void)arg;
    /* TODO: pull mic frame + AEC reference (from mixer loopback),
     *       run afe_handle->feed() then afe_handle->fetch(),
     *       publish output to mic_frame_q. */
    vTaskDelete(NULL);
}

static void i2s_spk_tx_task(void *arg)
{
    (void)arg;
    /* TODO: pull from spk_frame_q, i2s_channel_write to ES8388. */
    vTaskDelete(NULL);
}
