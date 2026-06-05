# Vendoring notes — cpuimage/WebRTC_NS

Source: https://github.com/cpuimage/WebRTC_NS
License: BSD-3-Clause (WebRTC project authors).
Imported at upstream commit `108e22e` (`Update README.md`).

The cpuimage repo packages WebRTC's classic noise-suppression module
as a single translation unit (`noise_suppression.c` + `.h`) with the
real-DFT bundled inline. Float math only — no SIMD, no intrinsics, no
inline asm. The 10 ms / 160-sample block size at 16 kHz maps 1:1 to
bike_comm's LC3 cadence.

## What we vendor

The upstream repo contains a WAV-file demo (`main.c`, `dr_mp3.h`,
`dr_wav.h`, `timing.h`) alongside the library proper. We only ship the
library files needed for embedded use:

- `noise_suppression.c` — the implementation, including the inlined
  Ooura `WebRtc_rdft`.
- `noise_suppression.h` — public API (`WebRtcNs_Create`,
  `WebRtcNs_Init`, `WebRtcNs_set_policy`, `WebRtcNs_Process`).
- `LICENSE`, `README.md` — provenance.

The demo files are intentionally excluded; they don't build under
ESP-IDF (depend on host file I/O and stdio).

## Why this and not espressif/esp-sr

esp-sr's precompiled `ns_process` for the original ESP32 LX6 calls
`heap_caps_check_integrity_all` and crashes inside the heap walker —
see CLAUDE.md "ESP-SR AFE doesn't work on the LX6" gotcha. The
cpuimage/WebRTC_NS path was identified in the 2026-06 deep-research
pass as the cleanest source-tree alternative for the LX6.

## API quick reference

```c
NsHandle *ns = WebRtcNs_Create();
WebRtcNs_Init(ns, 16000);                    // sample rate
WebRtcNs_set_policy(ns, 1);                  // 0=mild 1=medium 2=aggressive
// per 10 ms (160-sample) frame:
const int16_t *in[1] = { mic_pcm };
int16_t       *out[1] = { ns_out_pcm };
WebRtcNs_Process(ns, in, 1, out);
// teardown:
WebRtcNs_Free(ns);
```

## Re-importing

```sh
git clone --depth 1 https://github.com/cpuimage/WebRTC_NS.git /tmp/WebRTC_NS
cp /tmp/WebRTC_NS/{noise_suppression.c,noise_suppression.h,LICENSE,README.md} \
   firmware/components/webrtc_ns/upstream/
```

Update the commit hash above. Re-run the soak and check that the
audio_io 10 ms tick budget still holds (see `docs/codec_perf.md`).
