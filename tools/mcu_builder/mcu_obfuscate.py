#!/usr/bin/env python3
"""
Limcet STM32F105 MCU Firmware Obfuscator / De-obfuscator
Target: Companion MCU firmware (can_app.bin) for Limcet head units.

Matches the authentic factory bootloader de-obfuscation logic (address 0x08001F72)
documented in docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md (Sections 28 & 35):
    flash[i] = (~wire[i] + T[i % 128]) & 0xFF
    wire[i]  = ~(flash[i] - T[i % 128]) & 0xFF

Usage:
    python3 mcu_obfuscate.py --obfuscate plain.bin -o can_app.bin
    python3 mcu_obfuscate.py --deobfuscate can_app.bin -o recovered.bin
    python3 mcu_obfuscate.py --verify plain.bin
"""

import sys
import os
import argparse
import hashlib

# 128-byte lookup table T from factory bootloader at 0x08001F72
DEOBF_TABLE = bytes([
    0x78, 0xF8, 0x79, 0xF9, 0x7A, 0xFA, 0x7B, 0xFB, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    0xFF, 0x7F, 0xFE, 0x7E, 0xFD, 0x7D, 0xFC, 0x7C, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00,
    0x7C, 0xFC, 0x7D, 0xFD, 0x7E, 0xFE, 0x7F, 0xFF, 0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA, 0x99, 0x88,
    0xFB, 0x7B, 0xFA, 0x7A, 0xF9, 0x79, 0xF8, 0x78, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00,
    0x78, 0xF8, 0x79, 0xF9, 0x7A, 0xFA, 0x7B, 0xFB, 0x1F, 0x2E, 0x3D, 0x4C, 0x5A, 0x69, 0x78, 0x87,
    0xFF, 0x7F, 0xFE, 0x7E, 0xFD, 0x7D, 0xFC, 0x7C, 0x96, 0xA5, 0xB4, 0xC3, 0xD2, 0xE1, 0xF0, 0x00,
    0x7C, 0xFC, 0x7D, 0xFD, 0x7E, 0xFE, 0x7F, 0xFF, 0xF1, 0xE2, 0xD3, 0xC4, 0xA5, 0x96, 0x87, 0x78,
    0xFB, 0x7B, 0xFA, 0x7A, 0xF9, 0x79, 0xF8, 0x78, 0x69, 0x5A, 0x4B, 0x3C, 0x2D, 0x1E, 0x0F, 0x00
])

assert len(DEOBF_TABLE) == 128


def obfuscate_image(plain_bytes: bytes) -> bytes:
    """Transform plaintext flash binary into wire format for factory bootloader."""
    # Pad to a multiple of 128 bytes with 0xFF
    pad_len = (128 - (len(plain_bytes) % 128)) % 128
    padded = plain_bytes + (b"\xFF" * pad_len)

    wire = bytearray(len(padded))
    for i in range(len(padded)):
        t = DEOBF_TABLE[i % 128]
        wire[i] = (~(padded[i] - t)) & 0xFF
    return bytes(wire)


def deobfuscate_image(wire_bytes: bytes) -> bytes:
    """Transform wire format bytes into plaintext flash bytes."""
    plain = bytearray(len(wire_bytes))
    for i in range(len(wire_bytes)):
        t = DEOBF_TABLE[i % 128]
        plain[i] = ((~wire_bytes[i] & 0xFF) + t) & 0xFF
    return bytes(plain)


def main():
    parser = argparse.ArgumentParser(description="Limcet MCU Firmware Obfuscator/De-obfuscator")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--obfuscate", "-o-wire", metavar="PLAIN_BIN", help="Obfuscate plaintext binary into wire format")
    group.add_argument("--deobfuscate", "-d-wire", metavar="WIRE_BIN", help="De-obfuscate wire binary into plaintext")
    group.add_argument("--verify", metavar="PLAIN_BIN", help="Verify roundtrip (plain -> wire -> plain)")

    parser.add_argument("-o", "--output", help="Output file path")
    args = parser.parse_args()

    if args.verify:
        with open(args.verify, "rb") as f:
            plain = f.read()
        wire = obfuscate_image(plain)
        recovered = deobfuscate_image(wire)
        pad_len = (128 - (len(plain) % 128)) % 128
        expected = plain + (b"\xFF" * pad_len)
        if recovered == expected:
            print(f"[+] Roundtrip verification SUCCESS! ({len(plain)} bytes -> {len(wire)} bytes)")
            sys.exit(0)
        else:
            print(f"[-] Roundtrip verification FAILED!", file=sys.stderr)
            sys.exit(1)

    if args.obfuscate:
        with open(args.obfuscate, "rb") as f:
            plain = f.read()
        wire = obfuscate_image(plain)
        out_path = args.output or (args.obfuscate + ".wire")
        with open(out_path, "wb") as f:
            f.write(wire)
        print(f"[+] Obfuscated {len(plain)} plaintext bytes -> {len(wire)} wire bytes")
        print(f"    Output: {out_path} (SHA256: {hashlib.sha256(wire).hexdigest()})")

    if args.deobfuscate:
        with open(args.deobfuscate, "rb") as f:
            wire = f.read()
        plain = deobfuscate_image(wire)
        out_path = args.output or (args.deobfuscate + ".plain")
        with open(out_path, "wb") as f:
            f.write(plain)
        print(f"[+] De-obfuscated {len(wire)} wire bytes -> {len(plain)} plaintext bytes")
        print(f"    Output: {out_path} (SHA256: {hashlib.sha256(plain).hexdigest()})")


if __name__ == "__main__":
    main()
