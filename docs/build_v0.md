# v0 — bench bring-up on LyraT-Mini

Goal: two ESP32-LyraT-Mini boards talking voice over ESP-NOW. No phone, no PCB.

## What you need

- 4× **ESP32-LyraT-Mini** dev boards (Espressif). Mouser / DigiKey / official.
  - 2 are used in v0 (two-way voice). The other 2 unlock v0.5 mesh-protocol tests without a second order, and give us a spare for RF-range bench work (one board at an attenuator) and a known-good comparison node when something misbehaves. Two-node tests can't surface the interesting bugs in TDMA (slot arbitration, coordinator failover, multi-source jitter).
- 4× wired earbud headsets with mic (3.5 mm TRRS).
- 4× USB cables matching the LyraT-Mini connector (verify on the product listing — likely Micro-USB, possibly USB-C on newer revs). The laptop only flashes one at a time, but it's nicer to leave each board cabled to its own power source during multi-node tests.
- A Mac (you're already there) with ESP-IDF v5.5 installed.

## ESP-IDF setup

```sh
# One-time install
brew install cmake ninja dfu-util python@3.12 ccache
mkdir -p ~/esp && cd ~/esp
git clone --recursive --depth 1 -b v5.5.4 https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32

# Each shell
. ~/esp/esp-idf/export.sh
```

For the audio framework (ESP-ADF) and AFE (ESP-SR), add as managed components later — first get a plain ESP-IDF build green.

## First build

```sh
cd firmware/
idf.py set-target esp32
idf.py menuconfig         # confirm PSRAM + BT Classic enabled per sdkconfig.defaults
idf.py build
idf.py -p /dev/cu.usbserial-XXXX flash monitor
```

You should see `bike_comm starting (chip: ESP32 single-chip path)` and `bike_comm ready`. None of the modules do anything real yet — this just validates the build + boot.

## Bring-up order (status reflects what's actually working on the bench)

1. **NVS + UI + LED** — done; module init order in `main.c` is the source of truth.
2. **Audio I/O** — done. ES8311 playback + ES7243 mic ADC via `esp_codec_dev`. Onboard MEMS lands on ES7243 `AINRP/AINRN` (right channel, stereo I2S slot mask = RIGHT). ES8311 mic pins are dead on v1.2 (caps `(NC)`), so we don't use them. See the comment block at the top of `firmware/main/audio_pipeline.c` for the full pin map. Self-loopback verified through the 3.5 mm jack with a wired headphone.
3. **LC3** — done. `firmware/components/liblc3/upstream/` vendored at v1.1.1; `codec_lc3_init` pre-allocates the shared encoder and all 8 decoder slots upfront (lazy-allocating inside the wifi recv callback drops beacons).
4. **ESP-NOW + custom TDMA** — done. `mesh_mac.c` does join with bootstrap path for solo coordinator, alternating beacon/audio in slot 0 (lets the coordinator transmit its own mic in addition to beaconing every 40 ms), per-rider quiet-timeout, XOR-FEC piggyback, anti-replay seq tracking, and coord-loss failover (10 sframes = 200 ms tolerant of short RF bursts).
5. **Audio over ESP-NOW (one direction)** — done. Joiner → coordinator at ~50 fps decoded.
6. **Audio over ESP-NOW (bidirectional)** — done. Coordinator's slot-0 alternates beacon/audio so it can also TX; ~25 fps reverse direction.
7. **ESP-SR AFE** — not yet. Wire AEC + NS + VAD onto the audio_io task; the AEC reference signal is already exposed (`mixer_pull` second arg) and the schematic-level loopback path (ES8311 OUT → ES7243 `AINLP/AINLN`) already gives the AFE the speaker echo to subtract.

## What we are NOT doing in v0

- Phone (HFP/A2DP) — that's v0.5.
- 4-node mesh / large slot map — v0.5; the TDMA logic already supports 8 slots, just hasn't been load-tested past 2.
- Beacon-PLL time sync — v0.5. Local `esp_timer_get_time()` drives the superframe scheduler today. Drift is bounded at ±20 ppm crystal so we don't see slot collisions on the 2-board bench, but multi-node + longer durations will need PLL.
- Per-slot peer-mac collision detection — v0.5; today we trust ourselves on JOIN.
- Real helmet — v1.
- Custom PCB — v1.

## Known gotchas

- `audio_io` task stack must be **7 KB**. 4 KB overflows on LC3 encode + cross-core mesh mutex (manifests as a wild-PC `Guru Meditation IllegalInstruction` on core 0). 8 KB starts crowding internal RAM enough for `mesh_mac_start` to OOM.
- The ESP-NOW recv callback runs in the wifi task and **cannot block or allocate**. `on_mesh_rx` in `main.c` only memcpys to a small struct and `xQueueSend`s with timeout 0; the audio_io task drains the queue.

## Definition of done for v0

- Two boards, two headsets, two-way voice
- Subjective audio quality ≥ "phone call from a noisy cafe"
- End-to-end latency < 150 ms (measure with a click train on a scope)
- 30 min soak with no crash, no leak

If we pass that, we go to v0.5: scale to 8 riders + add the phone link.
