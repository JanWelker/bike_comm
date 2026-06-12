/*
 * codec2_alloc — PSRAM-backed allocator for vendored codec2 state.
 *
 * Why this exists: codec2_create(3200) does many small mallocs, each
 * well under CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (16 KB), so the IDF
 * default malloc keeps them all in internal DRAM. Even one encoder
 * plus two decoders drained enough internal heap that
 * mbedtls_ccm_setkey then returned ALLOC_FAILED during
 * mesh_crypto_init. Routing every codec2 allocation to PSRAM frees
 * the internal heap for Wi-Fi, BT, mbedtls, audio DMA buffers, and
 * the codec_lc3 path (which actually needs internal RAM for its hot
 * loop on the LX6).
 *
 * Tradeoff: PSRAM reads are 5-10x slower than internal SRAM on the
 * original ESP32. codec2 mode 3200 is computationally light enough
 * that the 20 ms budget probably still holds, but the bench is the
 * arbiter — codec_perf_log_and_reset reports the wall-clock cost so
 * the partition between PSRAM-resident and internal-resident state
 * can be retuned if needed.
 *
 * Wired into codec2 via:
 *   - target_compile_definitions(__EMBEDDED__) on the component,
 *     which makes debug_alloc.h declare extern codec2_malloc/calloc/
 *     free for the MALLOC/CALLOC/FREE macros it exposes.
 *   - Vendored kiss_fft.h patched to call codec2_malloc/free
 *     (KISS_FFT_MALLOC/FREE).
 *   - Vendored mbest.c and nlp.c patched to use MALLOC/FREE instead
 *     of raw malloc/free.
 */

#include <stddef.h>
#include <string.h>
#include "esp_heap_caps.h"

#define CODEC2_HEAP_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void *codec2_malloc(size_t size)
{
    return heap_caps_malloc(size, CODEC2_HEAP_CAPS);
}

void *codec2_calloc(size_t nmemb, size_t size)
{
    /* heap_caps_calloc is the IDF analogue; it zero-fills like libc
     * calloc. Overflow-safe vs naive size*nmemb: the IDF helper
     * checks the multiplication. */
    return heap_caps_calloc(nmemb, size, CODEC2_HEAP_CAPS);
}

void codec2_free(void *ptr)
{
    heap_caps_free(ptr);
}
