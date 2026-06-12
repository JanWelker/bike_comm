/*
 * nvs_cfg — persistent settings (NVS namespace "cfg").
 *
 * Stores:
 *   - paired_phone_addr   (esp_bd_addr_t)
 *   - mesh_group_psk      (16 bytes; installed via esp_now_set_pmk so the
 *                          post-v0 encrypted-mesh path slots in without a
 *                          re-architecture. v0 broadcast traffic is
 *                          plaintext — see docs/mesh_protocol.md.)
 *   - rider_nickname      (UTF-8, max 16 bytes)
 *   - gain_speaker, gain_mic, gain_mesh  (int8_t, dB offsets)
 *
 * On first boot (no PSK set), a CSPRNG-generated PSK is written
 * automatically so the device can join its own one-rider "group".
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_bt_defs.h"

esp_err_t nvs_cfg_init(void);

esp_err_t nvs_cfg_get_psk(uint8_t out_psk[16]);
esp_err_t nvs_cfg_set_psk(const uint8_t psk[16]);

/* Reserve a contiguous range of mesh-crypto nonce_lo values for this
 * boot.
 *
 * Reads the current watermark W from NVS (0 if missing), writes
 * W + window back, and returns W as *out_start. The caller hands out
 * values W..W+window-1 from RAM and must call again before exhausting
 * the window. NVS writes are therefore amortised to one per `window`
 * frames — at the default 1024 frames/window and 50 fps that's roughly
 * one NVS write per 20 s of TX, well inside NVS wear budget. The
 * watermark only ever advances, so (key, nonce) reuse across reboots
 * is impossible even if the in-RAM counter resets to 0 every boot. */
esp_err_t nvs_cfg_alloc_nonce_window(uint32_t window, uint32_t *out_start);

esp_err_t nvs_cfg_get_phone_addr(esp_bd_addr_t *out);
esp_err_t nvs_cfg_set_phone_addr(const esp_bd_addr_t *addr);
esp_err_t nvs_cfg_clear_phone_addr(void);

esp_err_t nvs_cfg_get_nickname(char out[17]);
esp_err_t nvs_cfg_set_nickname(const char *nickname);
