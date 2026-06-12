/*
 * mesh_proto — pure-C protocol logic. See mesh_proto.h.
 */

#include "mesh_proto.h"

/* ---- Anti-replay ------------------------------------------------------ */

bool mesh_proto_seq_accept(uint16_t last_seq, uint16_t new_seq)
{
    /* Treat the 16-bit space as circular: delta is the signed distance
     * from last_seq to new_seq. Accept iff strictly newer; any forward
     * jump is a resync (see header for why there is no upper window). */
    return (int16_t)(new_seq - last_seq) > 0;
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
