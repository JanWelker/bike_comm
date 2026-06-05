# Mesh Protocol v0

Custom TDMA on top of ESP-NOW. Single-hop, flat broadcast, up to 8 riders.

## Topology

- **Single hop.** All riders within RF range of each other. Multi-hop relay is a v2 problem; for v1 the 100–300 m line-of-sight ESP-NOW reach is good enough for a riding group.
- **No master.** Lowest-MAC rider in the current group serves as *coordinator* (just to emit beacons for time sync). Failover is automatic.

## Time

- **Superframe** = 20 ms, divided into 8 slots × 2.5 ms.
- **Slot N** belongs to rider with `slot_index = N` (claimed at join time).
- **Each rider transmits once per superframe** in their own slot. PHY sleeps in other slots.

### Slot math

| | |
|---|---|
| LC3 frame F_n (24 kbps, 10 ms) | 30 B |
| LC3 frame F_{n-1} (the prior 10 ms) | 30 B |
| Header (rider, flags, seq, sframe_ctr, CRC) | 8 B |
| ESP-NOW + Wi-Fi overhead | ~22 B |
| **Total on air** | ~90 B |
| Airtime @ 1 Mbps PHY | ~0.7 ms |
| Slot duration | 2.5 ms |
| Guard window | ~1.5 ms |

Two LC3 frames per packet covers the mic's 100 fps encode rate at the
slot's 50 fps cadence — see "Two LC3 frames per packet" below.

Crystal drift over 20 ms at ±20 ppm = ±0.4 µs. Guard absorbs it with ~1000× margin.

## Wire format

68 bytes payload (within the 250 B ESP-NOW MTU).

```
offset  size  field
   0     1    rider_id        (0..7, source slot)
   1     1    flags           (VAD | JOIN | LEAVE | BEACON | LC3_PREV_VALID)
   2     2    seq             (per-rider monotonic; seq of lc3 — lc3_prev's
                               implicit seq is seq - 1)
   4     2    superframe_ctr  (coordinator-broadcast; receivers track delta)
   6    30    lc3             (current 10 ms voice, F_n)
  36    30    lc3_prev        (prior 10 ms voice, F_{n-1}; gated by the
                               LC3_PREV_VALID flag — cleared on first packet
                               from a rider and on BEACON frames where this
                               area carries beacon payload instead)
  66     2    crc16           (CRC-16/CCITT over bytes 0..65)
```

The CRC is a defense against firmware bugs, not adversaries — see the Security section for the encryption story (short version: v0 ships unencrypted, see below).

## Beacon

When `flags & BEACON`, the `lc3_prev` slot (offset 36..65) instead carries:

```
offset  size  field
  36     1    BEACON_MAGIC    (0xB1)
  37     4    coord_mac_low   (lowest 4 bytes of coordinator MAC)
  41     4    us_timestamp    (esp_timer_get_time() at TX start, modulo 2^32)
  45     1    slot_map        (bit i = 1 if slot i is claimed)
  46     1    group_version   (bumped on PSK change, schema change, etc.)
  47    19    reserved (zeros, for future fields)
```

`LC3_PREV_VALID` is always cleared on beacon frames (those 30 B are beacon, not audio). The `lc3` slot still carries one audio frame, so the coordinator transmits one mic frame on every beacon-bearing slot 0 instead of going silent.

Slot 0 alternates between beacon (even superframes) and audio (odd superframes). Beacon rate is 25 fps (one beacon every 40 ms); the coordinator-loss timer (10 superframes ≈ 200 ms) tolerates the wider gap. Coordinator audio rate is 75 fps (25 beacon-slot frames + 50 audio-slot frames per second) — see the asymmetry note below.

## Join

1. Listen for ≥ 2 superframes (≥ 40 ms) to receive a beacon.
2. Read `slot_map`; pick the lowest unset slot.
3. In that slot, transmit a frame with `JOIN` flag set.
4. Wait one superframe.
5. If the next beacon's `slot_map` shows our bit set, we're in. Otherwise (collision with another joiner), back off `hash(MAC) mod 4` superframes and retry.

## Leave

- Explicit: transmit one final frame with `LEAVE` flag set.
- Implicit: any rider whose slot is silent for 10 superframes (200 ms) is dropped from the slot map by the next beacon.

## Coordinator failover

- If no beacon is received for 5 superframes (100 ms), the next-lowest-MAC rider starts beaconing on the next slot 0.
- A new coordinator that hears a *lower-MAC* coordinator's beacon yields immediately. Prevents flapping.

## Two LC3 frames per packet

The mic encodes at 100 fps (10 ms LC3 frames) but each rider's slot
fires at 50 fps (one per 20 ms superframe). To cover the 2× mismatch,
each packet carries the **two most recent** LC3 frames:

- `lc3` is F_n, the latest 10 ms of audio captured before TX.
- `lc3_prev` is F_{n-1}, captured 10 ms earlier.

`f.seq` names F_n. F_{n-1}'s implicit seq is `f.seq - 1`. The sender's
seq counter increments by 2 per dual-frame packet (by 1 on a packet
with only `lc3` — first ever from a rider, or back-pressure shed the
older). Header-only packets (JOIN/LEAVE/BEACON without audio) bump seq
by 1.

The receiver pushes `lc3_prev` first (so the JB sees frames in seq
order), gated by the `LC3_PREV_VALID` flag and a duplicate check
against `last_seq` — if the prior packet's `lc3` already delivered
that seq, the slot is skipped. Otherwise both frames flow into the
per-rider seq-aware jitter buffer.

If a packet is lost, the next packet's `lc3_prev` slot recovers the
older of the two LC3 frames it carried, so a single mesh packet drop
loses at most 10 ms of audio (where the prior design lost a full
20 ms). Two consecutive packet losses still lose audio — PLC fills.

The previous XOR-parity FEC slot bought partial recovery of F_{n-1}
from a single packet loss; carrying F_{n-1} directly is strictly
better at the same 30 B cost.

Asymmetry note: the coordinator's slot 0 alternates beacon and audio
(see "Beacon"). Audio slots carry 2 frames; beacon slots carry 1
(the other 30 B is beacon payload). Net coordinator audio rate is
~75 fps over the wire vs ~100 fps for joiners — the missing 25 fps
is the one mic frame per beacon slot that doesn't fit. The TX ring
evicts oldest under that sustained back-pressure, so the lost frame
is distributed evenly. Closing the gap fully needs a dedicated 9th
beacon slot — v0.5 work.

## No retransmission

Voice frames are loss-tolerant; late frames are useless. Retransmission would blow the latency budget. PLC and XOR-FEC handle the gaps.

## Security

- **v0 ships unencrypted on the wire.** ESP-NOW's built-in AES-128-CCM only applies to *unicast* peers with `encrypt = true`; broadcast traffic is plaintext. Our single-hop flat broadcast topology therefore goes out in the clear at v0. The PSK is still installed via `esp_now_set_pmk` so we don't have to re-architect when we add encryption.
- **Path to encrypted traffic** (post-v0, before any public/field deployment): either
  - (a) layer AES-128-CCM over the 64 B payload at the app layer using the group PSK as the key and `rider_id || seq || superframe_ctr` as the nonce, costing ~16 B of MIC tag (fits comfortably in the 250 B ESP-NOW MTU), or
  - (b) move from broadcast to N unicast peers with ESP-NOW's native CCM (each rider adds the other 7 as encrypted peers; PSK reuse is fine because ESP-NOW derives a per-peer LMK).
  Option (a) is simpler and preserves the broadcast topology; (b) is what the original spec assumed.
- PSK is generated by `tools/psk_gen.py` and exchanged via QR code (or flashed via serial).
- Anti-replay: receivers drop any frame with `seq` older than 2 superframes from the most-recent seen for that rider. This works against any attacker who can record-and-replay our traffic (and is the only adversarial defense in v0).

## Open issues (revisit during build)

1. **Beacon piggyback eating slot 0 audio** — the 30 B beacon fits in lc3_prev, so beacon slots still carry one audio frame in lc3, bringing the coordinator to 75 fps (vs joiners' 100 fps). The remaining 25 fps gap needs either a dedicated 9th beacon slot (shrinks each audio slot to ~2.22 ms — math still works) or a 4-LC3-frame bundle on coordinator audio slots (wider on-air frame, still within MTU). Pick when the asymmetry becomes audibly meaningful.
2. **Collision detection during JOIN** — we infer collision from "next beacon didn't add our bit." A more robust scheme listens to other riders' RSSI of our own JOIN — TBD on bench.
3. **Multi-hop** — explicitly deferred to v2.
