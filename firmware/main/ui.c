/*
 * ui — buttons + LED + battery monitor.
 *
 * Status: skeleton. GPIO pins are placeholders; real values come
 * from the LyraT-Mini board map (v0) and from hardware/v1 schematic later.
 */

#include "ui.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ui";

#define BTN_MODE_GPIO    36
#define BTN_VOL_UP_GPIO  39
#define BTN_VOL_DN_GPIO  34

#define LED_R_GPIO       22
#define LED_G_GPIO       19
#define LED_B_GPIO       21

#define BATT_ADC_GPIO    35   /* divider: V_bat/2 into ADC1_CH7 */

static ui_button_cb_t s_btn_cb = NULL;

static void ui_task(void *arg);

void ui_init(void)
{
    ESP_LOGI(TAG, "init");
    /* TODO:
     *   - configure GPIOs for buttons (input + pull-up, edge interrupts)
     *   - configure LED pins (LEDC PWM for smooth fades)
     *   - configure ADC1 oneshot for battery
     */
    xTaskCreate(ui_task, "ui", 2048, NULL, 8, NULL);
}

void ui_set_led(led_state_t state)
{
    (void)state;
    /* TODO: drive LEDC PWM per state (pulse / steady / blink). */
}

void ui_set_button_cb(ui_button_cb_t cb) { s_btn_cb = cb; }

uint16_t ui_get_battery_mv(void)
{
    /* TODO: read ADC1_CH7, scale by divider, average. */
    return 4100;
}

static void ui_task(void *arg)
{
    (void)arg;
    /* TODO: 50 ms tick. Debounce + long-press + combo detection. */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
