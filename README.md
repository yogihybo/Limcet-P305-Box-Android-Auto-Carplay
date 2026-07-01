# Prado Firmware Reconstruction

Reconstructed firmware for a Toyota Prado head unit running on the **Limcet Box P306** (ARK1680 SoC).

The Prado unit uses Holden firmware as its base but requires hardware-specific overrides for the display panel, product identity, and U-Boot environment. This repository tracks those overrides and the reconstruction process.

## Hardware

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

## Repository Structure

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
build.sh                     Combined interactive build and flash tool
build_rootfs.sh              Standalone rootfs UBI image builder
build_userdata.sh            Standalone userdata UBI image builder
generate_update.sh           Standalone SD card update script generator
patch_uboot.py               Patches compiled-in env and NAND offset in a U-Boot binary
build_bootable_sdcard.sh     Builds a bootable SD card image (uboot_final.bin + kernel + rootfs + userdata)
uboot_sdboot.bin             Input binary for patch_uboot.py (ARK1680 BSP source-compiled U-Boot)
uboot_final.bin              Patched U-Boot binary — place as UBOOT.BIN on SD p1 FAT32
sd_boot.img                  Generated bootable SD image (gitignored — output of build_bootable_sdcard.sh)
```

> `lzop_1.04-2_amd64.deb` at the repo root is a stray downloaded package, not referenced by any build step — safe to delete.

## Holden Firmware Compatibility

The Holden update package (`HOLDEN_KS_Auto_DSP(BT)_0219`) has been confirmed to boot successfully on the Prado device. This validates that the Holden firmware is a compatible base — the SoC, bootloader, and kernel are interoperable.

Known glitches when running stock Holden firmware on the Prado hardware:

- **Screen hue** — caused by mismatched LCD timings in `arkdata.ini` (`CLKDIV1=10`, `VBP=3`, `HBP=20` vs Prado values). Fixed by flashing the Prado `arkdata.ini` to mtd4.
- **Touch keys** — Holden `arkdata.ini` defines 5 side touch keys that do not exist on the Prado hardware.
- **Product identity** — `FactoryConfig.ini` and `MsnProductInfo.ini` reference Holden-specific IDs (`Ksmart_DSP`, `Box-C211`, `McuType=16`).

These are all corrected in the reconstructed firmware. See [Key Differences vs Holden Base Firmware](#key-differences-vs-holden-base-firmware) below.

## Key Differences vs Holden Base Firmware

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

## WiFi Access Point

A WPA2 access point starts automatically on boot via `/etc/wifi_ap.sh`, providing network access for SSH without needing a physical connection.

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

### WiFi module detection

Five Realtek drivers are bundled in `/lib/modules/3.4.0/`. At early boot, `wifi_ap.sh` probes them in order until one loads successfully:

| Priority | Module | Chip | Interface |
|----------|--------|------|-----------|
| 1 | `wlan_rtl8821cs.ko` | RTL8821CS | SDIO (most likely — Feasycom BT+WiFi combo) |
| 2 | `wlan_rtl8822cs.ko` | RTL8822CS | SDIO |
| 3 | `wlan_rtl8189fs.ko` | RTL8189FS | SDIO |
| 4 | `wlan_rtl8821cu.ko` | RTL8821CU | USB |
| 5 | `wlan_rtl8811cu.ko` | RTL8811CU | USB |

If the main app (`MsnCoreApp`) has already placed the correct driver at `/tmp/wlan.ko`, that is used instead. If `wlan0` does not come up, check `dmesg` on the serial console to identify which chip is present and adjust the probe order in `wifi_ap.sh`.

## SSH Access

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

## USB Networking

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

## Build & Flash Tool

`build.sh` is an interactive terminal tool that combines building firmware images and generating a NAND flash update package staged on an SD card into a single workflow. This flashes internal NAND — it is **not** the non-destructive SD-boot image described in [Booting from SD Card or USB](#booting-from-sd-card-or-usb-non-destructive) (that's `build_bootable_sdcard.sh`). Run it under Linux or WSL:

```bash
bash build.sh
```

### Menu layout

The whole menu is one line per item — no per-item description text — so it fits a standard ~24-line terminal without scrolling. Full detail for whichever row is highlighted is shown once, on the detail line just above the command bar, instead of repeated for every row:

```
  ARK1680 Prado — Build & Flash Tool
  ────────────────────────────────────────────────────────
  BUILD
    [ ]  Build rootfs image         no image yet
    [ ]  Build userdata image       no image yet

  NAND PARTITIONS  (staged on SD, flashed to internal NAND on boot)
    [X]  Root Filesystem        rootfs.img       missing
  ▶ [ ]  User Data              userdata.img     missing
    [ ]  Linux Kernel           zImage           missing
    [ ]  Display Config         arkdata.ini      found
     [-]  U-Boot Env             uboot-env.bin    disabled
    [ ]  U-Boot                 uboot.bin        found ⚠
    [ ]  Boot Logo              bootlogo         found
    [ ]  Boot Animation         bootanimation    found
    [ ]  Reversing Track        reversingtrack   found
    [ ]  Unicode Font           unicode          found
  ────────────────────────────────────────────────────────
  User Data: Prado settings / userdata UBI image (build below if needed)  (offset 0x6fa0000, size 0x600000)
  ↑/↓ move   Space/Enter toggle   a/n all/none   g go   q quit
```

The `▶` marker shows which row is highlighted — move it with the arrow keys; the line above the command bar always shows the description, offset, and size for that row.

**Defaults:** rootfs and userdata are selected by default. Kernel, U-Boot, arkdata, and other early-boot partitions default to off — they must be explicitly enabled to avoid accidental reflash. U-Boot Env is disabled entirely (not navigable) until `uboot-env.bin` is built.

### Commands

| Key | Action |
|-----|--------|
| `↑` / `↓` | Move the highlighted row |
| `Space` / `Enter` | Toggle the highlighted row (partition or build step) |
| `a` | Select all partitions |
| `n` | Deselect all partitions |
| `g` | Go — run selected builds then generate SD package |
| `q` | Quit |

### Output

Generated files land in `sd_update/output/`. Copy all files to the root of a FAT32 SD card to flash.

### Standalone scripts

The individual scripts are retained for use without the interactive menu:

| Script | Purpose |
|--------|---------|
| `build_rootfs.sh` | Build rootfs UBI image only |
| `build_userdata.sh` | Build userdata UBI image only |
| `generate_update.sh` | Generate SD package for selected partitions |

### Requirements

Build steps require `mkfs.ubifs` and `ubinize`:

```bash
sudo apt install mtd-utils   # Debian / Ubuntu / WSL
```

## Flashing via SD Card

On power-on, U-Boot checks for a FAT32 SD card. If a file named `UpConfig` is present in the SD root, U-Boot runs `arkupdate`, which reads the `update` script and flashes each listed partition in sequence. After completion the unit reboots — remove the SD card so it doesn't re-flash.

### Partition layout

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

### Quick start

**Step 1 — Build firmware images**

| Image | Command | Notes |
|-------|---------|-------|
| `rootfs.img` | `bash build_rootfs.sh` (Linux/WSL) | ~106 MB UBI image |
| `userdata.img` | `bash build_userdata.sh` (Linux/WSL) | ~6 MB UBI image |

**Step 2 — Generate the update script**

```bash
bash generate_update.sh
```

Toggle partitions with number keys, press `g` to generate. Output lands in `sd_update/output/`.

**Step 3 — Prepare SD card**

Format as FAT32 (max 32 GB). Copy everything from `sd_update/output/` to the SD root.

**Step 4 — Flash**

1. Power off the head unit
2. Insert the SD card
3. Power on — update progress shown on screen
4. Wait for automatic reboot (do **not** interrupt power)
5. Remove the SD card

### Manual update script

The `update` file is plain U-Boot commands, one per line:

```
fatload mmc 0 4000000 zImage
nand scrub 0x1a0000 0x400000 0x1a0000 0x400000
nand write 0x4000000 0x1a0000 ${filesize}

fatload mmc 0 4000000 userdata.img
nand scrub 0x6fa0000 0x600000 0x6fa0000 0x600000
nand write 0x4000000 0x6fa0000 ${filesize}
```

`nand scrub` takes `offset size offset size` (repeated — ARK1680-specific behaviour).

### Safety notes

- **Never flash S-Loader (Nboot) via SD** — corruption bricks the board (requires JTAG to recover)
- **U-Boot** writes to both primary (`0x20000`) and backup (`0xA0000`) slots with the same binary
- **userdata flash** erases all paired BT devices, call history, and user settings — recreated on first boot
- **rootfs flash** replaces the entire filesystem; bad block at 0x5FA0000 is handled automatically

### Serial console (recovery / monitoring)

Connect via the UART header near the SD card slot. Settings: **115200 8N1**.

| Pin | Colour |
|-----|--------|
| TX | Yellow |
| RX | Blue |
| GND | Black |

Adapter: PL-2303HX USB-TTL or Raspberry Pi GPIO UART.

**Connecting via Raspberry Pi:**
```
minicom -D /dev/ttyS0 -b 115200
```

**To interrupt U-Boot and drop to the prompt:** hold the spacebar continuously from the moment power is applied and keep holding until you see the `ark#` prompt.

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

### Boot sequence (stock NAND)

1. **S-Loader (Nboot / Stepldr)** — executes from ROM; loads U-Boot from NAND `0x020000`
2. **U-Boot** — initialises hardware; loads NAND env from `0x120000` (CRC valid — `bootdelay=9`, `bootcmd=run nandboot`, `nandboot`, `setbootargs` etc. all active)
3. **SD update check** — inspects the SD card FAT32 partition for `UpConfig`; if present, runs `arkupdate` to flash partitions listed in the `update` script
4. **Single keypress poll** — prints `Press space key to stop autoboot:  9`, then one `tstc()` check with no delay; if spacebar already held, drops to `ark#` interactive shell; otherwise boots immediately
5. **`run nandboot`** — executes `nandboot` from NAND env: `run setbootargs; bootnand`
   - `setbootargs` → `setenv bootargs console=ttyS0,115200n8 mem=180M ubi.mtd=6 root=ubi0:rootfs rootfstype=ubifs rootwait ro`
   - `bootnand` → custom compiled-in command: `nand read 0x1000000 <kernel_offset> <kernel_size>; bootz 0x1000000`
6. **Linux 3.4.0** starts

> **Note:** `uboot_final.bin` (SD boot) replaces steps 2–5 with standard U-Boot autoboot. `bootdelay=3` is baked into the compiled-in env and the NAND env is intentionally failed, giving a proper 3-second countdown where any key tap works.

At the U-Boot prompt you can manually flash any partition:

```
fatload mmc 0 4000000 zImage
nand scrub 0x1a0000 0x400000 0x1a0000 0x400000
nand write 0x4000000 0x1a0000 ${filesize}
```

## Booting from SD Card or USB (non-destructive)

`uboot_final.bin` is a patched U-Boot that boots a kernel and rootfs from removable media **without writing to NAND**. It is produced by `patch_uboot.py` applied to `uboot_sdboot.bin` (ARK1680 BSP source-compiled U-Boot).

Two patches are applied:
1. **Compiled-in env** — sdboot and usbboot commands baked in as fallback defaults
2. **NAND env redirect** — the three `MOV Rx, #0x120000` ARM instructions that load `CONFIG_ENV_OFFSET` are changed to `MOV Rx, #0xFF000000`, forcing the NAND env CRC to fail so the compiled-in defaults take effect

### Rebuild `uboot_final.bin`

```bash
python3 patch_uboot.py -i uboot_sdboot.bin -o uboot_final.bin --mode sdboot --patch-nand-offset
```

> **Note:** The checked-in `uboot_sdboot.bin` is the post-patch output (the pristine BSP binary was overwritten by `build_bootable_sdcard.sh`). Re-running the command above will succeed but print a warning that no NAND offset candidates were found — this is harmless because the instructions are already redirected.

Place `uboot_final.bin` as `UBOOT.BIN` on the SD card FAT32 partition (p1). Stepldr loads it in preference to the NAND copy.

### SD boot

The compiled-in env boots automatically from the SD card:

| Variable | Value |
|----------|-------|
| `bootcmd` | `run sdboot` |
| `bootdelay` | 3 seconds (interrupt with any key) |
| `bootfile` | `zImage` |
| `mmcdev` | `1` (SD slot) |
| `sdboot` | `run sdbootargs; fatload mmc ${mmcdev}:1 ${loadaddr} ${bootfile}; bootz ${loadaddr}` |
| `sdbootargs` | `console=ttyS0,115200n8 console=tty0 mem=180M root=/dev/mmcblk0p2 rootfstype=ext4 rootwait rw` |

**SD card layout:**

| Partition | Filesystem | Contents |
|-----------|-----------|---------|
| p1 | FAT32 | `UBOOT.BIN`, `zImage` |
| p2 | ext4 | rootfs (`/`), plus `/nanddata/` — see below |
| p3 | ext4 | userdata (`/data`) |

### Building the SD image with `build_bootable_sdcard.sh`

`build_bootable_sdcard.sh` assembles the full bootable SD image (or writes directly to a block device) in one interactive pass: patches `uboot_sdboot.bin` via `patch_uboot.py`, partitions and formats p1/p2/p3, syncs the rootfs and userdata trees, patches `rcS` on the p2 copy only (the source tree is never modified), and populates `/nanddata/` (see below).

```bash
sudo bash build_bootable_sdcard.sh                # interactive, writes sd_boot.img
sudo bash build_bootable_sdcard.sh --device /dev/sdb --non-interactive
```

Key options: `--image PATH` / `--device PATH` (output target), `--size MB` (default 512), `--uboot PATH` (use a prebuilt `UBOOT.BIN` as-is, skip patching), `--root DEVICE` (rootfs device for bootargs, default `/dev/mmcblk0p2`), `--no-userdata` (leave p3 empty — populated by the app on first boot), `--dry-run`. Run with `--help` for the full list.

Requirements: `parted dosfstools e2fsprogs rsync` (plus `mtd-utils` if also building rootfs/userdata images — see [Build & Flash Tool](#build--flash-tool)).

### `/data` mount on SD boot

The rootfs `rcS` copy on p2 is patched to try SD userdata first, falling back to the original NAND paths so the same rootfs image still boots correctly from NAND:

1. Try `mount -t ext4 /dev/mmcblk0p3 /data` (SD userdata)
2. Fall back to NAND UBIFS (`ubiattach` mtd7), then NAND yaffs2, matching the original stock `rcS` logic
3. `factory_reset=1` reformats whichever `/data` is currently active (SD ext4 or NAND UBI) and clears the flag

### Runtime NAND partition data (`/nanddata/`)

Four MTD partitions (bootlogo, bootanimation, reversingtrack, Unicode) are read by the running app via `/dev/mtdN` character devices but are not part of the rootfs or userdata images. A search of all rootfs binaries for MTD ioctls (`MEMGETINFO`, `MEMERASE`) found none of them are called on these partitions — access is plain `open()`/`read()` — so a regular file works as a transparent replacement for the character device.

`build_bootable_sdcard.sh` copies these into `/nanddata/` on p2:

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

USB mass storage is compiled in (MUSB HCD). **Unverified on Prado hardware** — run `usb start` at the U-Boot prompt to confirm the host controller and GPIO assignments work on your unit before relying on this. From the U-Boot prompt, boot from a USB drive with one command:

```
run usbboot
```

Or manually:

```
usb start
fatload usb 0:1 0x1000000 zImage
setenv bootargs "console=ttyS0,115200n8 console=tty0 mem=180M root=/dev/sda2 rootfstype=ext4 rootwait rw"
bootz 0x1000000
```

The `usbboot` and `usbbootargs` env variables are baked into `uboot_final.bin` compiled-in env:

| Variable | Value |
|----------|-------|
| `usbboot` | `usb start; fatload usb 0:1 ${loadaddr} ${bootfile}; run usbbootargs; bootz ${loadaddr}` |
| `usbbootargs` | `console=ttyS0,115200n8 console=tty0 mem=180M root=/dev/sda2 rootfstype=ext4 rootwait rw` |

**USB drive layout:**

| Partition | Filesystem | Contents |
|-----------|-----------|---------|
| p1 | FAT32 | `zImage` |
| p2 | ext4 | rootfs |

The kernel sees the USB drive as `/dev/sda`. The ARK1668 uses MUSB (not EHCI) — USB 2.0 drives work; USB 3.0 drives that require USB 3.0 speeds will not enumerate.

### Console on screen

Both sdbootargs and usbbootargs include `console=tty0`. Once the kernel initialises the LCD framebuffer (`CONFIG_FB_ARK1668LCD`), boot messages and a login prompt are mirrored to the screen via `fbcon`. The U-Boot phase itself is serial-only (no video console compiled into U-Boot).

## Sources

See [`docs/SOURCES.md`](docs/SOURCES.md) for full provenance of each file.
