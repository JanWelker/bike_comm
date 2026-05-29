# Hardware v1

Custom PCB. Empty until v0 firmware is working on LyraT-Mini.

## Tooling

KiCad 8+ (`brew install --cask kicad`). Schematic in `schematic.kicad_sch`, layout in `pcb.kicad_pcb`. Symbol/footprint libraries: KiCad stock + the JLCPCB / LCSC parts library for any odd house parts.

## Target

| | |
|---|---|
| Board outline | ~50 × 35 mm (helmet-mount module) |
| Layer count | 4 (signal / GND / 3V3 / signal) |
| Stackup | JLCPCB 4-layer 1.6 mm standard |
| Input | USB-C, 5 V |
| Battery | 3.7 V LiPo, 1500 mAh pouch |
| Estimated BOM | ~$25 at qty 10 |

## Blocks

1. ESP32-WROVER-IE module (external IPEX antenna)
2. ES8388 codec + INMP441 MEMS mic + MAX98357A I2S amp
3. TP4056 + DW01 + FS8205 charge / protection
4. TPS62840 buck (3.3 V) or MCP1700 LDO (simpler v1 fallback)
5. USB-C for charge + serial debug + OTA flash
6. 3 tactile buttons (glove-friendly), RGB LED
7. Battery monitor via ADC divider

## Layout notes (when we get there)

- 2.4 GHz antenna keep-out under the IPEX trace.
- Star ground at the codec.
- USB DP/DM 90 Ω diff pair, short.
- Battery + amp in their own copper pour to keep mic ground quiet.

## License

CERN-OHL-P-2.0 (permissive open hardware) once the first commit lands. The board files inherit the license from this directory's LICENSE-HARDWARE file (TBD).
