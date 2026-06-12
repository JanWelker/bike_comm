/*
 * mesh_crypto — AES-128-CCM wrapper. See mesh_crypto.h.
 */

#include "mesh_crypto.h"

#include <string.h>
#include "esp_log.h"
#include "mbedtls/ccm.h"

static const char *TAG = "mesh_crypto";

/* Separate contexts so the wifi-task recv path and the mesh_tx_task
 * never touch the same mbedtls state — no mutex needed. */
static mbedtls_ccm_context s_ccm_enc;
static mbedtls_ccm_context s_ccm_dec;
static bool                s_inited = false;

#define MESH_CRYPTO_NONCE_LEN 13   /* mbedtls accepts 7..13; we use the max */

static void build_nonce(uint8_t out[MESH_CRYPTO_NONCE_LEN],
                        const uint8_t src_mac[6], uint32_t nonce_lo)
{
    /* src_mac (6) || 0 0 0 || nonce_lo little-endian (4). The 3 zero
     * bytes are reserved for a future per-session epoch if we ever add
     * one; for now they are just padding to reach the 13 B max nonce
     * length CCM allows. */
    memcpy(&out[0], src_mac, 6);
    out[6] = out[7] = out[8] = 0x00;
    out[9]  = (uint8_t)(nonce_lo);
    out[10] = (uint8_t)(nonce_lo >> 8);
    out[11] = (uint8_t)(nonce_lo >> 16);
    out[12] = (uint8_t)(nonce_lo >> 24);
}

esp_err_t mesh_crypto_init(const uint8_t psk[16])
{
    if (s_inited) {
        mbedtls_ccm_free(&s_ccm_enc);
        mbedtls_ccm_free(&s_ccm_dec);
        s_inited = false;
    }
    mbedtls_ccm_init(&s_ccm_enc);
    mbedtls_ccm_init(&s_ccm_dec);

    int rc = mbedtls_ccm_setkey(&s_ccm_enc, MBEDTLS_CIPHER_ID_AES, psk, 128);
    if (rc != 0) {
        ESP_LOGE(TAG, "ccm_setkey enc: -0x%04x", -rc);
        mbedtls_ccm_free(&s_ccm_enc);
        mbedtls_ccm_free(&s_ccm_dec);
        return ESP_FAIL;
    }
    rc = mbedtls_ccm_setkey(&s_ccm_dec, MBEDTLS_CIPHER_ID_AES, psk, 128);
    if (rc != 0) {
        ESP_LOGE(TAG, "ccm_setkey dec: -0x%04x", -rc);
        mbedtls_ccm_free(&s_ccm_enc);
        mbedtls_ccm_free(&s_ccm_dec);
        return ESP_FAIL;
    }
    s_inited = true;
    return ESP_OK;
}

esp_err_t mesh_crypto_encrypt(const uint8_t src_mac[6],
                              uint32_t nonce_lo,
                              const mesh_frame_t *pt,
                              mesh_wire_t *wire)
{
    if (!s_inited || !src_mac || !pt || !wire) return ESP_ERR_INVALID_STATE;

    uint8_t nonce[MESH_CRYPTO_NONCE_LEN];
    build_nonce(nonce, src_mac, nonce_lo);

    wire->nonce_lo = nonce_lo;

    int rc = mbedtls_ccm_encrypt_and_tag(
        &s_ccm_enc,
        MESH_PROTO_BODY_BYTES,
        nonce, MESH_CRYPTO_NONCE_LEN,
        (const uint8_t *)&wire->nonce_lo, sizeof(wire->nonce_lo),  /* AAD */
        (const uint8_t *)pt,
        wire->cipher,
        wire->mic, MESH_PROTO_MIC_BYTES);
    if (rc != 0) {
        ESP_LOGE(TAG, "ccm_encrypt: -0x%04x", -rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t mesh_crypto_decrypt(const uint8_t src_mac[6],
                              const mesh_wire_t *wire,
                              mesh_frame_t *pt)
{
    if (!s_inited || !src_mac || !wire || !pt) return ESP_ERR_INVALID_STATE;

    uint8_t nonce[MESH_CRYPTO_NONCE_LEN];
    build_nonce(nonce, src_mac, wire->nonce_lo);

    int rc = mbedtls_ccm_auth_decrypt(
        &s_ccm_dec,
        MESH_PROTO_BODY_BYTES,
        nonce, MESH_CRYPTO_NONCE_LEN,
        (const uint8_t *)&wire->nonce_lo, sizeof(wire->nonce_lo),  /* AAD */
        wire->cipher,
        (uint8_t *)pt,
        wire->mic, MESH_PROTO_MIC_BYTES);
    if (rc != 0) {
        /* MBEDTLS_ERR_CCM_AUTH_FAILED == -0x000F. Don't log per-frame:
         * a passive attacker could trivially generate the log rate. */
        return ESP_ERR_INVALID_MAC;
    }
    return ESP_OK;
}
