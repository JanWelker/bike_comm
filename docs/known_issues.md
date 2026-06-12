# Known issues and deferred findings

Verified-but-deferred findings from the 2026-06-12 full-codebase
review. The review's CONFIRMED findings (20 of 25 candidates) were
fixed in the same pass — mesh failover/join/replay/phase-lock,
mixer rider lifecycle, VAD-gated encode, session_fsm wiring, and the
stale-comment/dead-API cleanup. What remains below are the three
PLAUSIBLE findings (real mechanism, not currently biting or already
mitigated) and the one REFUTED finding kept on file because it goes
live when the BT phone link lands.

## Plausible (deferred)

### 1. Torn int64 read of the mesh clock offset — fixed incidentally

`s_mesh_clock_offset_us` is int64 and was read/written from two
contexts with no protection; on the 32-bit LX6 that is two loads, so
a preemption between halves could yield a value off by 2^32 us
(~71 min) and feed a wild sleep into the slot scheduler. The
2026-06-12 pass put all shared mesh state, including the clock
offset, under the `s_mac_mux` spinlock — this finding is closed as a
side effect. Listed here so nobody "optimizes away" the critical
section in `mesh_now_us()` without knowing what it prevents.

### 2. Busy-wait slot alignment in mesh_tx_task

The TX task aligns to its slot with `vTaskDelay` (1 ms granularity)
plus an `esp_rom_delay_us` busy-wait for the remainder — worst case
~1.5 ms of spin, twice per 20 ms superframe, on core 0 at prio 19.
Interrupts stay enabled and the wifi task (prio 23) preempts the
spin freely, so this wastes CPU rather than starving wifi; the cost
lands on core-0 tasks at prio <= 19. Already marked TODO(v0.5) in
the code: replace with an esp_timer one-shot + task notification.
Worth doing before the BT Classic link adds Bluedroid load to
core 0.

### 3. codec_lc3_decode failure leaves the output buffer unwritten

`codec_lc3_decode` returns 0 without writing `out_pcm` on its guard
failures and on `lc3_decode` rc < 0, and `mixer_pull` ignores the
return value — a failed decode would duck-and-sum an uninitialized
stack buffer into the speaker output. Unreachable today: all 8
decoders are pre-allocated at init (ESP_ERROR_CHECKed), rider_id is
bounds-checked, and the payload length is always 40 B. It becomes
live if the decoder lifecycle ever changes (e.g. freeing decoders to
reclaim internal RAM). Cheap future-proofing when touching that
code: memset `out_pcm` to 0 on any failure path, or check the
return at the call sites.

## Refuted today, real later

### 4. mixer_pull holds the mixer mutex across all LC3 decodes

`mixer_pull` holds `s_mtx` across up to 8 LC3 decodes plus the
duck/sum loops — potentially several milliseconds. Refuted as a bug
today because no concurrent caller exists: push (via
`mesh_rx_drain_to_mixer`) and pull both run on the audio_io task,
and `mixer_push_phone_pcm` has no callers yet. The moment HFP/A2DP
lands, the BT task will block behind that hold every 10 ms tick —
priority inversion on exactly the coexistence path the project is
about to bench. Fix shape when wiring the phone path: copy the JB
entries out under the lock (40 B each), decode and sum after
`xSemaphoreGive`.
