#!/usr/bin/env python3
"""
psk_gen — generate a 128-bit mesh group PSK.

The PSK is the AES-128 key used by ESP-NOW for encryption + authentication
of mesh frames. Two riders with the same PSK are in the same group; two
riders with different PSKs can be in RF range but cannot decrypt each
other's packets (and our app-layer also ignores them).

Usage:
    psk_gen.py                  # print hex + base64 + optional QR
    psk_gen.py --qr out.png     # write a QR PNG to out.png

The QR payload is a URL of the form
    bikecomm://group?psk=<base64url>
so a future companion app can scan it and pair the rider in one tap.
"""

from __future__ import annotations

import argparse
import base64
import secrets
import sys
from pathlib import Path


def gen_psk() -> bytes:
    """128 bits from the OS CSPRNG."""
    return secrets.token_bytes(16)


def encode_url(psk: bytes) -> str:
    b64 = base64.urlsafe_b64encode(psk).rstrip(b"=").decode("ascii")
    return f"bikecomm://group?psk={b64}"


def write_qr(url: str, path: Path) -> None:
    try:
        import qrcode  # type: ignore
    except ImportError:
        sys.exit(
            "QR output requires the 'qrcode' package:\n"
            "    pip install 'qrcode[pil]'"
        )
    img = qrcode.make(url)
    img.save(str(path))
    print(f"wrote QR -> {path}")


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--qr", type=Path, help="write a QR PNG to this path")
    p.add_argument("--seed-hex", help="use this 32-char hex instead of CSPRNG (testing only)")
    args = p.parse_args()

    if args.seed_hex:
        psk = bytes.fromhex(args.seed_hex)
        if len(psk) != 16:
            sys.exit("--seed-hex must be exactly 32 hex chars (128 bits)")
    else:
        psk = gen_psk()

    url = encode_url(psk)

    print("PSK (hex)    :", psk.hex())
    print("PSK (base64) :", base64.b64encode(psk).decode("ascii"))
    print("Pair URL     :", url)
    print()
    print("Flash via serial console with:")
    print(f"  cfg set psk {psk.hex()}")

    if args.qr:
        write_qr(url, args.qr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
