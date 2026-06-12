/*
 * ui — buttons + LED + battery monitor.
 *
 * Buttons: LyraT-Mini v1.2 wires all six tactile buttons (Vol+, Vol-,
 * Set, Play, Mode, Rec) as a single resistor ladder into GPIO39 =
 * ADC1_CH3 (SENSOR_VN). Each press pulls the pin to a distinct
 * voltage; idle floats at the rail (~3000+ mV with 12 dB atten).
 * We sample at 50 Hz, classify by voltage band, debounce across two
 * samples (~40 ms), and emit an event on the press edge.
 *
 * Only VOL+ / VOL- are wired to the callback for now. The other four
 * buttons (REC / MODE / PLAY / SET) are recognized + logged so the
 * voltage bands can be calibrated on the bench, but they do not emit
 * events until session_fsm grows handlers for them.
 *
 * The voltage bands below mirror the ESP-ADF periph_adc_button
 * defaults for the LyraT key ladder. They are NOT verified against
 * this board revision — the per-press log line (raw mV) is the
 * ground truth; nudge the table if a press lands in the wrong band.
 *
 * LED + battery: still stubbed, see the TODOs.
 */

#include "ui.h"

#include <stdint.h>

#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

static const char *TAG = "ui";

/* ---- Button ladder on ADC1_CH3 / GPIO39 ---- */
#define BTN_ADC_UNIT      ADC_UNIT_1
#define BTN_ADC_CHANNEL   ADC_CHANNEL_3      /* GPIO39 / SENSOR_VN */
#define BTN_ADC_ATTEN     ADC_ATTEN_DB_12    /* ~0..2900 mV usable */
#define BTN_ADC_BITWIDTH  ADC_BITWIDTH_DEFAULT

#define BTN_IDLE_MIN_MV   2800   /* any reading >= this is "no press" — sits between REC (2.41 V) and the ~3.3 V rail */
#define BTN_TICK_MS       20
#define BTN_STABLE_TICKS  2      /* press confirmed after this many matching samples */

typedef enum {
    BTN_NONE = -1,
    BTN_REC = 0,
    BTN_MODE,
    BTN_PLAY,
    BTN_SET,
    BTN_VOL_DOWN,
    BTN_VOL_UP,
} btn_id_t;

struct btn_band {
    int      min_mv;       /* inclusive */
    int      max_mv;       /* exclusive */
    btn_id_t id;
};

/* Ascending in voltage. Per-button voltages are read directly from
 * the LyraT-Mini v1.2 schematic (10K pull-up to VDD33, switches
 * close to ground through a per-key ladder resistor R9..R14):
 *
 *     VOL+ 0.38 V   VOL- 0.82 V   SET 1.18 V
 *     PLAY 1.57 V   MODE 1.98 V   REC 2.41 V
 *
 * Band edges are midway between adjacent nominal voltages, then
 * widened to absorb the LX6 ADC's nonlinearity (~+/-100 mV near
 * the rails). */
static const struct btn_band BTN_BANDS[] = {
    {    0,  600, BTN_VOL_UP   },
    {  600, 1000, BTN_VOL_DOWN },
    { 1000, 1380, BTN_SET      },
    { 1380, 1780, BTN_PLAY     },
    { 1780, 2200, BTN_MODE     },
    { 2200, BTN_IDLE_MIN_MV, BTN_REC },
};

static const char *btn_name(btn_id_t b)
{
    switch (b) {
    case BTN_REC:      return "REC";
    case BTN_MODE:     return "MODE";
    case BTN_PLAY:     return "PLAY";
    case BTN_SET:      return "SET";
    case BTN_VOL_DOWN: return "VOL-";
    case BTN_VOL_UP:   return "VOL+";
    default:           return "NONE";
    }
}

/* ---- LED / battery placeholders ---- */
#define LED_R_GPIO       (-1)
#define LED_G_GPIO       (-1)
#define LED_B_GPIO       (-1)
#define BATT_ADC_GPIO    (-1)

static ui_button_cb_t s_btn_cb = NULL;

static adc_oneshot_unit_handle_t s_adc  = NULL;
static adc_cali_handle_t         s_cali = NULL;

static void ui_task(void *arg);

void ui_init(void)
{
    ESP_LOGI(TAG, "init (button ladder on GPIO39 / ADC1_CH3)");

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id  = BTN_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = BTN_ADC_BITWIDTH,
        .atten    = BTN_ADC_ATTEN,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, BTN_ADC_CHANNEL, &chan_cfg));

    /* ESP32 LX6 only supports line-fitting calibration; curve fitting
     * is S2/S3-only. eFuse Vref is picked up automatically when burnt;
     * default 1100 mV otherwise. Calibration failure is non-fatal — we
     * fall back to a coarse linear scaling of the raw read. */
    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id  = BTN_ADC_UNIT,
        .atten    = BTN_ADC_ATTEN,
        .bitwidth = BTN_ADC_BITWIDTH,
    };
    esp_err_t err = adc_cali_create_scheme_line_fitting(&cali_cfg, &s_cali);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "adc cali unavailable (%s); using raw scaling",
                 esp_err_to_name(err));
        s_cali = NULL;
    }

    /* 50 Hz tick, ~40 ms debounce — comfortable for human presses
     * without burning core 0. Stack: ~2.5 KB covers the ADC + log
     * call path. */
    xTaskCreate(ui_task, "ui", 2560, NULL, 8, NULL);
}

void ui_set_led(led_state_t state)
{
    (void)state;
    /* TODO: drive LEDC PWM per state (pulse / steady / blink). */
}

void ui_set_button_cb(ui_button_cb_t cb) { s_btn_cb = cb; }

uint16_t ui_get_battery_mv(void)
{
    /* TODO: read battery divider, scale, average. */
    return 4100;
}

static int read_button_mv(void)
{
    int raw = 0;
    if (adc_oneshot_read(s_adc, BTN_ADC_CHANNEL, &raw) != ESP_OK) return -1;
    if (!s_cali) {
        /* Coarse fallback: 12-bit raw scaled to ~2900 mV at 12 dB atten. */
        return (raw * 2900) / 4095;
    }
    int mv = 0;
    if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) return -1;
    return mv;
}

static btn_id_t classify(int mv)
{
    if (mv < 0 || mv >= BTN_IDLE_MIN_MV) return BTN_NONE;
    for (size_t i = 0; i < sizeof(BTN_BANDS)/sizeof(BTN_BANDS[0]); ++i) {
        if (mv >= BTN_BANDS[i].min_mv && mv < BTN_BANDS[i].max_mv) {
            return BTN_BANDS[i].id;
        }
    }
    return BTN_NONE;
}

static void emit(btn_id_t b)
{
    if (!s_btn_cb) return;
    switch (b) {
    case BTN_VOL_UP:   s_btn_cb(BTN_EVT_VOL_UP);   break;
    case BTN_VOL_DOWN: s_btn_cb(BTN_EVT_VOL_DOWN); break;
    default: break;  /* MODE/SET/PLAY/REC not wired to events yet */
    }
}

static void ui_task(void *arg)
{
    (void)arg;

    /* cur_stable: the press we last confirmed. pending / pending_n:
     * the candidate we're trying to confirm. We emit on the release-
     * to-press edge only, so holding a button does not repeat — that
     * matches the chosen "10 % per press, no auto-repeat" behavior. */
    btn_id_t cur_stable = BTN_NONE;
    btn_id_t pending    = BTN_NONE;
    int      pending_n  = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(BTN_TICK_MS));

        int mv = read_button_mv();
        btn_id_t obs = classify(mv);

        if (obs == pending) {
            if (pending_n < BTN_STABLE_TICKS) pending_n++;
        } else {
            pending   = obs;
            pending_n = 1;
        }

        if (pending_n < BTN_STABLE_TICKS) continue;
        if (pending == cur_stable)        continue;

        cur_stable = pending;
        if (cur_stable != BTN_NONE) {
            ESP_LOGI(TAG, "btn %s (%d mV)", btn_name(cur_stable), mv);
            emit(cur_stable);
        }
    }
}
