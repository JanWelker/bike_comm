/*
 * mesh_proto — pure-C protocol logic. See mesh_proto.h.
 */

#include "mesh_proto.h"

/* ---- CRC-16/CCITT-FALSE -----------------------------------------------
 *
 * Bitwise implementation. The mesh covers 66 bytes per frame at 50 Hz,
 * so we're computing ~3.3 KB/s — even on the LX6 the bitwise loop is
 * negligible. If we ever push voice rates up we'll swap in a 256-entry
 * table.
 */
uint16_t mesh_proto_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; ++b) {
            if (crc & 0x8000) {
                crc = (uint16_t)((crc << 1) ^ 0x1021);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

/* ---- Anti-replay ------------------------------------------------------ */

bool mesh_proto_seq_accept(uint16_t last_seq,
                           uint16_t new_seq,
                           uint16_t window)
{
    /* Treat the 16-bit space as circular: delta is the signed distance
     * from last_seq to new_seq. Accept iff strictly newer AND within
     * window. */
    int16_t delta = (int16_t)(new_seq - last_seq);
    return delta > 0 && delta <= (int16_t)window;
}

/* ---- Slot map --------------------------------------------------------- */

void mesh_proto_slot_claim(uint8_t *map, uint8_t slot)
{
    if (slot >= MESH_PROTO_MAX_RIDERS || map == NULL) return;
    *map |= (uint8_t)(1u << slot);
}

void mesh_proto_slot_release(uint8_t *map, uint8_t slot)
{
    if (slot >= MESH_PROTO_MAX_RIDERS || map == NULL) return;
    *map &= (uint8_t)~(1u << slot);
}

int mesh_proto_lowest_free_slot(uint8_t map)
{
    for (int i = 0; i < MESH_PROTO_MAX_RIDERS; ++i) {
        if ((map & (1u << i)) == 0) return i;
    }
    return -1;
}

/* ---- Coordinator election -------------------------------------------- */

bool mesh_proto_we_are_coordinator(uint32_t our_mac_low,
                                   const uint32_t *peer_mac_lows,
                                   size_t peer_count)
{
    for (size_t i = 0; i < peer_count; ++i) {
        if (peer_mac_lows[i] < our_mac_low) return false;
    }
    return true;
}
