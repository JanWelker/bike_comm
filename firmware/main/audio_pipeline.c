/*
 * audio_pipeline — v0 mic -> speaker loopback on ESP32-LyraT-Mini v1.2.
 *
 * Goal: confirm the I2S/codec signal path end-to-end on real hardware before
 * any DSP, mesh, or codec work goes near it. No AFE, no AEC, no LC3 — just
 * read mic samples and write them straight to the speaker.
 *
 * --------------------------------------------------------------------------
 * BOARD: ESP32-LyraT-Mini v1.2
 *
 *   Heads-up for future readers: despite the name, this board carries TWO
 *   audio chips, not one. The brief described it as "single ES8311 mono
 *   codec", which is wrong — that's a different LyraT variant. The actual
 *   wiring (verified against ESP-ADF's official board headers, source URL
 *   below) is:
 *
 *     - ES8311  -> playback DAC + HP/speaker driver  -> I2S0 (TX)
 *     - ES7243E -> mic preamp + ADC                  -> I2S1 (RX)
 *
 *   Both share a single I2C bus for control. The two codecs run as I2S
 *   slaves; the ESP32 is master on both ports.
 *
 *   Authoritative source:
 *     https://github.com/espressif/esp-adf/blob/release/v2.x/components/audio_board/lyrat_mini_v1_1/board_def.h
 *     https://github.com/espressif/esp-adf/blob/release/v2.x/components/audio_board/lyrat_mini_v1_1/board_pins_config.c
 *     (the "lyrat_mini_v1_1" folder also covers the v1.2 silkscreen rev)
 *
 *   GPIO pin map:
 *     I2C (shared)        SDA=GPIO18  SCL=GPIO23
 *     ES8311  (DAC, I2C addr 0x18 7-bit / 0x30 8-bit)
 *     ES7243E (ADC, I2C addr 0x10 7-bit / 0x20 8-bit)
 *
 *     I2S0 -> ES8311 (playback):
 *       BCLK = GPIO5   WS = GPIO25
 *       DOUT = GPIO26 (ESP -> codec)
 *       DIN  = GPIO35 (codec -> ESP; AEC reference loopback path, unused in v0)
 *       MCLK is NOT routed on I2S0 — the ES8311 derives its internal clock
 *       from BCLK (board_def.h: ES8311_MCLK_SOURCE = 1 = "from BCLK").
 *
 *     I2S1 -> ES7243E (capture):
 *       MCLK = GPIO0   BCLK = GPIO32   WS = GPIO33
 *       DIN  = GPIO36 (codec -> ESP)
 *       MCLK is required (ES7243E needs an external master clock).
 *
 *     Speaker PA enable: GPIO21 (drive HIGH to unmute the on-board NS4150 amp)
 *     Headphone detect:  GPIO19 (input, ignored in v0)
 *
 *   GPIO35 and GPIO36 are input-only pads on the original ESP32 — fine for
 *   I2S DIN.
 * --------------------------------------------------------------------------
 *
 * SOFTWARE STACK:
 *   - ESP-IDF v5.5 new I2S driver (driver/i2s_std.h)
 *   - `espressif/esp_codec_dev` managed component for both codec drivers
 *     (chosen over the now-deprecated `espressif/es8311` driver and over
 *      a hand-rolled register sequence — esp_codec_dev is maintained,
 *      already supports ES7243E, and yields a single, consistent API for
 *      both chips. Trade-off: one more managed dep to pull on first build.)
 *
 * THREADING:
 *   audio_pipeline_init()  - allocates everything, leaves channels disabled.
 *   audio_pipeline_start() - enables I2S, opens codecs, drives PA high,
 *                            spawns the loopback task on Core 1, prio 22.
 *
 *   loopback task: every 10 ms, read 160 mic samples, LC3-encode them,
 *   hand them to mesh_mac_queue_tx, drain any incoming mesh frames into
 *   the mixer, mixer_pull the speaker frame, write it to the codec.
 *   esp_codec_dev's read/write APIs go through the I2S DMA underneath,
 *   so the task blocks naturally on DMA. No intermediate queues; this
 *   is the whole hot path.
 *
 *   Latency: 4 x 240-sample DMA buffers x 2 ports ~= 60 ms end-to-end,
 *   generous and fine for v0.
 */

#include "audio_pipeline.h"
#include "codec.h"
#include "mesh_mac.h"
#include "mixer.h"

/* Implemented in main.c: drains the wifi-RX-to-mixer queue. Declared
 * here as extern so the audio_io task can drive it on every tick. */
extern void mesh_rx_drain_to_mixer(void);

#include <string.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"

#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_timer.h"

#include "noise_suppression.h"

static const char *TAG = "audio";

_Static_assert(AUDIO_FRAME_SAMPLES == CODEC_FRAME_SAMPLES &&
               AUDIO_FRAME_SAMPLES == MIXER_PCM_SAMPLES,
               "audio tick, codec, and mixer must share one frame size");
_Static_assert(AUDIO_SR_HZ == CODEC_SAMPLE_RATE_HZ,
               "audio sample rate must match the codec's");

/* WebRTC_NS state — allocated in audio_pipeline_start. The struct is
 * ~20-25 KB so it goes to PSRAM by default (above our 16 KB
 * SPIRAM_MALLOC_ALWAYSINTERNAL threshold). NS only runs once per
 * 10 ms tick, not in an ISR, so PSRAM latency is fine here.
 *
 * Mode: 1 = Medium (10 dB attenuation). Mild misses too much
 * background; Aggressive eats consonants. */
static NsHandle *s_ns = NULL;

/* Per-call wall-clock perf counters for WebRtcNs_Process, in the same
 * shape as codec_lc3's counters. Logged + reset every 1000 frames by
 * codec_perf_log_and_reset's neighbor below. */
static int64_t  s_ns_us_sum = 0;
static int64_t  s_ns_us_max = 0;
static uint32_t s_ns_count  = 0;

/* VAD gate. WebRTC NS already computes a prior speech probability as
 * part of its noise estimate; we ride along on it instead of running
 * a separate VAD. Above-threshold frames are "speech"; we then hold
 * the gate open for VAD_HOLD_FRAMES (= 500 ms at 100 fps) so word-
 * internal pauses don't toggle the gate.
 *
 * VAD-inactive frames skip BOTH the LC3 encode (~4.5 ms of the 10 ms
 * tick — the single largest line item in the core-1 budget) and the
 * TX queue: the mesh task keeps sending header-only frames every
 * superframe (the slot-claim / peer-quiet-timeout invariant), which
 * the receiver's mixer turns into decoded-for-free silence. Skipping
 * the encode leaves a small gap in the LC3 encoder's overlap history;
 * the first frame after re-activation may carry a minor artifact —
 * accepted trade-off. It does NOT yet skip the TX packet itself;
 * that needs a heartbeat scheme — separate v0.5 TODO. */
#define VAD_SPEECH_THRESHOLD   0.5f
#define VAD_HOLD_FRAMES        50

static int      s_vad_hold          = 0;
static uint32_t s_vad_active_count  = 0;
static uint32_t s_vad_total_count   = 0;
static float    s_vad_prob_max      = 0.0f;   /* per diag window */

/* ---- Board pinout (LyraT-Mini v1.2) ---- */
#define I2C_SDA_GPIO            GPIO_NUM_18
#define I2C_SCL_GPIO            GPIO_NUM_23
#define I2C_CTRL_PORT           I2C_NUM_0
#define I2C_CTRL_FREQ_HZ        100000

/* Playback path: I2S0 -> ES8311 */
#define SPK_I2S_PORT            I2S_NUM_0
#define SPK_I2S_MCLK_GPIO       I2S_GPIO_UNUSED /* ES8311 sources MCLK from BCLK */
#define SPK_I2S_BCLK_GPIO       GPIO_NUM_5
#define SPK_I2S_WS_GPIO         GPIO_NUM_25
#define SPK_I2S_DOUT_GPIO       GPIO_NUM_26
#define SPK_I2S_DIN_GPIO        I2S_GPIO_UNUSED /* GPIO35 wired but unused in v0 */

/* Capture path: I2S1 -> ES7243E */
#define MIC_I2S_PORT            I2S_NUM_1
#define MIC_I2S_MCLK_GPIO       GPIO_NUM_0
#define MIC_I2S_BCLK_GPIO       GPIO_NUM_32
#define MIC_I2S_WS_GPIO         GPIO_NUM_33
#define MIC_I2S_DOUT_GPIO       I2S_GPIO_UNUSED
#define MIC_I2S_DIN_GPIO        GPIO_NUM_36

#define PA_ENABLE_GPIO          GPIO_NUM_21

/* ---- Audio config ---- */
#define AUDIO_BITS_PER_SAMPLE   16
#define AUDIO_CHANNELS          1               /* mono */
#define LOOPBACK_FRAME_BYTES    (AUDIO_FRAME_SAMPLES * sizeof(int16_t))

/* DMA: 4 x 240-sample buffers ~= 60 ms backlog at 16 kHz (per direction). */
#define I2S_DMA_DESC_NUM        4
#define I2S_DMA_FRAME_NUM       240

#define LOOPBACK_TASK_PRIO      22
#define AUDIO_CORE              1

/* ---- Module state ---- */
static i2c_master_bus_handle_t s_i2c_bus = NULL;

static i2s_chan_handle_t       s_spk_tx_chan = NULL;  /* ES8311 DAC on I2S0 */
static i2s_chan_handle_t       s_mic_rx_chan = NULL;  /* ES7243 ADC on I2S1 */

static const audio_codec_data_if_t *s_spk_data_if = NULL;
static const audio_codec_data_if_t *s_mic_data_if = NULL;
static const audio_codec_ctrl_if_t *s_spk_ctrl_if = NULL;
static const audio_codec_ctrl_if_t *s_mic_ctrl_if = NULL;
static const audio_codec_gpio_if_t *s_gpio_if     = NULL;
static const audio_codec_if_t      *s_es8311_if   = NULL;
static const audio_codec_if_t      *s_es7243e_if  = NULL;

static esp_codec_dev_handle_t  s_spk_dev = NULL;
static esp_codec_dev_handle_t  s_mic_dev = NULL;

static volatile bool           s_running     = false;

/* Speaker output volume in percent. UI pokes this via
 * audio_pipeline_vol_step; the loopback task never touches it. */
static int                     s_spk_vol_pct = 100;

/* ---- forward decls ---- */
static esp_err_t init_i2c_bus(void);
static esp_err_t init_i2s_channels(void);
static esp_err_t init_codecs(void);
static esp_err_t init_pa_gpio(void);
static void      loopback_task(void *arg);

/* ====================================================================== */

esp_err_t audio_pipeline_init(void)
{
    ESP_LOGI(TAG, "init (LyraT-Mini v1.2: ES8311 playback + ES7243E mic)");

    ESP_RETURN_ON_ERROR(init_pa_gpio(),       TAG, "pa gpio init");
    ESP_RETURN_ON_ERROR(init_i2c_bus(),       TAG, "i2c bus init");
    ESP_RETURN_ON_ERROR(init_i2s_channels(),  TAG, "i2s channels init");
    ESP_RETURN_ON_ERROR(init_codecs(),        TAG, "codec init");

    ESP_LOGI(TAG, "init done; channels created but not yet enabled");
    return ESP_OK;
}

void audio_pipeline_start(void)
{
    if (s_running) {
        ESP_LOGW(TAG, "already running");
        return;
    }

    /* Bring up the data path before the codec drivers start clocking it. */
    ESP_ERROR_CHECK(i2s_channel_enable(s_spk_tx_chan));
    ESP_ERROR_CHECK(i2s_channel_enable(s_mic_rx_chan));

    /* Open both codec devices at the same sample format. */
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = AUDIO_BITS_PER_SAMPLE,
        .channel         = AUDIO_CHANNELS,
        .channel_mask    = 0x01,
        .sample_rate     = AUDIO_SR_HZ,
    };

    int rc = esp_codec_dev_open(s_spk_dev, &fs);
    if (rc != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "spk codec open failed: %d", rc);
        return;
    }
    /* For the mic read we want STEREO frames so the I2S RX delivers
     * just the right slot (where ES7243 puts AINRP/AINRN). */
    esp_codec_dev_sample_info_t mic_fs = fs;
    mic_fs.channel      = 2;
    mic_fs.channel_mask = 0x02;
    rc = esp_codec_dev_open(s_mic_dev, &mic_fs);
    if (rc != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "mic codec open failed: %d", rc);
        return;
    }

    /* Volume + gain tuned for v0 bench loopback. Mic at 33 dB stays
     * a couple of dB clear of ES7243's 37.5 dB saturation ceiling.
     * Speaker volume is mutable from the UI via vol_step; we apply the
     * current value (initially 100 %) here so a pre-start step call
     * still takes effect at start. */
    esp_codec_dev_set_out_vol(s_spk_dev, s_spk_vol_pct);
    esp_codec_dev_set_in_gain(s_mic_dev, 33.0f);

    /* Speaker amp must be high to actually hear anything. */
    gpio_set_level(PA_ENABLE_GPIO, 1);
    ESP_LOGI(TAG, "PA enable high");

    /* Bring up WebRTC noise suppression. Allocated here (after
     * mesh_mac_start has grabbed its DRAM task stack in app_main)
     * so the ~25 KB NS state goes to PSRAM via the default IDF
     * malloc above the 16 KB internal-RAM threshold. */
    s_ns = WebRtcNs_Create();
    ESP_ERROR_CHECK(s_ns ? ESP_OK : ESP_ERR_NO_MEM);
    if (WebRtcNs_Init(s_ns, AUDIO_SR_HZ) != 0) {
        ESP_LOGE(TAG, "WebRtcNs_Init failed");
        abort();
    }
    /* Mode 1 = Medium (10 dB attenuation). */
    if (WebRtcNs_set_policy(s_ns, 1) != 0) {
        ESP_LOGE(TAG, "WebRtcNs_set_policy failed");
        abort();
    }
    ESP_LOGI(TAG, "WebRTC_NS up (mode=medium, %d Hz)", AUDIO_SR_HZ);

    s_running = true;
    /* 4 KB overflows once LC3 encode + cross-core mesh mutex enter the
     * hot path (Guru Meditation on core 0 via the cross-core mutex).
     * 6 KB survives queue_tx + drain. mixer_pull (LC3 decode + 160-
     * sample int32 accumulator + per-rider PCM buffer) needs more —
     * trying 7 KB. 8 KB is the upper bound that still leaves enough
     * internal RAM for mesh_tx_task to be created afterwards. */
    BaseType_t ok = xTaskCreatePinnedToCore(loopback_task, "audio_loopback",
                                            7168, NULL, LOOPBACK_TASK_PRIO,
                                            NULL, AUDIO_CORE);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "loopback task create failed");
        s_running = false;
    } else {
        ESP_LOGI(TAG, "loopback task running on core %d", AUDIO_CORE);
    }
}

void audio_pipeline_vol_step(int delta_pct)
{
    int v = s_spk_vol_pct + delta_pct;
    if (v < 0)   v = 0;
    if (v > 100) v = 100;
    if (v == s_spk_vol_pct) return;
    s_spk_vol_pct = v;
    if (s_spk_dev) {
        esp_codec_dev_set_out_vol(s_spk_dev, v);
    }
    ESP_LOGI(TAG, "spk vol %d%%", v);
}

int audio_pipeline_get_vol_pct(void) { return s_spk_vol_pct; }

/* ====================================================================== */
/* setup helpers                                                          */
/* ====================================================================== */

static esp_err_t init_pa_gpio(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PA_ENABLE_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "PA gpio config");
    /* Hold the amp muted until the I2S DMA is actually producing samples,
     * otherwise the first packet of DMA noise pops through the speaker. */
    gpio_set_level(PA_ENABLE_GPIO, 0);
    return ESP_OK;
}

static esp_err_t init_i2c_bus(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port                     = I2C_CTRL_PORT,
        .sda_io_num                   = I2C_SDA_GPIO,
        .scl_io_num                   = I2C_SCL_GPIO,
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
}

static esp_err_t init_i2s_channels(void)
{
    /* --- I2S0: speaker TX --- */
    i2s_chan_config_t spk_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(SPK_I2S_PORT,
                                                                I2S_ROLE_MASTER);
    spk_chan_cfg.dma_desc_num  = I2S_DMA_DESC_NUM;
    spk_chan_cfg.dma_frame_num = I2S_DMA_FRAME_NUM;
    spk_chan_cfg.auto_clear    = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&spk_chan_cfg, &s_spk_tx_chan, NULL),
                        TAG, "i2s0 new_channel");

    i2s_std_config_t spk_std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SR_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = SPK_I2S_MCLK_GPIO,
            .bclk = SPK_I2S_BCLK_GPIO,
            .ws   = SPK_I2S_WS_GPIO,
            .dout = SPK_I2S_DOUT_GPIO,
            .din  = SPK_I2S_DIN_GPIO,
            .invert_flags = { 0 },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_spk_tx_chan, &spk_std),
                        TAG, "i2s0 init_std_mode");

    /* --- I2S1: mic RX --- */
    i2s_chan_config_t mic_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(MIC_I2S_PORT,
                                                                I2S_ROLE_MASTER);
    mic_chan_cfg.dma_desc_num  = I2S_DMA_DESC_NUM;
    mic_chan_cfg.dma_frame_num = I2S_DMA_FRAME_NUM;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&mic_chan_cfg, NULL, &s_mic_rx_chan),
                        TAG, "i2s1 new_channel");

    i2s_std_config_t mic_std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SR_HZ),
        /* Stereo slot, but mask to RIGHT only: the mic is on ES7243's
         * AINRP/AINRN (right channel); the LEFT slot carries the AEC
         * loopback (ES8311 OUTP/OUTN fed back into AINLP/AINLN). */
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = MIC_I2S_MCLK_GPIO,
            .bclk = MIC_I2S_BCLK_GPIO,
            .ws   = MIC_I2S_WS_GPIO,
            .dout = MIC_I2S_DOUT_GPIO,
            .din  = MIC_I2S_DIN_GPIO,
            .invert_flags = { 0 },
        },
    };
    /* ES7243 needs an external MCLK; default multiplier (256x fs) is fine. */
    mic_std.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    mic_std.slot_cfg.slot_mask    = I2S_STD_SLOT_RIGHT;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_mic_rx_chan, &mic_std),
                        TAG, "i2s1 init_std_mode");

    return ESP_OK;
}

static esp_err_t init_codecs(void)
{
    s_gpio_if = audio_codec_new_gpio();
    if (!s_gpio_if) {
        ESP_LOGE(TAG, "codec gpio if alloc");
        return ESP_ERR_NO_MEM;
    }

    /* --- ES8311 (playback) --- */
    {
        audio_codec_i2c_cfg_t i2c_cfg = {
            .port       = I2C_CTRL_PORT,
            .addr       = ES8311_CODEC_DEFAULT_ADDR,
            .bus_handle = s_i2c_bus,
        };
        s_spk_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
        if (!s_spk_ctrl_if) {
            ESP_LOGE(TAG, "es8311 i2c ctrl");
            return ESP_FAIL;
        }

        audio_codec_i2s_cfg_t i2s_cfg = {
            .port      = SPK_I2S_PORT,
            .tx_handle = s_spk_tx_chan,
            .rx_handle = NULL,
        };
        s_spk_data_if = audio_codec_new_i2s_data(&i2s_cfg);
        if (!s_spk_data_if) {
            ESP_LOGE(TAG, "es8311 i2s data if");
            return ESP_FAIL;
        }

        es8311_codec_cfg_t es8311_cfg = {
            .ctrl_if     = s_spk_ctrl_if,
            .gpio_if     = s_gpio_if,
            .codec_mode  = ESP_CODEC_DEV_WORK_MODE_DAC,
            .master_mode = false,
            .use_mclk    = false,  /* MCLK derived from BCLK on LyraT-Mini */
            .pa_pin      = -1,     /* PA controlled by us directly, not the driver */
            .pa_reverted = false,
            .hw_gain = {
                .pa_voltage        = 5.0f,
                .codec_dac_voltage = 3.3f,
            },
        };
        s_es8311_if = es8311_codec_new(&es8311_cfg);
        if (!s_es8311_if) {
            ESP_LOGE(TAG, "es8311_codec_new");
            return ESP_FAIL;
        }

        esp_codec_dev_cfg_t dev_cfg = {
            .dev_type = ESP_CODEC_DEV_TYPE_OUT,
            .codec_if = s_es8311_if,
            .data_if  = s_spk_data_if,
        };
        s_spk_dev = esp_codec_dev_new(&dev_cfg);
        if (!s_spk_dev) {
            ESP_LOGE(TAG, "spk esp_codec_dev_new");
            return ESP_FAIL;
        }
    }

    /* --- ES7243E (mic) --- */
    {
        audio_codec_i2c_cfg_t i2c_cfg = {
            .port       = I2C_CTRL_PORT,
            .addr       = ES7243E_CODEC_DEFAULT_ADDR,
            .bus_handle = s_i2c_bus,
        };
        s_mic_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
        if (!s_mic_ctrl_if) {
            ESP_LOGE(TAG, "es7243e i2c ctrl");
            return ESP_FAIL;
        }

        audio_codec_i2s_cfg_t i2s_cfg = {
            .port      = MIC_I2S_PORT,
            .tx_handle = NULL,
            .rx_handle = s_mic_rx_chan,
        };
        s_mic_data_if = audio_codec_new_i2s_data(&i2s_cfg);
        if (!s_mic_data_if) {
            ESP_LOGE(TAG, "es7243e i2s data if");
            return ESP_FAIL;
        }

        es7243e_codec_cfg_t es7243e_cfg = {
            .ctrl_if = s_mic_ctrl_if,
        };
        s_es7243e_if = es7243e_codec_new(&es7243e_cfg);
        if (!s_es7243e_if) {
            ESP_LOGE(TAG, "es7243e_codec_new");
            return ESP_FAIL;
        }

        esp_codec_dev_cfg_t dev_cfg = {
            .dev_type = ESP_CODEC_DEV_TYPE_IN,
            .codec_if = s_es7243e_if,
            .data_if  = s_mic_data_if,
        };
        s_mic_dev = esp_codec_dev_new(&dev_cfg);
        if (!s_mic_dev) {
            ESP_LOGE(TAG, "mic esp_codec_dev_new");
            return ESP_FAIL;
        }
    }

    /* Don't override s_mic_dev — keep it pointed at the ES7243 dev. */

    /* Mic source is the ES7243 dev built above. The schematic confirms
     * the J6 MIC connector goes through C64/C65 (populated) to ES7243's
     * AINRP/AINRN (right channel, pins 15/16). ES8311's MIC pins are
     * dead because the C21/C22 coupling caps are marked NC. ES7243's
     * AINLP/AINLN carry ES8311's OUTP/OUTN as the AEC loopback. */
    ESP_LOGI(TAG, "mic source: ES7243 AINRP/AINRN (right channel)");

    return ESP_OK;
}

/* ====================================================================== */
/* loopback task                                                          */
/* ====================================================================== */

/* Diagnostic: when set, log mic/speaker peak and RMS once per second.
 * Useful for tuning gain on the bench; left on by default for v0. The
 * level scan runs only on the tick that logs (a 1-frame point sample —
 * same information the per-frame scan produced, since only the logged
 * frame's values were ever reported). */
#define AUDIO_DIAG_MIC_LEVEL 1

#if AUDIO_DIAG_MIC_LEVEL
static void diag_log_level(const char *what, const int16_t *pcm,
                           uint32_t *frame_counter)
{
    if ((++*frame_counter % 100) != 0) return;  /* every 1 s @ 10 ms frames */
    int16_t peak = 0;
    int64_t sum_sq = 0;
    for (int i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
        int16_t s = pcm[i];
        int16_t a = s < 0 ? -s : s;
        if (a > peak) peak = a;
        sum_sq += (int32_t)s * (int32_t)s;
    }
    uint32_t rms = (uint32_t)__builtin_sqrt((double)sum_sq /
                                            AUDIO_FRAME_SAMPLES);
    ESP_LOGI(TAG, "%s level: peak=%d rms=%lu", what, peak,
             (unsigned long)rms);
}
#endif /* AUDIO_DIAG_MIC_LEVEL */

static void loopback_task(void *arg)
{
    (void)arg;

    int16_t *buf = heap_caps_malloc(LOOPBACK_FRAME_BYTES, MALLOC_CAP_DMA);
    if (!buf) {
        ESP_LOGE(TAG, "loopback: no DMA buf");
        vTaskDelete(NULL);
        return;
    }

    /* Pre-allocate the LC3 encode buffer + the mixer pull buffer so we
     * don't hit malloc in the hot loop. */
    uint8_t lc3_buf[CODEC_FRAME_BYTES];
    int16_t spk_buf[AUDIO_FRAME_SAMPLES];

    ESP_LOGI(TAG, "audio io: %d samples/frame @ %d Hz, mic->mesh + mixer->spk",
             AUDIO_FRAME_SAMPLES, AUDIO_SR_HZ);

    uint32_t underruns = 0;
    while (s_running) {
        /* --- Capture: mic -> LC3 -> mesh queue. */
        int rc = esp_codec_dev_read(s_mic_dev, buf, LOOPBACK_FRAME_BYTES);
        if (rc != ESP_CODEC_DEV_OK) {
            if ((++underruns % 100) == 1) {
                ESP_LOGW(TAG, "audio io: mic read err %d (count %lu)", rc,
                         (unsigned long)underruns);
            }
            continue;
        }
#if AUDIO_DIAG_MIC_LEVEL
        {
            static uint32_t s_mic_diag_n = 0;
            diag_log_level("mic", buf, &s_mic_diag_n);
        }
#endif /* AUDIO_DIAG_MIC_LEVEL */
        /* Noise suppression in place between the mic capture and the
         * LC3 encode. WebRTC_NS operates on 10 ms / 160-sample int16
         * blocks at 16 kHz — same cadence as our LC3 frame.
         *
         * Analyze must run for the speech/noise probability model
         * (SpeechNoiseProb -> priorSpeechProb) to update — it only
         * does so inside AnalyzeCore. Without it the prior stays at
         * its 0.5 init forever and the VAD gate (strictly > 0.5)
         * never opens — the bug that shipped the intercom mute.
         *
         * Analyze runs every 2nd frame: at ~2.6 ms per call on the
         * LX6 the every-frame version pushed the speech-period tick
         * to ~11.5 ms (over the 10 ms budget). At 50 updates/s the
         * probability model still settles orders of magnitude faster
         * than the 500 ms VAD hold; the noise estimate Process
         * consumes just lags one frame. The ns perf counter below
         * covers Analyze + Process together, so the logged mean is
         * the true per-tick average. */
        static bool s_ns_analyze_this_frame = true;
        int16_t *ns_in[1]  = { buf };
        int16_t *ns_out[1] = { buf };
        int64_t ns_t0 = esp_timer_get_time();
        if (s_ns_analyze_this_frame) {
            WebRtcNs_Analyze(s_ns, buf);
        }
        s_ns_analyze_this_frame = !s_ns_analyze_this_frame;
        WebRtcNs_Process(s_ns, (const int16_t *const *)ns_in, 1, ns_out);
        int64_t ns_dt = esp_timer_get_time() - ns_t0;
        s_ns_us_sum += ns_dt;
        if (ns_dt > s_ns_us_max) s_ns_us_max = ns_dt;
        s_ns_count++;

        /* VAD via WebRTC NS's internal speech probability + hold. */
        float speech_prob = WebRtcNs_prior_speech_probability(s_ns);
        if (speech_prob > s_vad_prob_max) s_vad_prob_max = speech_prob;
        bool vad_active;
        if (speech_prob > VAD_SPEECH_THRESHOLD) {
            vad_active = true;
            s_vad_hold = VAD_HOLD_FRAMES;
        } else if (s_vad_hold > 0) {
            vad_active = true;
            s_vad_hold--;
        } else {
            vad_active = false;
        }
        s_vad_total_count++;
        if (vad_active) s_vad_active_count++;

        /* Encode + queue only speech frames. During silence the mesh
         * task still claims our slot with header-only frames, so the
         * ~4.5 ms encode would produce bytes nobody decodes — the
         * receiver's mixer skips VAD-inactive payloads anyway. */
        if (vad_active) {
            codec_encode(buf, lc3_buf);
            mesh_mac_queue_tx(lc3_buf, true);
        }
        mesh_rx_drain_to_mixer();

        mixer_pull(spk_buf, NULL /* aec_ref: no consumer until v0.5 AEC */);
#if AUDIO_DIAG_MIC_LEVEL
        {
            static uint32_t s_spk_diag_n = 0;
            diag_log_level("spk", spk_buf, &s_spk_diag_n);
        }
#endif
        /* LC3 + NS + VAD diagnostic log every 1000 frames (~10 s).
         * Numbers land in docs/codec_perf.md. */
        {
            static uint32_t s_perf_n = 0;
            if ((++s_perf_n % 1000) == 0) {
                codec_perf_log_and_reset();
                if (s_ns_count > 0) {
                    ESP_LOGI(TAG, "ns: n=%u mean=%lld us max=%lld us",
                             (unsigned)s_ns_count,
                             (long long)(s_ns_us_sum / (int64_t)s_ns_count),
                             (long long)s_ns_us_max);
                    s_ns_us_sum = 0; s_ns_us_max = 0; s_ns_count = 0;
                }
                if (s_vad_total_count > 0) {
                    ESP_LOGI(TAG, "vad: %u/%u frames active (%u%%), "
                             "max prob %d%%",
                             (unsigned)s_vad_active_count,
                             (unsigned)s_vad_total_count,
                             (unsigned)((100u * s_vad_active_count) /
                                        s_vad_total_count),
                             (int)(s_vad_prob_max * 100.0f));
                    s_vad_active_count = 0;
                    s_vad_total_count  = 0;
                    s_vad_prob_max     = 0.0f;
                }
            }
        }
        int rc_w = esp_codec_dev_write(s_spk_dev, spk_buf, LOOPBACK_FRAME_BYTES);
        if (rc_w != ESP_CODEC_DEV_OK) {
            if ((++underruns % 100) == 1) {
                ESP_LOGW(TAG, "audio io: spk write err %d (count %lu)", rc_w,
                         (unsigned long)underruns);
            }
        }
    }

    heap_caps_free(buf);
    vTaskDelete(NULL);
}
