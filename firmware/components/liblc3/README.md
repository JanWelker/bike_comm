# liblc3 (vendored)

Google's reference LC3 codec, used as the mesh voice codec at 24 kbps / 10 ms / 16 kHz.

## Vendoring

This directory will hold `google/liblc3` as a git subtree (not submodule — keeps the codec sources in this repo so a clone is self-sufficient).

```sh
# from repo root
git subtree add --prefix=firmware/components/liblc3 \
    https://github.com/google/liblc3.git v1.1.1 --squash
```

After vendoring, add an `idf_component.yml` or an `idf_component_register()` shim so ESP-IDF picks it up as a component.

## Why LC3 (and not Opus)

- Designed for the exact duty cycle we want (10 ms voice frames at 16/24/32 kbps).
- Cheaper encoder than Opus, no CPU spikes — important on the original ESP32's LX6 cores which lack the PIE vector ISA the S3 has.
- Native PLC (packet-loss concealment) — saves us writing our own.
- Bluetooth SIG-standard codec, so we share heritage with LE Audio if we ever migrate the mesh transport.

## Reference vectors

Conformance vectors live in `liblc3/conformance/`. Wire them into the firmware unit tests (`firmware/test/`) so we can bit-exact-verify the encoder on host.
