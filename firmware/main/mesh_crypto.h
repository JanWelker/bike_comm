/*
 * mesh_crypto — AES-128-CCM wrapper for the mesh wire format.
 *
 * Keyed by the 16 B group PSK (installed once at boot from NVS). Nonce
 * is constructed per call from the sender's MAC and a per-device
 * monotonic counter:
 *
 *      nonce[13] = src_mac[6] || 0x00 0x00 0x00 || nonce_lo[4]
 *
 * The MIC covers the 4 B nonce_lo (as AAD) plus the encrypted body, so
 * a tampered nonce or a swapped header fails verify.
 *
 * Two independent mbedtls_ccm contexts are held: one for encrypt (only
 * touched by mesh_tx_task) and one for decrypt (only touched by the
 * ESP-NOW recv callback in the wifi task). No locks; the contexts never
 * cross task boundaries. Both share the same PSK so the symmetry is
 * just a thread-safety convenience.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "mesh_proto.h"

/* Initialise both CCM contexts with the group PSK. Safe to call once
 * at boot; calling twice will reset the key. */
esp_err_t mesh_crypto_init(const uint8_t psk[16]);

/* Encrypt-and-tag a mesh_frame_t into a wire packet.
 *
 * Inputs:
 *   src_mac  — our own 6 B MAC, used to derive the CCM nonce
 *   nonce_lo — per-device monotonic counter (NVS-backed, never reused)
 *   pt       — plaintext frame body
 * Output:
 *   wire     — fully serialised on-air packet (nonce_lo + cipher + MIC)
 *
 * The TX task picks nonce_lo from the NVS-backed counter (see
 * nvs_cfg_alloc_nonce_window / mesh_mac.c). Reuse of (key, nonce) is
 * catastrophic, so any path that picks the nonce must never replay a
 * value already used by this device with this key.
 */
esp_err_t mesh_crypto_encrypt(const uint8_t src_mac[6],
                              uint32_t nonce_lo,
                              const mesh_frame_t *pt,
                              mesh_wire_t *wire);

/* Decrypt-and-verify a wire packet. src_mac comes from the ESP-NOW recv
 * info; the nonce is reconstructed from src_mac + wire->nonce_lo.
 *
 * Returns ESP_OK on success and writes the plaintext body to *pt.
 * Returns ESP_ERR_INVALID_MAC if the MIC fails (forgery, replay across
 * the nonce, or simple bit-flip). No partial-decrypt output is written
 * on failure.
 *
 * Safe to call from the wifi task: no allocation, no blocking. */
esp_err_t mesh_crypto_decrypt(const uint8_t src_mac[6],
                              const mesh_wire_t *wire,
                              mesh_frame_t *pt);
