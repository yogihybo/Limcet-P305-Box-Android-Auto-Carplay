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
Prado dump/                  Raw MTD partition dumps from the live device
  mtd1-mtd2_uboot/           U-Boot binaries (raw + extracted)
  mtd3_env/                  U-Boot environment (raw + extracted)
  mtd4_arkdata/              Panel/hardware config (raw + extracted)
  mtd5_kernel/               Kernel zImage (raw + extracted)
  mtd6_rootfs/               Root filesystem UBIFS dump (raw)
  mtd6_rootfs_raw/           Raw MTD6 bin (Git LFS)

Prado reconstructed/         Reconstructed firmware for flashing
  mtd0_sloader/              Nboot.bin, Stepldr.bin
  mtd1-mtd2_uboot/           uboot.bin
  mtd6_rootfs/
    rootfs/                  Modified rootfs tree (Prado libs + SSH + WiFi AP)
  mtd7_userdata/
    userdata/                Userdata tree (Prado settings overlay)
  mtd8_bootlogo/             bootlogo
  mtd9_bootanimation/        (placeholder — no content yet)
  mtd10_reversingtrack/      reversingtrack

kernel/            zImage (from Holden base — identical kernel_size to Prado dump)
display/
  arkdata_prado.ini          Prado panel config (from MTD4 live dump)
  arkdata_holden.ini         Holden standard reference
  arkdata_holden_0324.ini    Holden March 2024 update reference
msn_factory_configs/
  FactoryConfig.ini          Prado identity + Holden firmware settings
  MsnProductInfo.ini         Hardware identity (Limcet-P306)
env/
  uboot-env.txt              Reconstructed env (bootdelay=9, 106m/6m layout)
  mtd3_env_prado_dump.bin    Raw env from live device (gitignored)
sd_update/
  UpConfig                   SD update trigger file
  update.example             Static reference script (generated version goes to output/)
  output/                    Generated SD card package (gitignored)
docs/
  SOURCES.md                 Where each file came from and why
  PARTITION_LAYOUT.md        NAND offsets, sizes, flash commands
build.sh                     Combined interactive build and flash tool
build_rootfs.sh              Standalone rootfs UBI image builder
build_userdata.sh            Standalone userdata UBI image builder
generate_update.sh           Standalone SD card update script generator
```

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

`g_zero.ko` has been removed from `Prado reconstructed/mtd6_rootfs/rootfs/etc/all.sh` — it was overriding the NCM gadget registration and breaking both USB host mode and the network interface.

## Build & Flash Tool

`build.sh` is an interactive terminal tool that combines building firmware images and generating an SD card update package into a single workflow. Run it under Linux or WSL:

```bash
bash build.sh
```

### Menu layout

```
  BUILD
  ─────
  9  [ ]  Build rootfs image      Compiles source tree → rootfs.img (~106 MB)
  10 [ ]  Build userdata image    Overlays Prado settings → userdata.img (~6 MB)

  SD CARD PARTITIONS
  ──────────────────
  1  [X]  Root Filesystem         rootfs.img       0x5a0000   106 MB
  2  [X]  User Data               userdata.img     0x6fa0000    6 MB
  3  [ ]  Linux Kernel            zImage           0x1a0000     4 MB
  4  [ ]  Display Config (arkdata) arkdata.ini     0x160000   256 KB
  5  [ ]  U-Boot Env              uboot-env.bin    0x120000   256 KB
  6  [ ]  U-Boot                  uboot.bin        0x020000   512 KB  ⚠ brick risk
  7  [ ]  Boot Logo               bootlogo         0x75a0000  512 KB
  8  [ ]  Boot Animation          bootanimation    0x7620000    3 MB
  9  [ ]  Reversing Track         reversingtrack   0x7920000    3 MB
```

**Defaults:** rootfs and userdata are selected by default. Kernel, U-Boot, arkdata, and other early-boot partitions default to off — they must be explicitly enabled to avoid accidental reflash.

### Commands

| Key | Action |
|-----|--------|
| `1`–`9` | Toggle SD partition on/off |
| `10`–`11` | Toggle build step on/off |
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

Adapter: PL-2303HX USB-TTL. At the U-Boot prompt you can manually flash any partition:

```
fatload mmc 0 4000000 zImage
nand scrub 0x1a0000 0x400000 0x1a0000 0x400000
nand write 0x4000000 0x1a0000 ${filesize}
```

## Sources

See [`docs/SOURCES.md`](docs/SOURCES.md) for full provenance of each file.
