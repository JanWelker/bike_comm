/*
 * nvs_cfg — persistent settings backed by NVS namespace "cfg".
 *
 * This one's a real implementation (no hardware dependency) and is
 * useful for unit testing on host with the ESP-IDF NVS simulator.
 */

#include "nvs_cfg.h"

#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG    = "cfg";
static const char *NS     = "cfg";
static const char *K_PSK   = "psk";
static const char *K_BD    = "bd_addr";
static const char *K_NICK  = "nick";
static const char *K_NONCE = "nonce_hi";    /* mesh-crypto nonce_lo watermark */

esp_err_t nvs_cfg_init(void)
{
    ESP_LOGI(TAG, "init");

    uint8_t psk[16];
    esp_err_t err = nvs_cfg_get_psk(psk);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no PSK present; generating one");
        esp_fill_random(psk, sizeof(psk));
        err = nvs_cfg_set_psk(psk);
    }
    return err;
}

esp_err_t nvs_cfg_get_psk(uint8_t out_psk[16])
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t len = 16;
    err = nvs_get_blob(h, K_PSK, out_psk, &len);
    nvs_close(h);
    if (err == ESP_OK && len != 16) return ESP_ERR_INVALID_SIZE;
    return err;
}

esp_err_t nvs_cfg_set_psk(const uint8_t psk[16])
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, K_PSK, psk, 16);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t nvs_cfg_get_phone_addr(esp_bd_addr_t *out)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t len = sizeof(esp_bd_addr_t);
    err = nvs_get_blob(h, K_BD, out, &len);
    nvs_close(h);
    if (err == ESP_OK && len != sizeof(esp_bd_addr_t)) return ESP_ERR_INVALID_SIZE;
    return err;
}

esp_err_t nvs_cfg_set_phone_addr(const esp_bd_addr_t *addr)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, K_BD, addr, sizeof(*addr));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t nvs_cfg_clear_phone_addr(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(h, K_BD);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

esp_err_t nvs_cfg_get_nickname(char out[17])
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t len = 17;
    err = nvs_get_str(h, K_NICK, out, &len);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        strcpy(out, "rider");
        return ESP_OK;
    }
    return err;
}

esp_err_t nvs_cfg_alloc_nonce_window(uint32_t window, uint32_t *out_start)
{
    if (window == 0 || out_start == NULL) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    uint32_t w = 0;
    err = nvs_get_u32(h, K_NONCE, &w);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        w   = 0;
        err = ESP_OK;
    }
    if (err != ESP_OK) {
        nvs_close(h);
        return err;
    }

    /* Reserve [w, w+window). If the increment would wrap the 32-bit
     * counter we refuse: rotating the PSK (and thus the key) is the
     * only safe recovery. At 50 fps + 1024-window this is about
     * 1300 years of continuous TX. */
    if (window > UINT32_MAX - w) {
        nvs_close(h);
        return ESP_ERR_INVALID_STATE;
    }

    err = nvs_set_u32(h, K_NONCE, w + window);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;

    *out_start = w;
    return ESP_OK;
}

esp_err_t nvs_cfg_set_nickname(const char *nickname)
{
    if (!nickname || strlen(nickname) > 16) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, K_NICK, nickname);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}
