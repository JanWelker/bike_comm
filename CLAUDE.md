# Working notes for Claude

Operational and stylistic conventions for this repo. The canonical
design lives in `docs/`; this file is about how to collaborate on the
code without re-relearning the same lessons every session.

## What this is

An open-source motorcycle helmet intercom on the original ESP32 (LX6,
WROVER-E). Single radio, single chip, ESP-IDF v5.5.4. Two-board mesh
audio over ESP-NOW already works on the bench; phone link (HFP-HF +
A2DP-sink) and ESP-SR AFE are not yet wired. See `README.md` for status
and `docs/architecture.md` for the layered view.

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

## Critical gotchas (don't relearn these)

- **`audio_io` task stack must be 7 KB.** 4 KB overflows on
  LC3 encode + cross-core mesh mutex (symptom: `Guru Meditation
  IllegalInstruction` with a wild PC on core 0, not core 1). 8 KB
  starves `mesh_mac_start` of internal RAM. Sweet spot is 7168.
- **The ESP-NOW recv callback cannot block or allocate.** It runs in
  the wifi task. `heap_caps_malloc` inside the callback (even
  transitively, via lazy codec decoder acquisition) stalls Wi-Fi and
  drops beacons -> coordinator-lost storms on peers. Pre-allocate
  everything at init; the callback only `memcpy`s into a small struct
  and `xQueueSend`s with timeout 0.
- **Coordinator can't TX both beacon and audio in the same slot.**
  Slot 0 alternates: beacon on even superframes, audio on odd. This
  halves the beacon rate to 25 fps (one per 40 ms). The coord-loss
  timer is set to 10 superframes (200 ms) to tolerate the wider gap.
  Don't tighten it without also revisiting the beacon cadence.
- **Mesh audio is currently asymmetric by design.** A joiner -> coord
  link runs at ~50 fps; coord -> joiner runs at ~25 fps because of the
  alternating beacon. Proper fix is a dedicated 9th slot for the beacon
  (v0.5).
- **LC3 codec state must live in internal RAM.** `heap_caps_malloc`
  with `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`. PSRAM access from the
  hot encode/decode loop craters throughput on the original ESP32.

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
  it." Counts per rider should be ~47 fps (joiner -> coord) and ~24 fps
  (coord -> joiner).
- A symmetric counts mismatch is the design; an asymmetric *drop* to
  zero is a regression.
- When tweaking timing constants (slot length, beacon cadence,
  coord-loss threshold), update both the code and
  `docs/mesh_protocol.md` in the same commit.

## Auto-memory

The user's auto-memory store at
`~/.claude/projects/-Users-welker-Dev-wlkr-ch-bike-comm/memory/`
already has entries for the LyraT-Mini mic routing, the recv-callback
constraint, the audio_io stack size, and the mesh-audio TX-vs-beacon
collision. Update those when the underlying fact changes (e.g. when
we move beacon to a dedicated 9th slot, retire the asymmetry memory).
