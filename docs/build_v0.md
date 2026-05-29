# v0 — bench bring-up on LyraT-Mini

Goal: two ESP32-LyraT-Mini boards talking voice over ESP-NOW. No phone, no PCB.

## What you need

- 2× **ESP32-LyraT-Mini** dev boards (Espressif). Mouser / DigiKey / official.
- 2× wired earbud headsets with mic (3.5 mm TRRS).
- 2× USB-C cables.
- A Mac (you're already there) with ESP-IDF v5.x installed.

## ESP-IDF setup

```sh
# One-time install
brew install cmake ninja dfu-util python@3.12 ccache
mkdir -p ~/esp && cd ~/esp
git clone --recursive https://github.com/espressif/esp-idf.git -b v5.3
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

## Bring-up order

1. **NVS + UI + LED** — confirm GPIOs match LyraT-Mini schematic; pulse the LED.
2. **Audio I/O** — I2S mic loopback to I2S speaker, no DSP. Hear your own voice with ~40 ms latency.
3. **ESP-SR AFE** — wire AEC + NS + VAD. Loopback should now sound clean.
4. **LC3** — vendor `google/liblc3` into `firmware/components/liblc3/`; encode + decode roundtrip; bit-exact against the LC3 conformance vectors.
5. **ESP-NOW raw** — board A broadcasts 30 B test pattern @ 100 Hz; board B receives and logs. Validate range and packet loss across the room.
6. **2-slot TDMA** — minimal version of `mesh_mac.c`: hardcoded 2 riders, alternating slots, no beacon.
7. **Audio over ESP-NOW** — encoder → mesh TX → mesh RX → decoder → speaker. **Milestone.**
8. **Bidirectional, both boards** — two-way conversation through wired headsets.

## What we are NOT doing in v0

- Phone (HFP/A2DP) — that's v0.5
- Beacon / coordinator failover — v0.5
- XOR FEC — v0.5
- Real helmet — v1 problem
- Custom PCB — v1

## Definition of done for v0

- Two boards, two headsets, two-way voice
- Subjective audio quality ≥ "phone call from a noisy cafe"
- End-to-end latency < 150 ms (measure with a click train on a scope)
- 30 min soak with no crash, no leak

If we pass that, we go to v0.5: scale to 8 riders + add the phone link.
