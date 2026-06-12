# Working notes for Claude

Operational and stylistic conventions for this repo. The canonical
design lives in `docs/`; this file is about how to collaborate on the
code without re-relearning the same lessons every session.

## What this is

An open-source motorcycle helmet intercom on the original ESP32 (LX6,
WROVER-E). Single radio, single chip, ESP-IDF v5.5.4. Two-board mesh
audio over ESP-NOW already works on the bench; phone link (HFP-HF +
A2DP-sink) is not yet wired. See `README.md` for status and
`docs/architecture.md` for the layered view.

## Build and flash

```sh
. ~/esp/esp-idf/export.sh
cd firmware/
idf.py set-target esp32      # one-time
idf.py build
idf.py -p /dev/cu.usbserial-XXXX flash monitor
```

Two boards are flashed sequentially from one machine. The TTY device
ID differs per cable / per board — list with `ls /dev/cu.usbserial-*`.

The IDE shows many false-positive diagnostics from clangd — mostly
Xtensa-specific GCC flags clang doesn't recognize, plus newlib headers
it can't find. The `.clangd` config at the repo root silences the bulk.
The source of truth for warnings is `idf.py build` — ignore anything
that doesn't come from there.

Host tests live under `firmware/test/<module>/` — pure-C builds against
the platform-independent code (`mesh_proto`, `mixer_jb`, vendored
liblc3) with plain `cc`, run with `make && ./test_*` in each subdir.
Run them whenever you touch the wire format or JB logic; the
mesh_proto layout-size assertion has caught silent rot before.

## Repo map (where to look)

| Path | What's there |
|---|---|
| `firmware/main/main.c` | Init order, `on_mesh_rx` queue bridge, `mesh_rx_drain_to_mixer` |
| `firmware/main/audio_pipeline.c` | LyraT-Mini v1.2 pin map, I2S setup, audio_io task (mic -> LC3 -> mesh + mixer pull -> spk) |
| `firmware/main/mesh_mac.c` | TDMA scheduler, beacon, join, coord failover |
| `firmware/main/codec_lc3.c` | liblc3 wrapper, pre-allocated encoder + 8 decoders |
| `firmware/main/mixer.c`, `mixer_jb.c` | Per-rider jitter buffer + summing mixer |
| `firmware/main/coex.c` | Wi-Fi + BT coexistence tuning (single-radio risk lives here) |
| `firmware/components/liblc3/upstream/` | Vendored google/liblc3 v1.1.1 |
| `docs/architecture.md` | Block diagram, layer map, CPU budget |
| `docs/mesh_protocol.md` | Wire format, slot math, beacon, failover |
| `docs/build_v0.md` | Bench bring-up, gotchas, definition-of-done |
| `docs/codec_perf.md` | Measured LC3 encode/decode cost on LX6 (LyraT-Mini bench) — the only LX6 codec numbers we have, since esp_audio_codec only publishes S3R8 |
| `docs/known_issues.md` | Verified-but-deferred review findings + the trigger conditions that make them live (check before touching the recv path, the slot scheduler, or wiring the BT phone path) |
| `tools/psk_gen.py` | Generate the 16 B group PSK |

## Hardware quirks worth knowing up front

- **LyraT-Mini v1.2 is two codecs, not one.** ES8311 does playback only;
  ES7243 does mic capture only. They share an I2C bus, two separate I2S
  ports. The full pin map is in the comment block at the top of
  `audio_pipeline.c`.
- **Onboard mic is on ES7243 right channel.** The ES8311's mic pins are
  dead (caps `(NC)` on v1.2). I2S1 reads with `SLOT_MODE_STEREO` and
  `SLOT_RIGHT` mask; capture buffer is stereo, right sample carries the
  signal.
- **Speaker PA enable is GPIO21.** Drive it HIGH or the headphone jack
  is silent even though I2S is happily clocking.
- **Six tactile buttons share GPIO39 as an ADC resistor ladder** (not
  individual GPIOs). Per the v1.2 schematic, idle is ~3.15 V and the
  per-key press voltages are VOL+ 0.38, VOL- 0.82, SET 1.18, PLAY 1.57,
  MODE 1.98, REC 2.41 V. Decoded in `ui.c` with ~40 ms debounce. Only
  VOL+ / VOL- emit events today (5 % step via
  `audio_pipeline_vol_step`); the other four are recognized and logged
  but ignored until `session_fsm_on_button` grows handlers.

## Critical gotchas (don't relearn these)

- **`audio_io` task stack must be 7 KB.** 4 KB overflows on
  LC3 encode + cross-core mesh mutex (symptom: `Guru Meditation
  IllegalInstruction` with a wild PC on core 0, not core 1). 8 KB
  starves `mesh_mac_start` of internal RAM. 7168 sits right on the
  cliff — any new BSS or large managed component that grabs DRAM
  tips us over.
- **The ESP-NOW recv callback cannot block or allocate.** It runs in
  the wifi task. `heap_caps_malloc` inside the callback (even
  transitively, via lazy codec decoder acquisition) stalls Wi-Fi and
  drops beacons -> coordinator-lost storms on peers. Pre-allocate
  everything at init; the callback only `memcpy`s into a small struct
  and `xQueueSend`s with timeout 0.
- **Coordinator slot 0 alternates beacon and audio.** Beacon (30 B)
  sits in the `lc3_prev` half of slot 0 on even superframes; `lc3`
  still carries one audio frame on those turns. Net rx-drain rates:
  ~100 fps joiner -> coord, ~75 fps coord -> joiner. Closing the
  residual gap needs a dedicated 9th beacon slot (v0.5). Coord-loss
  timer is 10 superframes (200 ms) to tolerate the 40 ms inter-beacon
  gap — don't tighten without revisiting beacon cadence.
- **Heartbeat: silent slots skip TX, claim refreshed every
  `MESH_HEARTBEAT_SFRAMES` (5) superframes.** Joiners only radiate
  when carrying audio, when JOIN/LEAVE/BEACON is pending, or once per
  100 ms as a keep-alive. K must stay strictly under
  `MESH_PEER_QUIET_SFRAMES` (10) so a single lost heartbeat doesn't
  collapse the peer view. Three latent bugs (seq advancing on
  un-sent, bounded forward replay window, one-shot JOIN) made the
  first attempt at this break asymmetrically; all three are fixed
  in code, so don't reintroduce them when tuning K. The
  `tx 10s: audio=… hb=… skip=…` log line on each rider tells you
  whether silence is actually saving airtime.
- **LC3 codec state must live in internal RAM.** `heap_caps_malloc`
  with `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`. PSRAM access from the
  hot encode/decode loop craters throughput on the original ESP32.
- **ESP-SR AFE doesn't work on the LX6 — don't re-add it.** esp-sr
  2.4.6's precompiled `ns_process` for ESP32 calls
  `heap_caps_check_integrity_all` and crashes walking our heap; older
  2.0.5 doesn't have the check but exhausts internal DRAM and OOMs
  the audio task. We use `cpuimage/WebRTC_NS` instead (vendored at
  `firmware/components/webrtc_ns/`) — BSD-3, single-TU float32 C,
  10 ms / 160-sample blocks that map 1:1 to our LC3 cadence. State
  is ~25 KB and lands in PSRAM via the default 16 KB threshold;
  measured cost on LX6 is ~2.6 ms per 10 ms frame (Analyze every
  2nd frame + Process every frame — Analyze is REQUIRED or the VAD
  probability stays pinned at 0.5; see `docs/codec_perf.md`).
- **The app partition is ~95% full.** Each OTA slot is 0x1E0000
  (1.9375 MB, bumped from 0x1D0000 to leave headroom for the
  codec2 build-time alternate); the LC3 binary fills it to ~5%
  free. The codec2 stub build sits at ~11% free because liblc3
  falls out via `--gc-sections`. Adding any further managed
  component over a few tens of KB will start to crowd the LC3
  build. The `storage` spiffs partition is now 124 KB (was 252 KB
  before the codec2 resize, 380 KB pre-WebRTC_NS) — further shrink
  possible if a much bigger component lands.
- **Wi-Fi must stay in `WIFI_MODE_STA` once BT Classic is on.**
  Espressif's coexist.html marks ESP-NOW RX as `S` (stable in STA
  mode only) under all BR/EDR coexistence states; APSTA/AP modes
  will degrade ESP-NOW RX (audio + beacons). `main.c::platform_init`
  already sets STA and asserts it — don't relax that without
  revisiting the whole coex story.
- **esp_audio_codec performance tables are S3R8-only.** Espressif's
  benchmarks (LC3, Opus, AAC, etc.) are measured on ESP32-S3R8 with
  LX7 + PIE vector ops. Numbers cannot be transferred 1:1 to our
  LX6. For LX6 planning, use the measurements in `docs/codec_perf.md`
  (our own LyraT-Mini bench) or re-measure.

## Code conventions

- C, ESP-IDF style, 4-space indent, brace-on-same-line for control
  flow. No tabs.
- Module pattern: `foo.h` declares the public API; `foo.c` keeps state
  in file-scope `s_*` statics; init is `foo_init()` returning
  `esp_err_t`, start is `foo_start()` if there's a task to spawn.
- Tasks: declare priority + core pin + stack size at the spawn site, in
  a comment, with a one-line rationale if non-obvious. Audio on core 1,
  radio and control on core 0.
- Logs: `ESP_LOGI/W/E` with a module-local `TAG`. Keep log lines short
  enough to render in 80 columns when prefixed.
- Comments explain *why*, not *what*. Mass-write only at the top of a
  file when the wiring or constraints are surprising (see
  `audio_pipeline.c` for the model).
- No emojis in source, docs, or commits. Plain text only.
- No "Sena" or "Cardo" mentions anywhere in the repo. The plan agent's
  scratch file uses them for context; that file lives outside the repo.

## Commits and pushes

- Author identity: Jan Welker <jan@wlkr.ch>.
- **Do not commit without an explicit ask.** Even if a change is
  obviously ready, leave it staged or unstaged and wait. The user reads
  every diff before it lands.
- **Do not push without an explicit ask.** Likewise force-push,
  reset --hard, branch delete, or anything destructive — confirm first.
- Commit message style: plain text, terse subject (under ~70 chars),
  optional body paragraph for the *why*. No emojis. No
  `Co-Authored-By: Claude ...` trailer. No "Generated with Claude
  Code" line. See `git log` for the in-repo style.
- Never use `git rebase -i`, `git add -i`, or any `-i` interactive
  flag — they hang the non-interactive shell.

## Working with mesh-audio changes

- The mesh path is touchy. Always validate with a two-board soak
  (`idf.py monitor` on both, grep for `coordinator lost`, `peer left`,
  any panic). Sixty seconds of clean log is the bar for "didn't break
  it." Per-rider rx-drain rates should be ~100 fps (joiner -> coord)
  and ~75 fps (coord -> joiner).
- A symmetric counts mismatch is the design; an asymmetric *drop* to
  zero is a regression.
- When tweaking timing constants (slot length, beacon cadence,
  coord-loss threshold), update both the code and
  `docs/mesh_protocol.md` in the same commit.

## Open work (v0.5 roadmap)

Concrete updates surfaced by the 2026-06 deep-research pass over the
OSS landscape. Listed in priority order; tick the box when landed.
Each item names the source / proven OSS work to draw from. The full
research notes live in the workflow output under the session tasks
dir.

- [x] **Lock `WIFI_MODE_STA` at boot.** Done; assertion in
  `platform_init`. See gotcha above.
- [x] **Measure LC3 cost on LX6.** Done; see `docs/codec_perf.md`.
- [x] **Vendor `cpuimage/WebRTC_NS` as the NS module.** Done; lives
  at `firmware/components/webrtc_ns/`, wired into `loopback_task`
  between mic read and LC3 encode (mode = medium). Measured ~2.6 ms
  per 10 ms frame on LX6 (Analyze every 2nd frame + Process every
  frame); required a partition resize (OTA slots bumped 1.75 →
  1.875 MB) to fit. The speech-period audio_io tick sits at ~8.5 ms
  of 10 ms at 2 riders (quiet ticks ~4 ms thanks to the VAD encode
  skip) — further DSP additions must live on the other core. See
  `docs/codec_perf.md`.
- [x] **Adopt `Hemisphere-Project/ESPNowMeshClock`'s forward-only-
  slewed time-sync algorithm** for beacon resync. Done; reimplemented
  in `mesh_mac.c` (`mesh_now_us`, `MESH_CLOCK_*` constants) — no
  GPL-3.0 code copied, only the algorithm. Slew alpha = 1/16, large-
  step threshold = 5 ms. First beacon snaps the joiner's clock to the
  coord's; subsequent ones slew silently. Verified: rx-drain rates
  stay at ~100/75 fps after lock.
- [x] **VAD-gate the TX path so the wire bit is real.** Done — but it
  shipped broken and was repaired 2026-06-12: WebRTC_NS only updates
  `priorSpeechProb` inside `WebRtcNs_Analyze` (AnalyzeCore ->
  SpeechNoiseProb), which we never called, so the probability sat at
  its 0.5 init forever and the strictly-greater 0.5 threshold never
  opened — the intercom was effectively mute from that commit until
  the fix (the original "decode-count win" was PLC underruns, not
  VAD). With Analyze+Process per frame the gate works (38-100%
  active during speech, max prob 88-99%). Analyze-every-frame cost
  ~4.1 ms and blew the tick budget (~11.5 ms); the shipping config
  runs Analyze every 2nd frame: NS ~2.6 ms mean / ~7 ms max, tick
  back at ~10.0-10.15 ms under sustained speech, VAD 54-92% active
  while talking, drain rates match the VAD duty cycle (~0.9 x
  100/75). Verified on the bench 2026-06-12 with a talk test —
  which is the lesson: quiet-room counters alone can't validate
  anything VAD-shaped.
- [x] **Stop transmitting during silence (actual airtime save).**
  Done; `MESH_HEARTBEAT_SFRAMES = 5` in `mesh_mac.c`. Silent joiner
  slots skip the radio entirely; one header-only frame goes out every
  5 superframes (100 ms) as a slot-claim keep-alive — half the
  `MESH_PEER_QUIET_SFRAMES` (10) margin. Coordinators are exempt: the
  beacon every 40 ms is their keep-alive, so audio-slot silence on
  the coord is dropped outright (net coord silence airtime = one
  beacon every 40 ms vs the old two frames every 20 ms). The 2026-06
  revert root causes (`s_tx_seq` bumping on un-sent, bounded 16-seq
  replay window, one-shot JOIN) were already repaired by the
  2026-06-12 review. A 10-s window log emits `tx 10s: audio=… hb=…
  skip=… (X% silenced)` on each rider. Still needs a two-board soak
  to confirm coord-loss and peer-left timers stay quiet under
  sustained silence.
- [~] **Add Codec2 as a build-time alternate codec behind Kconfig.**
  Scaffold done; upstream not yet vendored. What's in:
    - Partition resize: OTA slots 0x1D0000 → 0x1E0000 (`firmware/
      partitions.csv`); LC3 build is back to 5% free.
    - Codec abstraction `firmware/main/codec.h` with one API and
      `CODEC_FRAME_BYTES/SAMPLES/SAMPLE_RATE_HZ/MAX_DECODERS`
      consumed by every former `codec_lc3.h` call site.
    - `codec_lc3.c` refactored to implement the new API; old
      `codec_lc3.h` deleted.
    - `Kconfig.projbuild` CHOICE `BIKE_CODEC = LC3 | CODEC2`
      (default LC3); `CMakeLists.txt` picks `codec_lc3.c` or
      `codec_codec2.c` at configure time.
    - `codec_codec2.c` stub: codec2 build links cleanly (saves
      ~115 KB via `--gc-sections` because liblc3 falls out) but
      `codec_init` aborts with a clear log until upstream lands.
  What's next (separate diff because it's hundreds of files):
    - Vendor `drowe67/codec2` at
      `firmware/components/codec2/upstream/` with a component
      CMakeLists building only the 3200 bps mode.
    - Flesh out `codec_codec2.c`: pre-alloc 1x encoder + 8x decoders
      in internal RAM, buffer two `CODEC_FRAME_SAMPLES` PCM ticks
      per encode (20 ms), zero-pad the 8 B output into the 40 B
      wire slot every other tick, return 0 on intermediate ticks.
    - Measure on the LX6 bench; add a codec2 column to
      `docs/codec_perf.md` and an A/B speech eval blurb to
      `docs/build_v0.md`.
- [ ] **Vendor SpeexDSP** (from `rjsachse/ESP32-SpeexDSP/src/speex/`)
  for AEC + AGC + VAD — pull the C sources, not the Arduino wrapper.
  Matters once helmet speaker bleeds into mic. 3-5 days; DRAM audit
  first.
- [ ] **Bookmark `tanakamasayuki/PCMFlowG722`** as a Plan-B wideband
  codec (~10 KB flash, ~512 B RAM, but 64 kbps wire cost is 2x our
  LC3 — bad for the 8-slot frame). 0 effort; don't migrate.
- [ ] **Defer BTstack-vs-Bluedroid decision** until coexistence is
  bench-tested with HFP on STA-locked Wi-Fi + mesh active. No
  verified OSS evidence currently favors either stack on LX6 in
  2026; decision must be data-driven. 1 week bench harness when
  ready.

## Auto-memory

The user's auto-memory store at
`~/.claude/projects/-Users-welker-Dev-wlkr-ch-bike-comm/memory/`
already has entries for the LyraT-Mini mic routing, the recv-callback
constraint, the audio_io stack size, and the mesh-audio TX-vs-beacon
collision. Update those when the underlying fact changes (e.g. when
we move beacon to a dedicated 9th slot, retire the asymmetry memory).

## graphify

This project has a knowledge graph at graphify-out/ with god nodes, community structure, and cross-file relationships.

Rules:
- For codebase questions, first run `graphify query "<question>"` when graphify-out/graph.json exists. Use `graphify path "<A>" "<B>"` for relationships and `graphify explain "<concept>"` for focused concepts. These return a scoped subgraph, usually much smaller than GRAPH_REPORT.md or raw grep output.
- If graphify-out/wiki/index.md exists, use it for broad navigation instead of raw source browsing.
- Read graphify-out/GRAPH_REPORT.md only for broad architecture review or when query/path/explain do not surface enough context.
- After modifying code, run `graphify update .` to keep the graph current (AST-only, no API cost).
