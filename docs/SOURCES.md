# Sources

**Status:** Reference
**Last Updated:** 2026-08-04

## Overview

## Base firmware
**Holden firmware package** — complete firmware for Ksmart DSP / Box-C211  
Used as structural base: bootloaders, kernel, rootfs image, userdata image, UpConfig trigger, update script.

## Prado-specific overrides
**Prado live NAND dump** — raw MTD partition dumps from a physical Toyota Prado running Holden firmware  
Source of: arkdata display config (MTD4), U-Boot environment (MTD3), partition layout confirmation.

**Prado live userdata dump** — UBIFS `/data` partition extracted from the live device  
Source of: MsnProductInfo.ini (ProductId=Limcet-P306), msncfg settings, pointercal calibration.

## Reference only (not in build)
**Alfa 5G Italian 8.8 -06 P305** — Alfa Romeo firmware package for comparison  
**Prado Msnconfig** — Prado-specific factory config variations  
**`ArkPro Reference/`** — ASTRI's reference ARK1680 kernel/U-Boot/userspace source (public leak,
`cphatt/ArkPro` commit `e743744`), used to confirm SoC-identity and register-field findings in
`docs/HARDWARE_AND_SOC_REFERENCE.md` §9 and `docs/DISPLAY_SUBSYSTEM.md`; see `ArkPro Reference/README.md`  
**`linux-arkmicro Reference/`** — ArkMicro's own public U-Boot BSP (`RD_Software/linux-arkmicro`,
a later generation than the Prado's actual stock U-Boot — see that folder's README), the starting
point for `docs/UBOOT_BUILD_GUIDE.md`'s from-source build plan  
**`ark1668ed-bsp`** (personal Downloads, not tracked in any repo) — ArkMicro's own internal BSP
for a newer SoC variant (ARK1668ED, Linux 6.12.56, ArkMicro's internal Gogs server), different
kernel generation and USB PHY but same WiFi/BT chip family; source of the RTL8821CS/RTL8811CU
WiFi driver updates (`linux-arkmicro` repo, branch `wifi-rtl8821cs-driver-port`) and the Android
Auto/audio-pipeline/USB-hotplug findings in `docs/VENDOR_BSP_RESEARCH.md`  
**`cstech-ip17-rootfs`** (personal Downloads, not tracked in any repo) — a rootfs-only dump from a
different, older ArkMicro product (ARK1680, Linux 3.4.0); wrong SoC generation for direct reuse,
used only to corroborate the `com.arkmicro.*` D-Bus naming convention — see
`docs/VENDOR_BSP_RESEARCH.md` §1

## Key differences applied vs base Holden firmware

| Item | Holden base | Prado override | Source |
|------|------------|----------------|--------|
| arkdata display | 800×480, CLKDIV1=10, IVS=1, no touch keys | 800×480, CLKDIV1=11, IVS=1, VBP=29, HBP=32 | MTD4 dump |
| bootdelay | 0 | 9 | MTD3 env dump |
| ProductId | Ksmart_DSP | Limcet-P306 | userdata MsnProductInfo |
| ResourceName | Box-C211 | Box-P301 | userdata MsnProductInfo |
| McuType | 16 | 6 | userdata MsnProductInfo |
| SoundType | 4 (DSP) | 0 | userdata MsnProductInfo |
| BlueToothType | 6 | 6 | matches |
| DeviceName (BT) | Ksmart | Limcet Box | Prado identity |
| PairCode | 0000 | 8362 | Prado identity |
| HomeIconLabel | HOLDEN | TOYOTA | Prado vehicle branding |
| ReversingVolumeCut | 0 | 0 | matches Holden |
| Partition layout | 106m/6m | 106m/6m | confirmed identical |