/*
 * ui — buttons + RGB LED + battery monitor.
 *
 * Three tactile buttons (glove-friendly): MODE/PTT, VOL+, VOL-.
 * Debouncing in firmware; long-press and combo detection in here.
 */

#pragma once

#include <stdint.h>

typedef enum {
    BTN_EVT_MODE_SHORT,    /* short press */
    BTN_EVT_MODE_LONG,     /* >= 1s */
    BTN_EVT_VOL_UP,
    BTN_EVT_VOL_DOWN,
    BTN_EVT_ALL_HELD,      /* all three held >= 5s — pairing reset */
} button_event_t;

typedef enum {
    LED_OFF,
    LED_IDLE,              /* slow blue pulse */
    LED_MESH_JOINED,       /* steady green */
    LED_PHONE_CONNECTED,   /* steady blue */
    LED_ERROR,             /* red */
    LED_PAIRING,           /* fast white blink */
} led_state_t;

typedef void (*ui_button_cb_t)(button_event_t evt);

void ui_init(void);
void ui_set_led(led_state_t state);
void ui_set_button_cb(ui_button_cb_t cb);

/* Returns battery voltage in millivolts (averaged over last second). */
uint16_t ui_get_battery_mv(void);
