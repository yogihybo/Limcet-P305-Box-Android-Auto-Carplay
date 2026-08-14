# Handoff: 2026-07-17 -- bootnand hang, NAND data-integrity bug, MsnCoreApp
# version mismatch, RTC, and dual-USB-controller/WiFi fix

**Audience:** the agent/model picking this up next. Self-contained --
you don't need the prior conversation. This was a single long session
that started with one specific bug report (`bootnand` hanging) and
expanded into several independent, real, hardware-confirmed fixes.
Detailed technical writeups live in the topic-specific docs linked from
each section below; this doc is the map, not the full detail.

**Repos touched:** `/media/sf_GitHub/prado-firmware-reconstruction`
(main repo) and `/home/osboxes/Downloads/linux-arkmicro` (separate git
repo, U-Boot + kernel source tree). Both fully committed and pushed as
of this doc.

---

## Headline results (all confirmed on real hardware unless noted)

1. **`bootnand` kernel-entry hang -- FIXED.** A NULL ATAGS pointer was
   clobbering the exception vector table at address `0x0`. `bootnand`
   now boots the stock kernel+rootfs all the way to `MsnCoreApp start`.
2. **Active NAND flash-corruption bug -- FIXED.** The kernel NAND driver
   was silently writing wrong bad-block tables to physical flash on
   *every* boot (even boots that never touch NAND), reproducing the
   historical "417 false bad blocks" symptom. Root-caused to a missing
   `ecc.read_oob` override reading the wrong physical OOB layout.
   Confirmed: 417 false bad blocks -> exactly 1, matching the known-good
   historical reference count.
3. **`MsnCoreApp` was the wrong binary -- FIXED, then refined again.**
   First fix used a binary matching an old capture in this repo, but
   the *real* device runs a different, newer core-app build (confirmed
   via the real device's own About screen + embedded version strings in
   `libMsnCommons.so`). Replaced with the exact binary set confirmed
   running on the real car unit right now.
4. **RTC never worked -- FIXED.** Hardware and driver both existed,
   `CONFIG_RTC_CLASS` was just never enabled.
5. **WiFi could never work at the same time as a boot/accessory USB
   stick -- FIXED.** Root cause: a bad device-tree interrupt override
   left one of the board's *two* USB controllers permanently broken
   since 2026-07-16. Confirmed on hardware: WiFi AP + boot stick working
   simultaneously, matching stock.
6. **Display investigation on `bootnand`/stock kernel -- inconclusive,
   deprioritized.** Real fix found and applied (OSD2 config), but it
   fixed a *different* boot path than the one under investigation.
   `bootnand`'s stock-kernel display crash is still open; `bootstock`
   remains the reliable path to a working stock UI.

---

## 1. `bootnand` kernel-entry hang + display investigation

Full detail: `docs/historical/HANDOFF_nand_ecc_uboot_vs_kernel.md`
(despite the filename, this doc now covers the kernel-hang and display
investigation too, not just NAND ECC -- read the whole thing, not just
the NAND sections).

**Root cause**: `CONFIG_SYS_BOOTPARAMS_LEN` was never defined for this
board, so `initr_malloc_bootparams()` never ran and `gd->bd->
bi_boot_params` stayed at its zero-initialized value. `setup_start_tag()`
then wrote the entire ATAGS list to physical address `0x0` -- exactly
where this core's exception vector table lives (`SCTLR.V=0`, low
vectors). The kernel's own early boot code corrupted its own vector
table before ever installing real ones. Confirmed via a live register/
ATAGS-pointer dump at the exact jump instant.

`linux-arkmicro` commit `3af8aa7f2`.

**Display investigation** (separate, mostly a dead end): spent a long
stretch on `bootnand`'s `open /dev/ark_display fail` / DirectFB crash.
Ruled out seven register-level hypotheses via live A/B comparison
against real stock U-Boot (`md.l`/`regr`): `LCD_CLK_CFG`, the full
clock-enable/reset block, `LCD_TV_CONTROL`, PWM backlight, DRAM size,
`arkdata.ini` presence, and finally OSD2 layer config (the one real
difference found -- stock leaves OSD2 fully configured/enabled, this
fork left it disabled). Fixing OSD2 did **not** fix the `bootnand`
display crash (confirmed via cold-boot retest, byte-identical failure).
That investigation is understood to require either the stock kernel's
source or a live `dmesg`/shell to progress further, neither available --
left open. `bootstock` (chainloads the real stock U-Boot binary) already
provides a fully working alternative path to the stock UI.

**However**, the OSD2 fix turned out to matter for a different, more
important path: on `bootusb` (this fork's own kernel + reconstructed
rootfs), `MsnCoreApp` was crashing immediately on startup before this
fix, and starts cleanly after it (see section 3 -- this was compounded
with the wrong-binary bug, both needed fixing).

`linux-arkmicro` commit `6f2d3c338`.

---

## 2. NAND: active flash-corruption bug, fully resolved

Full detail: `docs/historical/HANDOFF_nand_ecc_uboot_vs_kernel.md`
section 1 and its follow-ups.

Three-layer fix, all confirmed on real hardware:

1. **Write-prevention override** (`ark_bbt_main_no_oob_descr`/
   `ark_bbt_mirror_no_oob_descr`, `linux-arkmicro` commit `5daf57c02`)
   -- stops the driver from ever persisting a bad-block table to flash,
   mirroring an equivalent U-Boot-side fix from a prior session.
2. **Disabled flash-based BBT caching entirely** (dropped
   `nand-on-flash-bbt` from the device tree, commit `bed41680e`) --
   matches stock's own behavior of always rescanning real factory OOB
   markers at boot rather than trusting a persisted cache. Confirmed:
   `Bad block table written` never appears again.
3. **Root cause, found after the above didn't fully explain a persistent
   ~417 false-bad-block count**: the driver never overrode
   `ecc.read_oob` for `NAND_ECC_HW_SYNDROME` mode, so the generic
   mainline fallback (`nand_read_oob_syndrome()`) was handling every
   OOB-only read -- including the factory bad-block-marker check. That
   generic function assumes an interleaved data/ECC physical layout;
   this chip's real layout (confirmed from the driver's own working
   data-read path) is non-interleaved. Added a correct `ecc.read_oob`
   (commit `e5e8ff00c`). **Confirmed on real hardware: 417 false bad
   blocks -> exactly 1**, matching the known-good reference count from
   a completely separate, older investigation
   (`docs/historical/HANDOFF_touch_and_bootargs_fix.md`).

Also fixed the U-Boot-side equivalent of the probe-time ECC default and
BBT-write issue (`linux-arkmicro` commit `9a28228d2`, main repo doc
commit `0cfc451`) -- not yet independently hardware-verified on the
U-Boot side specifically, but same-class fix, same confidence level as
the kernel-side one which *is* confirmed.

---

## 3. `MsnCoreApp` binary mismatch -- wrong, then refined to correct

Full detail: `msnapp/README.md` (new tracking folder, one subfolder per
known binary variant with MD5/size/provenance).

**First fix** (main repo commit `b209bad`): `MsnCoreApp` in
`firmware_source/prado_reconstructed` was crashing with a NULL-pointer
segfault in `MCUAdapter_BoxP300::onInited()`. Turned out to be a
completely different, much older/smaller binary than what a known-good
reference boot log showed running. Replaced with the copy from
`firmware_dumps/Prado firmware dump` (`prado_dump`), which matched that
reference log's printed version.

**Correction** (main repo commit `9d56450`): built a full version-string
comparison across every firmware dump in the repo (`prado_dump`,
`holden`, `cstech`, `p306`, `prado_recovery`, extracted from each UBI
`rootfs.img` via `ubireader_extract_files`), disassembling
`GetAppVersionString()` out of each `libMsnCommons.so` to recover real
embedded version+date strings. Found `prado_dump`'s own version
(`V3.10.3.0212`) does **not** match the real device's actual on-screen
version (`V3.21.09.0219`, confirmed via a photo of the real unit's About
screen) -- but `holden`'s does, exactly. `prado_dump` in this repo is
believed to be an older capture of this same unit, predating a firmware
update; the real device's branding (`ProductId=Limcet-P306`, confirmed
via the same photo) is genuinely Limcet P306-specific, but its *core app
binary* matches what's preserved in the `holden` dump instead -- most
likely both product lines received the same core-app update around Feb
2024, with per-unit branding layered on top via
`msnprofile/MsnProductInfo.ini`.

**User confirmed directly** that this exact combination (holden's core
binaries + Limcet P306's own config) is what's currently running on the real
car unit. Replaced `MsnCoreApp`, `MsnFirstInit`, `msncarlife`, and 14
matched plugin libraries with `holden`'s versions;
`msnprofile/MsnProductInfo.ini` and all other Limcet P306-specific
branding/profile files left untouched.

**Not yet re-tested on hardware** after this second, corrected swap
(the first swap *was* tested and confirmed the crash was gone, but with
the wrong-family binary -- worth a fresh boot test to confirm the
corrected holden-based set works at least as well).

Also flagged but not yet fixed: `MsnFirstInit` (22KB, unique, doesn't
match any known-good source) and `libSetting.so` (unique MD5, same size
as the reference) look like they could be the same class of bug --
noted in `msnapp/README.md`, not investigated further this session.

---

## 4. RTC never worked

`hwclock: can't open '/dev/misc/rtc'` in every boot log. The device tree
already declared the real hardware (`rtc@40406000`, `compatible =
"arkmicro,ark-rtc"`) and a matching driver (`drivers/rtc/rtc-ark.c`)
already existed -- `CONFIG_RTC_CLASS` was simply never enabled, so the
whole RTC subsystem was compiled out and no `/dev/rtc0` ever appeared.
busybox's `hwclock` applet tries `/dev/rtc`, `/dev/rtc0`, then
`/dev/misc/rtc` in that order (all three compiled in as fallbacks); all
three failed since none existed -- the `/dev/misc/rtc` message shown was
just the last of three failed attempts, not evidence of a real path
mismatch needing a symlink workaround.

Fixed: `CONFIG_RTC_CLASS=y`, `CONFIG_RTC_DRV_ARK=y` in
`ark1668_defconfig` (`CONFIG_RTC_HCTOSYS` came along for free via
`olddefconfig`, syncing system time from the hardware RTC at boot).
`linux-arkmicro` commit `2ec1c5855`. **Not yet hardware-tested.**

---

## 5. WiFi + boot-stick simultaneous operation -- root cause found, fully confirmed

Full detail: `docs/1.4_WIRELESS_AND_INIT.md` section 1a.

Started as "wlan0 doesn't show up when booting via `bootusb`". Initial
theory (this board's single USB hub has one downstream port, shared
between the boot stick and the onboard WiFi module) explained the
correlation in every boot log perfectly but couldn't explain how stock
manages both at once.

**Real root cause**: this board has *two* separate USB controllers,
`usb0` (0xE0100000, external-facing port) and `usb1` (0xE0400000,
almost certainly the onboard WiFi module's own dedicated controller).
`musb-ark e0400000.usb: Failed to get irq.` appeared in *every* boot
log ever captured, working or not -- `usb1` never successfully probed
at all, regardless of boot medium. Traced to a bad device-tree override
in `ark1668_limcet_p305.dts` (`interrupts = <40>, <39>`, added
2026-07-16, bundled unexplained into an unrelated I2S/audio commit):
`usb1`'s interrupt-parent (`vich`) is a single `arm,pl192-vic` with only
32 lines (valid range 0-31); 40 and 39 are out of range, and equal the
device tree base file's correct values (8, 7) plus 32 -- a global-vs-
VIC-local interrupt-numbering mix-up. Fixed by dropping the override.

`linux-arkmicro` commit `6c413e421`.

**CONFIRMED on real hardware**: `bootusb` with a physical USB stick
plugged in, `wlan0: AP-ENABLED` (`carplay_wifi`), `usb-test.sh` shows 2
USB hubs enumerated (both controllers now probing) with WiFi bound and
`wlan0` present. Matches full stock capability -- no SD-card-boot
workaround needed anymore.

Also fixed along the way (main repo commits `06f29c0`, `57558b6`):
reverted `rcS` back to stock default WiFi AP mode (`etc/wifi_ap.sh`,
was switched to client/STA mode 2026-07-14 for development
convenience), and fixed both `wifi_ap.sh`/`wifi_client.sh` using a fixed
`sleep 1` before checking for `wlan0` -- replaced with a poll loop (up
to 30s), since this hardware's USB enumeration can genuinely take
15-20+ seconds.

---

## What's still open

- **`bootnand`'s stock-kernel display crash** (`open /dev/ark_display
  fail`, DirectFB init failure) -- needs stock kernel source or a live
  `dmesg`/shell to progress further. `bootstock` is the reliable
  workaround.
- **U-Boot-side NAND ECC probe-time/BBT-write fix** -- applied, same
  confidence as the kernel-side fix, but not independently
  hardware-verified on the U-Boot side specifically (the kernel side
  *is* confirmed).
- **The corrected `holden`-based `MsnCoreApp` swap** -- not yet
  re-tested on hardware (the *first*, wrong-family swap was tested and
  confirmed non-crashing; the corrected swap hasn't had its own
  hardware test yet).
- **RTC enable** -- not yet hardware-tested.
- **`MsnFirstInit` and `libSetting.so`** -- flagged as possibly the same
  "wrong binary" class of bug as the original `MsnCoreApp` issue, not
  investigated.
- **`p306` firmware dump's `MsnCoreApp`** (663KB, unusually small,
  turned out to be an auth/launcher wrapper around `MsnCoreApp-original`
  -- noted in `msnapp/README.md`, not used for anything yet.

## Where to look for more detail

- `docs/historical/HANDOFF_nand_ecc_uboot_vs_kernel.md` -- bootnand hang,
  display investigation, all NAND ECC/BBT work (despite the filename).
- `docs/1.4_WIRELESS_AND_INIT.md` -- WiFi/Bluetooth hardware reference,
  the dual-USB-controller fix.
- `msnapp/README.md` -- every known `MsnCoreApp` binary variant, how to
  extract more from a UBI `rootfs.img`, version-string comparison
  method.
