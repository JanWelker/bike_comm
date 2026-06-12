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
| LC3 frame F_n (32 kbps, 10 ms) | 40 B |
| LC3 frame F_{n-1} (the prior 10 ms) | 40 B |
| Header (rider, flags, seq, sframe_ctr) | 6 B |
| CCM nonce_lo + MIC | 20 B |
| ESP-NOW + Wi-Fi overhead | ~22 B |
| **Total on air** | ~128 B |
| Airtime @ 1 Mbps PHY | ~1.05 ms |
| Slot duration | 2.5 ms |
| Guard window | ~1.45 ms |

Two LC3 frames per packet covers the mic's 100 fps encode rate at the
slot's 50 fps cadence — see "Two LC3 frames per packet" below.

Crystal drift over 20 ms at ±20 ppm = ±0.4 µs. Guard absorbs it with ~1000× margin.

## Wire format

106 bytes on air (within the 250 B ESP-NOW MTU). The plaintext body is
86 B; the CCM nonce_lo prefix and MIC suffix add 20 B.

```
offset  size  field
   0     4    nonce_lo        (per-device monotonic counter, cleartext but
                               authenticated by the MIC as AAD)
   4    86    cipher          (AES-128-CCM ciphertext of the plaintext
                               body below)
  90    16    mic             (AES-128-CCM authentication tag)
```

The plaintext body that gets encrypted/decrypted (CCM in/out, 86 B):

```
offset  size  field
   0     1    rider_id        (0..7, source slot)
   1     1    flags           (VAD | JOIN | LEAVE | BEACON | LC3_PREV_VALID)
   2     2    seq             (per-rider monotonic; seq of lc3 — lc3_prev's
                               implicit seq is seq - 1)
   4     2    superframe_ctr  (coordinator-broadcast; receivers track delta)
   6    40    lc3             (current 10 ms voice, F_n)
  46    40    lc3_prev        (prior 10 ms voice, F_{n-1}; gated by the
                               LC3_PREV_VALID flag — cleared on first packet
                               from a rider and on BEACON frames where this
                               area carries beacon payload instead)
```

The CCM nonce is reconstructed by the receiver as
`src_mac (6) || 0x00 0x00 0x00 || nonce_lo (4)` = 13 B. Both inputs to
the nonce are available without further protocol exchange: `src_mac`
comes from the ESP-NOW recv info; `nonce_lo` rides in the frame as
AAD.

## Beacon

When `flags & BEACON`, the `lc3_prev` slot (offset 46..85) instead carries:

```
offset  size  field
  46     1    BEACON_MAGIC    (0xB1)
  47     4    coord_mac_low   (lowest 4 bytes of coordinator MAC)
  51     4    us_timestamp    (mesh time of this superframe's slot-0
                               boundary, modulo 2^32)
  55     1    slot_map        (bit i = 1 if slot i is claimed)
  56     1    group_version   (bumped on PSK change, schema change, etc.)
  57    24    slot_owner      (8 x 3 bytes: low 3 bytes of the owner MAC
                               per claimed slot, big-endian; 0 = free or
                               owner not yet learned)
  81     5    reserved (zeros, for future fields)
```

`LC3_PREV_VALID` is always cleared on beacon frames (those 40 B are beacon, not audio). The `lc3` slot still carries one audio frame, so the coordinator transmits one mic frame on every beacon-bearing slot 0 instead of going silent.

`us_timestamp` does double duty: receivers slew their mesh clock toward it (value sync) AND re-derive their superframe grid from it (phase sync) — the timestamp is by definition a slot-0 boundary, so each rider's slot grid stays phase-locked to the coordinator's instead of free-running at a power-on-random 0..20 ms offset.

`slot_owner` lets every rider verify its claim end-to-end: bit set + owner == us is the join ack; bit set + foreign owner means the slot was lost (join collision, handed away during an RF fade, or the rider is the yielding half of a dual-coordinator split) and triggers renegotiation; bit clear means the coordinator dropped us and we re-claim.

Slot 0 alternates between beacon (even superframes) and audio (odd superframes). Beacon rate is 25 fps (one beacon every 40 ms); the coordinator-loss timer (10 superframes ≈ 200 ms) tolerates the wider gap. Coordinator audio rate is 75 fps (25 beacon-slot frames + 50 audio-slot frames per second) — see the asymmetry note below.

## Join

1. Listen for ≥ 2 superframes (≥ 40 ms) to receive a beacon.
2. Read `slot_map`; pick the lowest unset slot and commit locally.
3. Transmit in that slot with the `JOIN` flag set — and keep setting it
   on every frame until a beacon acks the claim (`slot_map` bit set AND
   `slot_owner` == our MAC). A single lost JOIN frame therefore can't
   leave the claim dangling.
4. If a beacon shows our slot owned by someone else (concurrent joiner
   won the race), renegotiate: pick the lowest free slot from that
   beacon, claim it, keep the JOIN flag running. If no slot is free,
   give up and go idle.

## Leave

- Explicit: transmit one final frame with `LEAVE` flag set.
- Implicit: any rider whose slot is silent for 10 superframes (200 ms) is dropped from the slot map by the next beacon.
- Recovery: a rider that sees its own bit cleared in a beacon (RF fade
  outlived the quiet timeout) re-claims the same slot via the JOIN
  mechanism; a rider that sees its slot owned by another MAC
  renegotiates a fresh slot. Receivers also clear per-rider state for
  any slot a beacon reports as released, so a re-claimed slot starts
  from a clean seq history.

## Heartbeat (VAD-silent TX skip)

VAD already gates the encoder so silent ticks don't burn LC3 cost, but
without further gating the TX task still emits a header-only frame
every superframe just to refresh peers' implicit-leave quiet counter.
The heartbeat scheme drops those silent-slot transmissions on the
floor and only forces a header-only TX every `K = 5` superframes
(= 100 ms) as a slot-claim keep-alive.

- `K` must be strictly less than `MESH_PEER_QUIET_SFRAMES` (10) so a
  single lost heartbeat still has four superframes of headroom before
  the receiver tears the peer down.
- Coordinators never reach the heartbeat path: their beacon every
  other superframe (40 ms cadence) already acts as keep-alive, so the
  audio slots can be skipped outright during silence. Net coordinator
  airtime during silence is one beacon every 40 ms instead of two
  frames every 20 ms.
- Signalling (JOIN/LEAVE/BEACON) and audio-bearing frames always
  send, regardless of where we are in the heartbeat cycle.
- `s_tx_seq` only advances when a frame actually radiates, so a
  K-superframe gap doesn't burn through the 16-bit seq space.
- Anti-replay accepts any strictly-forward seq delta, so the first
  packet after a heartbeat gap re-syncs the receiver in one shot.

A prior implementation of this scheme was reverted in 2026-06 because
it interacted badly with three latent bugs (seq advancing on un-sent
slots, a bounded 16-seq forward replay window = 8 superframes, and
one-shot JOIN). All three were fixed in the 2026-06-12 review, which
is what unblocks the heartbeat re-attempt.

## Coordinator failover

- If no beacon is received for 10 superframes (200 ms — matches the
  coordinator-loss timer; the beacon-bearing slot only fires every
  other superframe) AND the rider is next-lowest-MAC, it takes the
  coordinator role: it releases its old slot, force-releases and claims
  slot 0, and starts beaconing on the next even superframe. The role is
  welded to slot 0 so the beacon schedule never needs a second wake-up
  inside the superframe.
- A coordinator that hears a *lower-MAC* coordinator's beacon yields
  immediately: it adopts that beacon's map and clock and renegotiates a
  fresh slot for itself (its slot 0 now belongs to the winner). A
  coordinator ignores beacons from *higher-MAC* coordinators — exactly
  one side backs down. Prevents flapping and resolves simultaneous
  bootstrap (two riders powered on inside the same listen window).

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
by 1, and only when the frame actually goes on air — a TX gap doesn't
burn through the seq space.

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
better at the same 40 B cost.

Asymmetry note: the coordinator's slot 0 alternates beacon and audio
(see "Beacon"). Audio slots carry 2 frames; beacon slots carry 1
(the other 40 B is beacon payload). Net coordinator audio rate is
~75 fps over the wire vs ~100 fps for joiners — the missing 25 fps
is the one mic frame per beacon slot that doesn't fit. The TX ring
evicts oldest under that sustained back-pressure, so the lost frame
is distributed evenly. Closing the gap fully needs a dedicated 9th
beacon slot — v0.5 work.

## No retransmission

Voice frames are loss-tolerant; late frames are useless. Retransmission would blow the latency budget. PLC and XOR-FEC handle the gaps.

## Security

- **App-layer AES-128-CCM over every frame.** The 16 B group PSK is the
  CCM key; the nonce is `src_mac (6) || 0x00 0x00 0x00 || nonce_lo (4)`
  (13 B, max CCM nonce length). The MIC is 16 B and covers the 4 B
  `nonce_lo` (as AAD) plus the 86 B encrypted body. Both the nonce
  inputs and the AAD are reconstructable at the receiver without any
  prior key exchange beyond the PSK. ESP-NOW's built-in CCM is
  bypassed (broadcast peers can't use it); the PSK is still installed
  via `esp_now_set_pmk` as defence in depth if the broadcast topology
  is ever swapped for N unicast peers.
- **Nonce uniqueness across reboots.** `nonce_lo` is a 32-bit per-device
  monotonic counter, watermarked in NVS with skip-ahead (1024-value
  windows). Boot reads the current watermark W, immediately writes
  W+1024 back, then hands out values W..W+1023 from RAM. The next
  refill writes W+2048 and hands out W+1024..W+2047, etc. A crashed or
  power-loss-reset device picks up at the last persisted watermark, so
  the (key, nonce) pair is never reused even if the in-RAM counter
  starts again at 0 every boot. At 50 fps that's one NVS write per
  ~20 s of TX. The 32-bit counter does not wrap inside ~1300 years of
  continuous transmission; on the (extremely unlikely) overflow, the
  refill fails and the only safe recovery is rotating the PSK.
- **MAC binding per slot.** The first frame accepted on slot R pins
  the sender's L2 MAC; every subsequent frame on R must match the
  pinned MAC. Combined with the MIC check this means a PSK-holding
  insider cannot impersonate another rider's slot without also MAC-
  spoofing at the Wi-Fi layer. The pinning is cleared on slot release
  (explicit LEAVE, beacon-released slot, implicit-leave quiet
  timeout, or our own coordinator-failover takeover), so a re-join
  by a different physical device re-uses the same slot cleanly.
- **Anti-replay.** Receivers drop any frame whose `seq` is not strictly
  newer (16-bit wrap-aware) than the most-recent seen for that rider.
  Forward jumps of any size are accepted as a resync — a bounded
  forward window is a liveness trap: a TX gap longer than the window
  makes every subsequent frame land outside it too, deafening the
  receiver until the quiet-timeout tears the peer down. In addition,
  the receiver tracks the highest `nonce_lo` seen per slot and rejects
  any frame whose `nonce_lo` does not strictly advance. This second
  watermark survives a seq reset (LEAVE, beacon-released slot, quiet
  timeout): even if `s_seq_seen[R]` is cleared, a replayed pre-reset
  frame still carries a `nonce_lo` ≤ the last value we saw and is
  rejected before any state mutates.
- **Beacon source check.** A BEACON frame must arrive on a src_mac
  whose low 32 bits match the embedded `coord_mac_low`, so a PSK-
  holder cannot beacon as a rider other than themselves.
- **Threat model boundary.** The group PSK is a shared secret. Any
  device that holds it is a group member and can transmit valid
  frames as itself; it cannot impersonate another member without also
  MAC-spoofing. Per-device asymmetric keys would close that final gap
  and are a v1 candidate.
- PSK is generated by `tools/psk_gen.py` and exchanged via QR code (or
  flashed via serial).
- Mismatched `group_version` beacons are ignored (first heard beacon
  fixes the version), so two incompatible builds or two groups with
  different PSK epochs don't silently merge.

## Open issues (revisit during build)

1. **Beacon piggyback eating slot 0 audio** — the 40 B beacon fits in lc3_prev, so beacon slots still carry one audio frame in lc3, bringing the coordinator to 75 fps (vs joiners' 100 fps). The remaining 25 fps gap needs either a dedicated 9th beacon slot (shrinks each audio slot to ~2.22 ms — math still works) or a 4-LC3-frame bundle on coordinator audio slots (wider on-air frame, still within MTU). Pick when the asymmetry becomes audibly meaningful.
2. **Collision detection during JOIN** — we infer collision from "next beacon didn't add our bit." A more robust scheme listens to other riders' RSSI of our own JOIN — TBD on bench.
3. **Multi-hop** — explicitly deferred to v2.
