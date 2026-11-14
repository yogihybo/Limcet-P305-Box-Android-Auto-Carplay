#!/usr/bin/env python3
"""Refuse application images that can't run on this unit's MCU.

The Limcet bootloader starts the application at 0x08003000 and only checks its
stack pointer before jumping (factory 0x08001844), so an image linked anywhere
else is accepted, programmed, and then crashes. The DCn32-family reference
images in hardware/MCU/reference_vendor_images/ and firmware_dumps/MCU/
(Volvo, Acura) are linked at 0x08004000 for a different product -- they are
NOT this unit's stock firmware and must never be used to "restore" it. Restore
with the reconstructed factory app instead:
hardware/MCU/live_dumps/vehicle_live_2026-09-14/live_factory_app_52k_reconstructed.bin

Checks (on the plain image, before wire obfuscation):
  - size fits the 116 KB application region;
  - initial SP passes the bootloader's own test, (SP & 0x2FFE0000) == 0x20000000;
  - reset vector is Thumb, inside the image, and in its first 4 KB
    (0x08003000-0x08003FFF). Images linked at 0x08004000 or higher always fail
    this; the factory app (0x08003599) and the clean-room app (0x08003155) pass.

Usage: check_app_image.py <plain.bin>     exit 0 = OK
"""
import struct
import sys

APP_BASE = 0x08003000
APP_SIZE = 0x1D000          # 58 pages x 2 KB
FIRST_4K_END = APP_BASE + 0x1000


def problems(image: bytes) -> list[str]:
    out = []
    if len(image) < 8:
        return ["image is shorter than a vector table"]
    if len(image) > APP_SIZE:
        out.append(f"{len(image)} bytes exceeds the {APP_SIZE}-byte application region")
    sp, reset = struct.unpack_from("<II", image)
    if (sp & 0x2FFE0000) != 0x20000000:
        out.append(f"initial SP 0x{sp:08X} fails the bootloader's check")
    if not reset & 1:
        out.append(f"reset vector 0x{reset:08X} is not a Thumb address")
    if not (APP_BASE <= reset < min(FIRST_4K_END, APP_BASE + len(image))):
        hint = (" -- linked for 0x08004000 (DCn32 family: Volvo/Acura reference image, not this unit's firmware)"
                if 0x08004000 <= reset < 0x08005000 else "")
        out.append(f"reset vector 0x{reset:08X} is not in the image's first 4 KB at 0x{APP_BASE:08X}{hint}")
    return out


def main():
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    with open(sys.argv[1], "rb") as f:
        found = problems(f.read())
    for p in found:
        print(f"[!] {sys.argv[1]}: {p}", file=sys.stderr)
    if found:
        print("[!] Refusing: this image would not run on the Limcet MCU (application base 0x08003000).",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
