# Prado Firmware Reconstruction

Reconstructed firmware for a Toyota Prado head unit running on the **Limcet Box P306** (ARK1680 SoC).

The Prado unit uses Holden firmware as its base but requires hardware-specific overrides for the display panel, product identity, and U-Boot environment. This repository tracks those overrides and the reconstruction process.

## Table of Contents

- [1.0 Hardware](#10-hardware)
- [2.0 Serial Console (recovery / monitoring)](#20-serial-console-recovery--monitoring)
- [3.0 Boot Sequence (stock NAND)](#30-boot-sequence-stock-nand)
- [4.0 U-Boot Prompt](#40-u-boot-prompt)
- [5.0 Booting from SD Card or USB (non-destructive)](#50-booting-from-sd-card-or-usb-non-destructive)
- [6.0 Build & Flash Tool](#60-build--flash-tool)
- [7.0 Flashing via SD Card](#70-flashing-via-sd-card)
- [8.0 Repository Structure](#80-repository-structure)
- [9.0 NAND Partition Layout](#90-nand-partition-layout)
- [10.0 Device Access](#100-device-access)
  - [WiFi Access Point](#wifi-access-point)
  - [SSH Access](#ssh-access)
  - [USB Networking](#usb-networking)
- [11.0 Holden Firmware Compatibility](#110-holden-firmware-compatibility)
- [12.0 Key Differences vs Holden Base Firmware](#120-key-differences-vs-holden-base-firmware)
- [13.0 Sources](#130-sources)

## 1.0 Hardware

| Item | Value |
|------|-------|
| SoC | ARK1680 (ARM Cortex-A5) |
| OS | Linux 3.4.0 / BusyBox |
| Bootloader | U-Boot 2012.10 |
| Product ID | Limcet-P306 |
| Resource | Box-P301 |
| Display | 800×480 RGB888 |
| Sound | None (SoundType=0) |
| MCU type | 6 |
| BT module | Feasycom (BlueToothType=6) |

### Limcet Board (DC_LIMCET_MB_REV_003)

The board running this firmware is a third-party **Limcet Box P306** aftermarket module, not a Toyota-made assembly — it's a piggyback module that ties into the existing factory head unit's harness and vehicle bus rather than replacing it outright.

| Component | Part | Role |
|-----------|------|------|
| SoC | ARK1668 (die marking; ARK1680 in firmware/software — same device) | Main applications processor |
| NAND | Toshiba TC58BVG0S3HTA00, 128 MB SLC | Firmware/rootfs storage, on a soldered daughter module (the "Limcet Box" compute module) |
| MCU | STM32F105RBT6 (ARM Cortex-M3) | Vehicle-side I/O — CAN bus, touch/button/reverse/ACC-IGN signals — talks to the ARK1668 over `/dev/ttyHS0` |
| CAN transceiver | NXP TJA1042 | Bridges the MCU's CAN controller to the vehicle CANH/CANL lines |
| BT/WiFi module | Feasycom FSC-BT8251 V1.1 (Realtek RF) | Bluetooth + WiFi AP, over `/dev/ttyHS1` |
| Rear camera decoder | RN6752 | CVBS composite → ITU-656 digital video for the reversing camera feed |
| Display adapter | DC_FUJITSU_CON96P_REV_002 (interposer) | Adapts the main board's edge connector to the LCD panel's 96-pin Fujitsu FPC |

**Connecting to the existing car wiring:**

- A multi-pin wiring harness (yellow/red/white connectors along the board's bottom edge, plus a red power connector) carries 12V ignition, battery, reverse-trigger, and audio lines from the vehicle into the piggyback board.
- Steering wheel controls are **not** read via an ADC voltage divider on a dedicated SWC wire, despite the `EnableSWCSwitchHardware` option in the ARK1668 config. The STM32F105 MCU instead decodes Toyota-specific messages directly off the vehicle's **CAN bus** (through the TJA1042 transceiver) and forwards translated key events to the ARK1668 over UART.
- The reversing camera connects as a standard CVBS composite feed, decoded by the RN6752 into digital video for the SoC.

Full teardown details and board photos are in `Limcet Hardware/BOARD_ANALYSIS.md` (linked below).

**Documentation:**

- [`Limcet Hardware/BOARD_ANALYSIS.md`](Limcet%20Hardware/BOARD_ANALYSIS.md) — board/component teardown (SoC, NAND, BT, MCU, CAN bus), with photos in the same folder.
- [`docs/SOURCES.md`](docs/SOURCES.md) — provenance of every file in this repo.
- [`docs/PARTITION_LAYOUT.md`](docs/PARTITION_LAYOUT.md) — NAND offsets, sizes, flash commands (see also [NAND Partition Layout](#90-nand-partition-layout) below).
- [`docs/SD_BOOT_PLAN.md`](docs/SD_BOOT_PLAN.md) — historical SD-boot planning doc, superseded by [Booting from SD Card or USB](#50-booting-from-sd-card-or-usb-non-destructive) below.

### Accessing the device

Four ways to reach the device:

| Method | Use for | Details |
|--------|---------|---------|
| Serial console (UART, 115200 8N1) | Recovery, monitoring, interrupting boot | [Serial console](#20-serial-console-recovery--monitoring) |
| SD/USB bootable image (non-destructive) | Testing changes without touching NAND | [Booting from SD Card or USB](#50-booting-from-sd-card-or-usb-non-destructive) |
| Network (WiFi AP / USB / SSH) | Once Linux has booted — only useful if the reconstructed, SSH-patched rootfs is already running (see caveat below) | [Device Access](#100-device-access) |
| SD update (flashes internal NAND) | Permanently updating firmware on the unit | [Flashing via SD Card](#70-flashing-via-sd-card) |

## 2.0 Serial Console (recovery / monitoring)

Connect via the UART header near the SD card slot. Settings: **115200 8N1**.

| Pin | Colour |
|-----|--------|
| TX | Yellow |
| RX | Blue |
| GND | Black |

Adapter: a generic USB-serial adapter (e.g. PL2303) or Raspberry Pi GPIO UART.

**Output vs input:** console output is available over serial through both U-Boot and the Linux kernel (boot log, `dmesg`, kernel messages). Keystroke input, however, only works at the U-Boot prompt — once Linux has booted, the serial line is view-only, with no login shell or interactive input on it.

**Connecting via USB-serial adapter (Windows / PuTTY):**

1. Plug in the USB-serial adapter (e.g. PL2303) and note its COM port from Device Manager (Ports (COM & LPT)) — install the adapter's driver first if it doesn't show up.
2. Open PuTTY, set **Connection type** to `Serial`.
3. **Serial line**: the COM port from step 1 (e.g. `COM5`).
4. **Speed**: `115200`.
5. Under Connection → Serial, confirm **Data bits** `8`, **Stop bits** `1`, **Parity** `None`, **Flow control** `None`.
6. Click **Open**.

**Connecting via Raspberry Pi:**
```
minicom -D /dev/ttyS0 -b 115200
```

### U-Boot Console

To interrupt U-Boot and drop to the prompt: hold the spacebar continuously from the moment power is applied and keep holding until you see the `ark#` prompt.

The stock Prado U-Boot has a **custom boot loop** — disassembly of the binary (`TEXT_BASE=0x00030000`, function at `0x0003cf3c`) confirms the mechanism:

```
env_get("bootdelay")   → r4 = 9   (from NAND env — value is real)
env_get("bootcmd")     → r5 = "run nandboot"
printf("Press space key to stop autoboot: %2d", r4)  ← 9 is cosmetic only
tstc()                 → ONE keypress poll, no sleep
  space held → readline("> ") loop  (ark# interactive shell)
  no key     → run_command("run nandboot")  ← immediate boot
```

`bootdelay=9` is read from the NAND env and printed in the message, but there is no countdown or sleep — the `%2d` is cosmetic. After the printf, there is one `tstc()` poll and then Linux boots immediately. You must already be holding space when that poll fires.

### Kernel Console

Once U-Boot hands off, the same UART carries the Linux kernel's boot log and `dmesg` output — enabled via the `console=ttyS0,115200n8` kernel bootarg (see [Boot Sequence](#30-boot-sequence-stock-nand) below). This is receive-only, as noted above: there is no login shell or interactive input on this console once Linux is running. It's still the fastest way to see what's happening early in boot — e.g. checking which WiFi driver bound to `wlan0` (see [WiFi module detection](#wifi-module-detection)) or diagnosing a hang before userspace and SSH come up.

On SD/USB boot, `console=tty0` is also set, so the same messages are mirrored to the physical screen via `fbcon` — see [Console on screen](#console-on-screen).

## 3.0 Boot Sequence (stock NAND)

1. **S-Loader (Nboot / Stepldr)** — executes from ROM; checks the SD card FAT32 partition (p1) for a `UBOOT.BIN` file and loads that in preference if present, otherwise loads U-Boot from NAND `0x020000` (see [Booting from SD Card or USB](#50-booting-from-sd-card-or-usb-non-destructive))
2. **U-Boot** — initialises hardware; loads NAND env from `0x120000` (CRC valid — `bootdelay=9`, `bootcmd=run nandboot`, `nandboot`, `setbootargs` etc. all active)
3. **SD update check** — inspects the SD card FAT32 partition for `UpConfig`; if present, runs `arkupdate` to flash partitions listed in the `update` script
4. **Single keypress poll** — prints `Press space key to stop autoboot:  9`, then one `tstc()` check with no delay; if spacebar already held, drops to `ark#` interactive shell; otherwise boots immediately
5. **`run nandboot`** — executes `nandboot` from NAND env: `run setbootargs; bootnand`
   - `setbootargs` → `setenv bootargs console=ttyS0,115200n8 mem=180M ubi.mtd=6 root=ubi0:rootfs rootfstype=ubifs rootwait ro`
   - `bootnand` → custom compiled-in command: `nand read 0x1000000 <kernel_offset> <kernel_size>; bootz 0x1000000`
6. **Linux 3.4.0** starts

## 4.0 U-Boot Prompt

Holding the interrupt key (see [Boot Sequence](#30-boot-sequence-stock-nand) above) drops you into `ark#`, U-Boot's interactive shell.

### Boot commands

Selected commands relevant to this device — type `help` at the `ark#` prompt for the full command list built into this U-Boot.

| Command | Effect |
|---------|--------|
| `run nandboot` | Boots the stock NAND kernel/rootfs — `run setbootargs; bootnand` (default `bootcmd` on stock NAND, see [Boot Sequence](#30-boot-sequence-stock-nand)) |
| ~~`run sdboot`~~ | ~~Boots kernel/rootfs from the SD card p1/p2 (`uboot_final.bin` only) — see SD boot~~ **Only exists on a patched U-Boot — the patched binaries this project shipped are corrupted, see below. Use [Manual SD Card Boot](#manual-sd-card-boot) instead.** |
| ~~`run usbboot`~~ | ~~Boots kernel/rootfs from a USB drive (`uboot_final.bin` only) — see USB boot~~ **Same corrupted-binary caveat as `run sdboot` above.** |
| `usb start` | Initialises the USB host controller — run this first to confirm USB works before `run usbboot` |

### Manual SD Card Boot

`run sdboot` above only exists on a patched U-Boot — and the patched binaries this project previously produced (`uboot_sdboot.bin`, `uboot_final.bin`) turned out to be corrupted (see [Booting from SD Card or USB](#50-booting-from-sd-card-or-usb-non-destructive) below and [`docs/UBOOT_SDBOOT_INVESTIGATION.md`](docs/UBOOT_SDBOOT_INVESTIGATION.md) for the full writeup). Until a genuinely safe patched U-Boot exists, boot a kernel and rootfs from SD manually instead — no patching, no corruption risk, works today on the stock U-Boot already on the device.

At the `ark#` prompt, with an SD card containing `zImage` on a FAT32 partition (p1) and an ext4 rootfs on a second partition (p2):

```
setenv bootargs console=ttyS0,115200n8 console=tty0 mem=180M root=/dev/mmcblk0p2 rootfstype=ext4 rootwait rw
fatload mmc ${mmcdev}:1 ${loadaddr} ${bootfile}
bootz ${loadaddr}
```

`${mmcdev}`, `${loadaddr}`, and `${bootfile}` are already defined in the device's real NAND env (`mmcdev=1`, `loadaddr=0x1000000`, `bootfile=zImage` — see `env/uboot-env.txt`), so nothing needs to be `setenv`'d for those. This has to be re-typed every boot — it isn't saved to NAND env (no `saveenv` is run), so the stock boot behavior is unaffected.

### Manual partition flash

At the `ark#` prompt you can manually flash any partition — this example loads a kernel image from the SD card and writes it to the kernel partition (see [NAND Partition Layout](#90-nand-partition-layout) for the offsets of other partitions):

```
fatload mmc 0 4000000 zImage
nand scrub 0x1a0000 0x400000 0x1a0000 0x400000
nand write 0x4000000 0x1a0000 ${filesize}
```

## 5.0 Booting from SD Card or USB (non-destructive)

> **⚠ The patched-U-Boot approach below is known corrupted — do not use `uboot_sdboot.bin`/`uboot_final.bin`.** Struck-through text is kept for historical record only. Use [Manual SD Card Boot](#manual-sd-card-boot) (section 4.0) instead — it needs no patched U-Boot at all. Full investigation: [`docs/UBOOT_SDBOOT_INVESTIGATION.md`](docs/UBOOT_SDBOOT_INVESTIGATION.md); the corrupted files themselves are quarantined under [`corrupted/`](corrupted/README.md).

Reflashing NAND for every kernel or rootfs change (see [Flashing via SD Card](#70-flashing-via-sd-card)) is slow. A bad image can also leave the device unbootable, with only a single-keypress recovery window (see [Boot Sequence](#30-boot-sequence-stock-nand)).

~~`uboot_final.bin` avoids that risk. It's a patched U-Boot that boots a kernel and rootfs from removable media, **without writing to NAND** — the NAND-resident firmware stays untouched, so there's always a known-good fallback.~~

~~`patch_uboot.py` produces it by directly editing the compiled `uboot_sdboot.bin` binary (believed to be ARK1680 BSP source-compiled U-Boot) — no source recompilation involved.~~ **This was wrong — `uboot_sdboot.bin` turned out to be Holden's stock `uboot.bin` with its command table wiped by a (since-fixed) `patch_uboot.py` bug, not a genuine BSP-compiled binary. See the investigation doc linked above.**

~~Two patches are applied:~~
~~1. **Compiled-in env** — sdboot and usbboot commands baked in as fallback defaults~~
~~2. **NAND env redirect** — the three `MOV Rx, #0x120000` ARM instructions that load `CONFIG_ENV_OFFSET` are changed to `MOV Rx, #0xFF000000`, forcing the NAND env CRC to fail so the compiled-in defaults take effect~~

~~> **Note:** `uboot_final.bin` replaces steps 2–5 of the stock [Boot Sequence](#30-boot-sequence-stock-nand) with standard U-Boot autoboot. `bootdelay=3` is baked into the compiled-in env and the NAND env is intentionally failed, giving a proper 3-second countdown where any key tap works.~~

### ~~Rebuild `uboot_final.bin`~~

```bash
# DO NOT RUN — reproduces a corrupted binary, see warning above.
# corrupted/uboot_sdboot.bin is quarantined, not a valid patch source.
python3 patch_uboot.py -i corrupted/uboot_sdboot.bin -o uboot_final.bin --mode sdboot --patch-nand-offset
```

~~> **Note:** this is a separate, manual `patch_uboot.py` invocation from the one `build_bootable_sdcard.sh` runs internally — that script patches its own auto-detected U-Boot source and writes its output to `sd_bootable/uboot_sdboot.bin`, never touching the repo-root `uboot_sdboot.bin` used here.~~

~~Place `uboot_final.bin` as `UBOOT.BIN` on the SD card FAT32 partition (p1). Stepldr loads it in preference to the NAND copy.~~

### ~~SD boot~~ (compiled-in autoboot — not currently usable)

~~The compiled-in env boots automatically from the SD card:~~

| Variable | Value |
|----------|-------|
| ~~`bootcmd`~~ | ~~`run sdboot`~~ |
| ~~`bootdelay`~~ | ~~3 seconds (interrupt with any key)~~ |
| ~~`bootfile`~~ | ~~`zImage`~~ |
| ~~`mmcdev`~~ | ~~`1` (SD slot)~~ |
| ~~`sdboot`~~ | ~~`run sdbootargs; fatload mmc ${mmcdev}:1 ${loadaddr} ${bootfile}; bootz ${loadaddr}`~~ |
| ~~`sdbootargs`~~ | ~~`console=ttyS0,115200n8 console=tty0 mem=180M root=/dev/mmcblk0p2 rootfstype=ext4 rootwait rw`~~ |

For the [Manual SD Card Boot](#manual-sd-card-boot) workflow, the equivalent `bootargs` value above is typed by hand each boot — see section 4.0.

**SD card layout:**

| Partition | Filesystem | Contents |
|-----------|-----------|---------|
| p1 | FAT32 | ~~`UBOOT.BIN`,~~ `zImage` (`UBOOT.BIN` is only needed for the currently-unusable patched-autoboot method above — not needed for [Manual SD Card Boot](#manual-sd-card-boot)) |
| p2 | ext4 | rootfs (`/`), plus `/nanddata/` — see below |
| p3 | ext4 | userdata (`/data`) |

### Building the SD image with `build_bootable_sdcard.sh`

`build_bootable_sdcard.sh` assembles the full bootable SD image (or writes directly to a block device): patches its own auto-detected U-Boot source via `patch_uboot.py`, partitions and formats p1/p2/p3, syncs the rootfs and userdata trees, patches `rcS` on the p2 copy only (the source tree is never modified), and populates `/nanddata/` (see below). Output lands in `sd_bootable/` (gitignored).

> **U-Boot patching is currently non-functional** (see the warning at the top of this section) — the "Patch U-Boot for SD boot" toggle below will either safely refuse (raw NAND-dumped `uboot.bin`, no reserved env space) or, if given `--uboot`/a prebuilt binary, has no genuinely working source to patch. The rootfs/userdata/`/nanddata/` portions of this tool are unaffected and still useful — build with `--uboot PATH` pointing at any placeholder binary (or toggle U-Boot patching off) and rely on [Manual SD Card Boot](#manual-sd-card-boot) (section 4.0) instead of the SD card's `UBOOT.BIN`.

It uses the same interactive menu as `build_update.sh` — arrow keys move the highlighted row, Space/Enter toggles it, `a`/`n` select/deselect all, `g` builds, `q` quits:

```bash
sudo bash build_bootable_sdcard.sh
```

```
  ARK1680 Prado — Bootable SD Card Builder
  ────────────────────────────────────────────────────────
  BUILD OPTIONS
  ▶ [X]  Patch U-Boot for SD boot
    [X]  Patch NAND env offset redirect
    [X]  Include userdata (p3)
    [X]  Redirect bootlogo/bootanimation/etc to SD

  SD IMAGE CONTENTS  → sd_bootable/sd_boot.img
       Part Item                   File             Status
       p1   U-Boot                 uboot.bin        found
       p1   Kernel                 zImage           found
       p2   Rootfs                 rootfs           found
       p3   Userdata               userdata         found
  ────────────────────────────────────────────────────────
```

U-Boot, Kernel, and Rootfs aren't independently toggleable — they're required for a bootable image, and the build refuses to proceed if any is missing. Only the four **BUILD OPTIONS** are selectable; Userdata's inclusion follows the "Include userdata (p3)" toggle above.

"Redirect bootlogo/bootanimation/etc to SD" controls the `/nanddata/` symlink patch described [below](#runtime-nand-partition-data-nanddata): on (default) symlinks mtd8–11 to files on the SD card, off leaves `/dev/mtd8`–`/dev/mtd11` reading whatever's already in NAND — useful when testing a change without needing real dumps of those partitions.

Paths and sizes stay CLI-flag-only (the menu doesn't do free-text editing): `--image PATH` / `--device PATH` (output target, default `sd_bootable/sd_boot.img`), `--size MB` (default 512), `--uboot PATH` (use a prebuilt `UBOOT.BIN` as-is, skip patching), `--uboot-src` / `--kernel` / `--rootfs-dir` / `--userdata-dir` (override auto-detected paths), `--root DEVICE` (rootfs device for bootargs, default `/dev/mmcblk0p2`), `--no-mtd-redirect` (equivalent to toggling the fourth option off), `--non-interactive` (skip the menu, use flags/autodetected values as-is — needed for `--device`), `--dry-run`. Run with `--help` for the full list.

Requirements — `parted`, `mkfs.fat` (dosfstools), `mkfs.ext4` (e2fsprogs), `losetup` (util-linux), `rsync`, `python3` — are checked at startup, same as `build_update.sh`'s requirements check (plus `mtd-utils` if also building rootfs/userdata images via [Build & Flash Tool](#60-build--flash-tool)).

### `/data` mount on SD boot

The rootfs `rcS` copy on p2 is patched to try SD userdata first, falling back to the original NAND paths so the same rootfs image still boots correctly from NAND:

1. Try `mount -t ext4 /dev/mmcblk0p3 /data` (SD userdata)
2. Fall back to NAND UBIFS (`ubiattach` mtd7), then NAND yaffs2, matching the original stock `rcS` logic
3. `factory_reset=1` reformats whichever `/data` is currently active (SD ext4 or NAND UBI) and clears the flag

### Runtime NAND partition data (`/nanddata/`)

Four MTD partitions (bootlogo, bootanimation, reversingtrack, Unicode) are read by the running app via `/dev/mtdN` character devices but are not part of the rootfs or userdata images. A search of all rootfs binaries for MTD ioctls (`MEMGETINFO`, `MEMERASE`) found none of them are called on these partitions — access is plain `open()`/`read()` — so a regular file works as a transparent replacement for the character device.

`build_bootable_sdcard.sh` copies these into `/nanddata/` on p2 when the "Redirect bootlogo/bootanimation/etc to SD" build option is on (default; `--no-mtd-redirect` or toggling it off in the menu leaves `/dev/mtdN` reading whatever's already in NAND instead, and skips populating `/nanddata/` entirely):

| MTD | Partition | Source file | Status |
|-----|-----------|--------------|--------|
| 8 | bootlogo | `mtd8_bootlogo/bootlogo` | Dumped (31 KB used of 512 KB) |
| 9 | bootanimation | `mtd9_bootanimation/bootanimation` | Placeholder — erased during Holden flash, no dump yet |
| 10 | reversingtrack | `mtd10_reversingtrack/reversingtrack` | Dumped (1.2 MB, RSTK format — see below) |
| 11 | Unicode | `mtd11_unicode/unicode` | Placeholder — no dump yet |

The patched `rcS` replaces each `/dev/mtdN` node unconditionally after `mdev -s`:

```sh
for mtdmap in "8:bootlogo" "9:bootanimation" "10:reversingtrack" "11:unicode"; do
    num="${mtdmap%%:*}"; name="${mtdmap##*:}"
    rm -f /dev/mtd${num}
    ln -sf /nanddata/${name} /dev/mtd${num}
done
```

To replace a placeholder once a real dump is obtained (via serial console: `dd if=/dev/mtd9 of=/tmp/bootanimation`), drop the file into the matching `mtd*_*/` folder under `Prado firmware reconstructed/` and rebuild the image.

**RSTK format (reversingtrack):** the reversing-camera guide-line overlay file is a custom container — 4-byte magic `"RSTK"`, file size, entry count (41), steering-position count (100), image dimensions (800×480), guide-line zone parameters, then a 41 × 20-byte index table (`index, index, file_offset, compressed_size, flag`) followed by 41 zlib-compressed overlay images. The 41 frames span full-left to full-right steering angles; frame sizes form a bell curve (31 KB at the extremes, 17 KB at centre) consistent with symmetric guide-line geometry compressing better near centre. The app decompresses the frame matching the current steering angle and composites it over the camera feed.

### USB boot

USB mass storage is compiled in (MUSB HCD). **Unverified on Prado hardware** — run `usb start` at the U-Boot prompt to confirm the host controller and GPIO assignments work on your unit before relying on this.

~~From the U-Boot prompt, boot from a USB drive with one command:~~

```
# DO NOT RUN — `usbboot` only exists on a patched U-Boot, which is currently
# corrupted (see the warning under section 5.0 above). Use the manual
# sequence below instead — it works on the stock, unpatched U-Boot.
run usbboot
```

Manually (works today, no patched U-Boot needed — same pattern as [Manual SD Card Boot](#manual-sd-card-boot), section 4.0):

```
usb start
fatload usb 0:1 0x1000000 zImage
setenv bootargs "console=ttyS0,115200n8 console=tty0 mem=180M root=/dev/sda2 rootfstype=ext4 rootwait rw"
bootz 0x1000000
```

~~The `usbboot` and `usbbootargs` env variables are baked into `uboot_final.bin` compiled-in env:~~

| Variable | Value |
|----------|-------|
| ~~`usbboot`~~ | ~~`usb start; fatload usb 0:1 ${loadaddr} ${bootfile}; run usbbootargs; bootz ${loadaddr}`~~ |
| ~~`usbbootargs`~~ | ~~`console=ttyS0,115200n8 console=tty0 mem=180M root=/dev/sda2 rootfstype=ext4 rootwait rw`~~ |

**USB drive layout:**

| Partition | Filesystem | Contents |
|-----------|-----------|---------|
| p1 | FAT32 | `zImage` |
| p2 | ext4 | rootfs |

The kernel sees the USB drive as `/dev/sda`. The ARK1668 uses MUSB (not EHCI) — USB 2.0 drives work; USB 3.0 drives that require USB 3.0 speeds will not enumerate.

### Console on screen

Both the [Manual SD Card Boot](#manual-sd-card-boot) and manual USB boot `bootargs` above include `console=tty0`. Once the kernel initialises the LCD framebuffer (`CONFIG_FB_ARK1668LCD`), boot messages and a login prompt are mirrored to the screen via `fbcon`. The U-Boot phase itself is serial-only (no video console compiled into U-Boot).

## 6.0 Build & Flash Tool

`build_update.sh` is an interactive terminal tool that combines building firmware images and generating a NAND flash update package staged on an SD card into a single workflow. This flashes internal NAND — it is **not** the non-destructive SD-boot image described in [Booting from SD Card or USB](#50-booting-from-sd-card-or-usb-non-destructive) (that's `build_bootable_sdcard.sh`). Run it under Linux or WSL:

```bash
bash build_update.sh
```

### Menu layout

The whole menu is one line per item — no per-item description text — so it fits a standard ~24-line terminal without scrolling. Full detail for whichever row is highlighted is shown once, on the detail line just above the command bar, instead of repeated for every row:

```
  ARK1680 Prado — Build & Flash Tool
  ────────────────────────────────────────────────────────
  BUILD
    [ ]  Build rootfs image         no image yet
    [ ]  Build userdata image       no image yet
  ▶ [ ]  Build U-Boot env image     no image yet

  NAND PARTITIONS  (staged on SD, flashed to internal NAND on boot)
         MTD  Partition              File
    [ ]  1-2  U-Boot                 uboot.bin        found ⚠
    [ ]  3    U-Boot Env             uboot-env.bin    missing - build first
    [ ]  4    Display Config         arkdata.ini      found
    [ ]  5    Linux Kernel           zImage           found
    [X]  6    Root Filesystem        rootfs.img       missing - build first
    [X]  7    User Data              userdata.img     missing - build first
    [ ]  8    Boot Logo              bootlogo         found
    [ ]  9    Boot Animation         bootanimation    found
    [ ]  10   Reversing Track        reversingtrack   found
    [ ]  11   Unicode Font           unicode          found
  ────────────────────────────────────────────────────────
  Build U-Boot env image: Compiles env/uboot-env.txt into uboot-env.bin (256 KB, mkenvimage)
  ↑/↓ move   Space/Enter toggle   a/n all/none   g go   q quit
```

Rows are listed in MTD numerical order (U-Boot spans mtd1 and mtd2, since the same binary is written to both the primary and backup slots).

The `▶` marker shows which row is highlighted — move it with the arrow keys; the line above the command bar always shows the description, offset, and size for that row. `rootfs.img`, `userdata.img`, and `uboot-env.bin` all have a corresponding build step above, so their missing-status hints to build first instead of just saying "missing".

**Defaults:** rootfs and userdata are selected by default. Kernel, U-Boot, arkdata, U-Boot Env, and other early-boot partitions default to off — they must be explicitly enabled to avoid accidental reflash.

### Output

Generated files land in `sd_update/output/`. Copy all files to the root of a FAT32 SD card to flash.

### Standalone scripts

The individual scripts are retained for use without the interactive menu:

| Script | Purpose |
|--------|---------|
| `build_rootfs.sh` | Build rootfs UBI image only |
| `build_userdata.sh` | Build userdata UBI image only |
| `legacy/generate_update.sh` | Legacy — generate SD package for selected partitions without the build steps |

### Requirements

Build steps require `mkfs.ubifs`, `ubinize` (rootfs/userdata), and `mkenvimage` (U-Boot env):

```bash
sudo apt install mtd-utils u-boot-tools   # Debian / Ubuntu / WSL
```

`build_update.sh` checks for all three on startup and prints their status before showing the menu. Missing tools only block the build steps that need them — you can still select partitions and generate the SD package without them.

`mtd-utils`, `u-boot-tools`, `parted`, `dosfstools`, `e2fsprogs`, and `util-linux` all install their binaries to `/usr/sbin` or `/sbin`, which isn't always on `$PATH` for non-root shells (WSL, non-login shells). `build_update.sh`, `build_bootable_sdcard.sh`, `build_rootfs.sh`, and `build_userdata.sh` all add `/usr/sbin:/sbin` to `$PATH` themselves, so this should be transparent — but if you see "not found" for a tool `dpkg -l` shows as installed, check `which mkfs.ubifs` / `which ubinize` / `which mkenvimage` / `which parted` / `which mkfs.fat` / `which mkfs.ext4` / `which losetup` for the actual path.

## 7.0 Flashing via SD Card

On power-on, U-Boot checks for a FAT32 SD card. If a file named `UpConfig` is present in the SD root, U-Boot runs `arkupdate`, which reads the `update` script and flashes each listed partition in sequence. After completion the unit reboots — remove the SD card so it doesn't re-flash. See [NAND Partition Layout](#90-nand-partition-layout) below for offsets and sizes.

### Quick start

**Step 1 — Build firmware images**

| Image | Command | Notes |
|-------|---------|-------|
| `rootfs.img` | `bash build_rootfs.sh` (Linux/WSL) | ~106 MB UBI image |
| `userdata.img` | `bash build_userdata.sh` (Linux/WSL) | ~6 MB UBI image |

**Step 2 — Generate the update script**

```bash
bash legacy/generate_update.sh
```

Toggle partitions with number keys, press `g` to generate. Output lands in `sd_update/output/`.

> Steps 1–2 can be replaced entirely by running `bash build_update.sh`, which does the build and package generation in one interactive session — see [Build & Flash Tool](#60-build--flash-tool) above.

**Step 3 — Prepare SD card**

Format as FAT32 (max 32 GB). Copy everything from `sd_update/output/` to the SD root.

**Step 4 — Flash**

1. Power off the head unit
2. Insert the SD card
3. Power on — update progress shown on screen
4. Wait for automatic reboot (do **not** interrupt power)
5. Remove the SD card

### Manual update script

The `update` file is a plain list of partition keywords, one per line — **not** raw U-Boot commands. `arkupdate` has the NAND offsets and sizes for each keyword compiled in; the SD file never states them:

```
uboot
bootlogo
kernel
filesystem
userdata
arkdata
reversingtrack
bootanimation
```

Confirmed against the reference packages (`Holden firmware update/update`, `Prado firmware recovery holden based/update`, `sd_update/update.example` — all identical) and cross-checked against the literal `"*****Now update <name> ......"` strings compiled into `uboot.bin`. Note `filesystem` is the keyword for the rootfs partition, and `kernel` expects a file named `zImage` on the SD card, not `kernel.img` or similar — filenames must match exactly what's shown in the [NAND Partition Layout](#90-nand-partition-layout) table below.

`uboot-env` and `unicode` are deliberately left out of `build_update.sh`'s generated `update` file — neither is an independent arkupdate keyword:

- **U-Boot Env — confirmed on real hardware**: it's only flashed as a side effect of updating `uboot` itself, not addressable on its own. Matches the different compiled-in message format (`"Update U-boot-Env ......"` vs `"*****Now update X ......"` for everything above) — it's a sub-step of the uboot routine, not its own top-level keyword. Selecting U-Boot Env without also selecting U-Boot is a no-op on the device; `build_update.sh` warns about this both ways (env selected without uboot, or uboot selected without env — the latter because flashing uboot touches env regardless, so what state it ends up in without a known-good `uboot-env.bin` alongside it isn't confirmed).
- **Unicode** — mechanism still unconfirmed; no reference package includes it either.

Both stay flashable manually from the U-Boot prompt instead — see [4.0 U-Boot Prompt](#40-u-boot-prompt) above.

### Safety notes

- **Never flash S-Loader (Nboot) via SD** — corruption bricks the board (requires JTAG to recover)
- **U-Boot** writes to both primary (`0x20000`) and backup (`0xA0000`) slots with the same binary, and also touches U-Boot Env as a side effect — see above
- **userdata flash** erases all paired BT devices, call history, and user settings — recreated on first boot
- **rootfs flash** replaces the entire filesystem; bad block at 0x5FA0000 is handled automatically

## 8.0 Repository Structure

```
Prado firmware dump/                  Raw MTD partition dumps from the live device
  mtd1-mtd2_uboot/           U-Boot binaries (raw + extracted)
  mtd3_env/                  U-Boot environment (raw + extracted)
  mtd4_arkdata/              Panel/hardware config (raw + extracted)
  mtd5_kernel/               Kernel zImage (raw + extracted)
  mtd6_rootfs/               Root filesystem UBIFS dump (raw)
  mtd6_rootfs_raw/           Raw MTD6 bin (Git LFS)

Prado firmware reconstructed/         Reconstructed firmware for flashing
  mtd0_sloader/              Nboot.bin, Stepldr.bin
  mtd1-mtd2_uboot/           uboot.bin
  mtd3_env/                  (placeholder — reconstructed env lives in env/uboot-env.txt instead)
  mtd4_arkdata/              arkdata.ini (Prado panel config — copy of display/arkdata.ini)
  mtd5_kernel/               zImage (reconstructed kernel — see note on top-level kernel/ below)
  mtd6_rootfs/
    rootfs/                  Modified rootfs tree (Prado libs + SSH + WiFi AP)
  mtd7_userdata/
    userdata/                Userdata tree (Prado settings overlay)
  mtd8_bootlogo/             bootlogo
  mtd9_bootanimation/        (placeholder — no content yet)
  mtd10_reversingtrack/      reversingtrack
  mtd11_unicode/             unicode (placeholder — no content yet)

Holden firmware update/               Stock Holden update package (reference — validated it boots on Prado hw)
Prado firmware recovery holden based/ Stock Holden package repackaged with Prado msn_factory_configs, for recovery

Limcet Hardware/
  BOARD_ANALYSIS.md         Board/component teardown notes (SoC, NAND, BT, MCU, CAN bus)
  *.jpg                     Board photos referenced from BOARD_ANALYSIS.md

ui/                Qt 4.7.4 UI analysis and resource extraction — see ui/UI.md
  UI.md                      Qt module layout, key binaries, /msnprofile/ filesystem layout
  qm_extracted/              Decompiled translation strings (lang_en.txt, lang_arabic.txt, ...)
  rcc_extracted/             Decompiled Qt resource bundles, one dir per screen/resolution
  tools/
    extract_qm.py            Decompiles .qm translation files to text
    extract_rcc.py           Decompiles .rcc resource bundles

kernel/            zImage (from Holden base — identical kernel_size to Prado firmware dump; gitignored, not present in every checkout — Prado firmware reconstructed/mtd5_kernel/zImage is the copy actually used for builds)
display/
  arkdata.ini                Prado panel config (from MTD4 live dump) — build source for mtd4
  mtd4_arkdata_prado_dump.bin  Raw MTD4 dump the .ini was derived from
  arkdata_holden.ini         Holden standard reference
  arkdata_holden_0324.ini    Holden March 2024 update reference
msn_factory_configs/
  FactoryConfig.ini          Prado identity + Holden firmware settings
  MsnProductInfo.ini         Hardware identity (Limcet-P306)
env/
  uboot-env.txt              Reconstructed env (bootdelay=9, 106m/6m layout)
  mtd3_env_prado_firmware_dump.bin    Raw env from live device (gitignored)
sd_update/
  UpConfig                   SD update trigger file
  update.example             Static reference script (generated version goes to output/)
  output/                    Generated SD card package (gitignored)
docs/
  SOURCES.md                 Where each file came from and why
  PARTITION_LAYOUT.md        NAND offsets, sizes, flash commands
  SD_BOOT_PLAN.md            Historical SD-boot planning doc — superseded, see below
build_update.sh              Combined interactive build and flash tool
build_rootfs.sh              Standalone rootfs UBI image builder
build_userdata.sh            Standalone userdata UBI image builder
patch_uboot.py               Patches compiled-in env and NAND offset in a U-Boot binary
build_bootable_sdcard.sh     Interactive bootable SD card image builder (same arrow-key menu as build_update.sh)
sd_bootable/                 Generated bootable SD image output (gitignored — sd_boot.img + patched uboot_sdboot.bin)
corrupted/                   Known-corrupted uboot_sdboot.bin/uboot_final.bin — do not use, see corrupted/README.md
legacy/
  generate_update.sh         Superseded by build_update.sh — standalone partition-selection + SD-package
                              generator only, no build steps; kept for standalone use
```

## 9.0 NAND Partition Layout

| Partition | Start | Size | Contents |
|-----------|-------|------|----------|
| S-Loader | `0x000000` | 128 KB | Nboot (do NOT update via SD) |
| U-Boot | `0x020000` | 512 KB | 2nd-stage bootloader |
| U-Boot_back | `0x0A0000` | 512 KB | U-Boot backup slot |
| U-Boot-Env | `0x120000` | 256 KB | U-Boot environment variables |
| arkdata | `0x160000` | 256 KB | Display / TvoutType config |
| kernel | `0x1A0000` | 4 MB | Linux 3.4.0 zImage |
| rootfs | `0x5A0000` | 106 MB | Root filesystem (UBIFS/UBI) |
| userdata | `0x6FA0000` | 6 MB | User settings / BT pairs (UBI) |
| bootlogo | `0x75A0000` | 512 KB | Boot splash screen |
| bootanimation | `0x7620000` | 3 MB | Boot animation |
| reversingtrack | `0x7920000` | 3 MB | Reversing camera audio |
| Unicode | `0x7C20000` | 256 KB | Unicode font data |

**Known bad block at 0x5FA0000** — inside the rootfs partition. `nand scrub` handles this automatically.

See also [`docs/PARTITION_LAYOUT.md`](docs/PARTITION_LAYOUT.md) for the same data plus flash commands.

## 10.0 Device Access

### WiFi Access Point

A WPA2 access point starts automatically on boot via `/etc/wifi_ap.sh`, providing network access for SSH without needing a physical connection.

> **Note:** joining `carplay_wifi` only gets you an IP address on `192.168.43.0/24` — the AP itself doesn't expose any open ports. It's only useful for reaching SSH once the [SSH](#ssh-access)-patched reconstructed rootfs is the one actually running on the device; on stock/Holden firmware (or before flashing) there's nothing listening at `192.168.43.1`.

| Item | Value |
|------|-------|
| SSID | `carplay_wifi` |
| Password | `88888888` |
| AP IP | `192.168.43.1` |
| DHCP range | `192.168.43.20 – 192.168.43.254` |
| Config | `/etc/hostapd/hostapd.conf`, `/etc/udhcpd.conf` |

**To connect:**

```sh
# Connect to carplay_wifi (WPA2, password: 88888888)
ssh root@192.168.43.1
```

#### WiFi module detection

Five Realtek drivers are bundled in `/lib/modules/3.4.0/`. At early boot, `wifi_ap.sh` probes them in order until one loads successfully:

| Priority | Module | Chip | Interface |
|----------|--------|------|-----------|
| 1 | `wlan_rtl8821cs.ko` | RTL8821CS | SDIO (most likely — Feasycom BT+WiFi combo) |
| 2 | `wlan_rtl8822cs.ko` | RTL8822CS | SDIO |
| 3 | `wlan_rtl8189fs.ko` | RTL8189FS | SDIO |
| 4 | `wlan_rtl8821cu.ko` | RTL8821CU | USB |
| 5 | `wlan_rtl8811cu.ko` | RTL8811CU | USB |

If the main app (`MsnCoreApp`) has already placed the correct driver at `/tmp/wlan.ko`, that is used instead. If `wlan0` does not come up, check `dmesg` on the serial console to identify which chip is present and adjust the probe order in `wifi_ap.sh`.

### SSH Access

SSH is enabled in the reconstructed rootfs and starts automatically on boot.

| Item | Value |
|------|-------|
| Binary | `/usr/bin/sshd` (OpenSSH 4.6p1) |
| Config | `/etc/ssh/sshd_config` |
| Host keys | `/etc/ssh/ssh_host_rsa_key` (RSA 2048), `/etc/ssh/ssh_host_dsa_key` |
| Login | `root` with existing password hash from `/etc/shadow` |

**To connect:**

```sh
ssh root@192.168.7.1
```

### USB Networking

The ARK1680 USB gadget stack is configured to use CDC-NCM (`g_ncm.ko`), which creates a `usb0` network interface when connected to a host PC.

| Item | Value |
|------|-------|
| Device IP | `192.168.7.1` |
| Subnet | `255.255.255.0` |
| Set PC address to | `192.168.7.2` (static) |

**Platform notes:**
- **macOS / Linux** — CDC-NCM supported natively; interface appears automatically
- **Windows** — may require the CDC-NCM host driver from Windows Update

`g_zero.ko` has been removed from `Prado firmware reconstructed/mtd6_rootfs/rootfs/etc/all.sh` — it was overriding the NCM gadget registration and breaking both USB host mode and the network interface.

## 11.0 Holden Firmware Compatibility

The Holden update package (`HOLDEN_KS_Auto_DSP(BT)_0219`) has been confirmed to boot successfully on the Prado device. This validates that the Holden firmware is a compatible base — the SoC, bootloader, and kernel are interoperable.

Known glitches when running stock Holden firmware on the Prado hardware:

- **Screen hue** — caused by mismatched LCD timings in `arkdata.ini` (`CLKDIV1=10`, `VBP=3`, `HBP=20` vs Prado values). Fixed by flashing the Prado `arkdata.ini` to mtd4.
- **Touch keys** — Holden `arkdata.ini` defines 5 side touch keys that do not exist on the Prado hardware.
- **Product identity** — `FactoryConfig.ini` and `MsnProductInfo.ini` reference Holden-specific IDs (`Ksmart_DSP`, `Box-C211`, `McuType=16`).

These are all corrected in the reconstructed firmware. See [Key Differences vs Holden Base Firmware](#120-key-differences-vs-holden-base-firmware) below.

## 12.0 Key Differences vs Holden Base Firmware

| Item | Holden | Prado |
|------|--------|-------|
| ProductId | Ksmart_DSP | **Limcet-P306** |
| ResourceName | Box-C211 | **Box-P301** |
| McuType | 16 | **6** |
| SoundType | 4 (DSP) | **0** |
| Panel timing (CLKDIV1) | 10 | **11** |
| Panel VBP/HBP | 3/20 | **29/32** |
| Touch keys | 5 | **none** |
| bootdelay | 0 | **9** |
| BT device name | Ksmart | **Limcet Box** |
| BT pair code | 0000 | **8362** |
| Vehicle branding | HOLDEN | **TOYOTA** |

## 13.0 Sources

See [`docs/SOURCES.md`](docs/SOURCES.md) for full provenance of each file.
