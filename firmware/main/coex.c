/*
 * coex — Wi-Fi/ESP-NOW ↔ BT Classic coexistence priority tuning.
 *
 * This module isolates the central single-radio risk. If a future
 * version migrates to two radios (e.g. adds a BM83 module on UART),
 * these calls become no-ops and nothing else has to change.
 */

#include "coex.h"

#include "esp_log.h"
#include "esp_coexist.h"

static const char *TAG = "coex";

void coex_init(void)
{
    ESP_LOGI(TAG, "init (balanced)");
    esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
}

void coex_prefer_bt_call(void)
{
    ESP_LOGI(TAG, "prefer BT (SCO active)");
    esp_coex_preference_set(ESP_COEX_PREFER_BT);
}

void coex_prefer_balanced(void)
{
    esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
}

void coex_prefer_wifi(void)
{
    ESP_LOGI(TAG, "prefer Wi-Fi (mesh-only)");
    esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
}
