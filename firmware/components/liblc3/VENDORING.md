# liblc3 (vendored)

Google's reference LC3 codec, used as the mesh voice codec at 24 kbps /
10 ms / 16 kHz mono. Lives at `upstream/`; this directory's
`CMakeLists.txt` wraps it as an ESP-IDF component.

## Version

- liblc3 **v1.1.1** (https://github.com/google/liblc3)
- License: **Apache-2.0** (same as this project; see `upstream/LICENSE`)

## How it got here

```sh
# from repo root
git subtree add --prefix=firmware/components/liblc3/upstream \
    https://github.com/google/liblc3.git v1.1.1 --squash
```

The `upstream/` subdirectory holds the unmodified source tree. The
ESP-IDF component wrapper (`CMakeLists.txt`) sits one level up so
upstream stays clean and the next subtree pull is conflict-free.

## How to update

```sh
git subtree pull --prefix=firmware/components/liblc3/upstream \
    https://github.com/google/liblc3.git <new-tag> --squash
```

Then re-run the host-side roundtrip test in `firmware/test/codec_lc3/`
to catch any API drift, and bump this file's version line.

## Why LC3 (and not Opus)

- Designed for the exact duty cycle we want (10 ms voice frames at
  16/24/32 kbps).
- Cheaper encoder than Opus, no CPU spikes — important on the original
  ESP32's LX6 cores which lack the PIE vector ISA the S3 has.
- Native PLC (packet-loss concealment) — saves us writing our own.
- Bluetooth SIG-standard codec, so we share heritage with LE Audio if
  we ever migrate the mesh transport.

## Conformance vectors

Upstream ships conformance vectors in `upstream/conformance/`. Not yet
wired into firmware unit tests; for now the host roundtrip in
`firmware/test/codec_lc3/` is the smoke test.
