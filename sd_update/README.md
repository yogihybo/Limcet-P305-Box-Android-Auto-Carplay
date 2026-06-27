# SD Card Update — Prado / Limcet-P306 (ARK1680)

This folder contains the tools to build an SD card that will flash one or more NAND
partitions on the head unit at boot. The update is triggered by U-Boot's `arkupdate`
command, which detects the `UpConfig` file on a FAT32 SD card.

---

## How the update mechanism works

1. On power-on, U-Boot checks for a FAT32 SD card in the SD slot
2. If a file named `UpConfig` is present in the SD root, U-Boot runs `arkupdate`
3. `arkupdate` reads the `update` script from the SD card and executes each line as a
   U-Boot command
4. Each partition is flashed by: `fatload` (RAM) → `nand scrub` (erase) → `nand write`
5. After completion the unit reboots; remove the SD card so it doesn't re-flash

---

## Prado partition layout

| Partition      | Start      | Size      | Contents                         |
|----------------|------------|-----------|----------------------------------|
| S-Loader       | 0x000000   | 128 KB    | Nboot (do NOT update via SD)     |
| U-Boot         | 0x020000   | 512 KB    | 2nd-stage bootloader             |
| U-Boot_back    | 0x0A0000   | 512 KB    | U-Boot backup slot               |
| U-Boot-Env     | 0x120000   | 256 KB    | U-Boot environment variables     |
| arkdata        | 0x160000   | 256 KB    | Display / TvoutType config       |
| kernel         | 0x1A0000   | 4 MB      | Linux 3.4.0 zImage               |
| rootfs         | 0x5A0000   | 106 MB    | Root filesystem (UBIFS/UBI)      |
| userdata       | 0x6FA0000  | 6 MB      | User settings / BT pairs (UBI)   |
| bootlogo       | 0x75A0000  | 512 KB    | Boot splash screen               |
| bootanimation  | 0x7620000  | 3 MB      | Boot animation (not updateable here) |
| reversingtrack | 0x7920000  | 3 MB      | Reversing camera audio           |
| Unicode        | 0x7C20000  | 256 KB    | Unicode font data                |

**Known bad block at 0x5FA0000** — inside the rootfs partition. `nand scrub` handles
this automatically during the erase phase.

---

## Quick start

### Step 1 — Build the firmware images you want to flash

| Image | Build command | Notes |
|-------|---------------|-------|
| `rootfs.img` | `bash build_rootfs.sh` (Linux/WSL) | ~106 MB UBI image |
| `userdata.img` | `bash build_userdata.sh` (Linux/WSL) | ~6 MB UBI image |
| `uboot.bin` | Holden firmware package → `uboot.bin` | or `bootloaders/uboot.bin` |
| `zImage` | Holden firmware package → `zImage` | or `kernel/zImage` |
| `uboot-env.bin` | `mkenvimage -s 4096 -o uboot-env.bin env.txt` | 4096 bytes exactly |
| `arkdata.ini` | Edit `display/arkdata.ini` | plain text INI |
| `bootlogo` | Raw binary, device-specific format | from `bootloaders/bootlogo` |
| `reversingtrack` | Raw binary | from `bootloaders/reversingtrack` |

### Step 2 — Generate the update script

Run the interactive selector (Linux/WSL):

```bash
cd sd_update/
bash generate_update.sh
```

Toggle partitions with the number keys, then press `g` to generate.  
Output files land in `sd_update/output/`.

### Step 3 — Prepare the SD card

Format the SD card as **FAT32** (max 32 GB).  

Copy everything from `sd_update/output/` to the **root** of the SD card:

```
SD:/
  UpConfig          ← trigger file (empty)
  update            ← generated flash script
  zImage            ← (if selected)
  rootfs.img        ← (if selected)
  userdata.img      ← (if selected)
  uboot.bin         ← (if selected)
  ...
```

### Step 4 — Flash

1. Power off the head unit completely
2. Insert the prepared SD card
3. Power on — the unit will show update progress on screen
4. Wait until the unit reboots on its own (do **not** interrupt power)
5. Remove the SD card

---

## Manually writing the update script

If you prefer to build the `update` file by hand, the format is plain U-Boot commands,
one per line. Example for kernel + userdata only:

```
fatload mmc 0 4000000 zImage
nand scrub 0x1a0000 0x400000 0x1a0000 0x400000
nand write 0x4000000 0x1a0000 ${filesize}

fatload mmc 0 4000000 userdata.img
nand scrub 0x6fa0000 0x600000 0x6fa0000 0x600000
nand write 0x4000000 0x6fa0000 ${filesize}
```

The `nand scrub` command takes `offset size offset size` (offset and size repeated — this
is ARK1680-specific behaviour observed from live update logs).

---

## Safety notes

- **Never flash S-Loader (Nboot) via SD update** — if it is corrupted the board is bricked
  with no software recovery path (requires JTAG)
- **U-Boot update** writes to both the primary slot (0x20000) and backup slot (0xA0000)
  with the same binary. This is the correct dual-slot behaviour.
- **userdata flash** erases all paired BT devices, call history, and user settings.
  The application recreates defaults on first boot.
- **rootfs flash** replaces the entire filesystem. The Prado has a known bad block at
  0x5FA0000; `nand scrub` marks it and skips it automatically.
- Remove the SD card after flashing. If `UpConfig` is still present when the unit boots,
  `arkupdate` will run again.

---

## Serial console (for recovery / monitoring)

If the update fails or the unit fails to boot, connect via the UART header near the SD
card slot (3-pin: TX=yellow, RX=blue, GND=black). Settings: **115200 8N1**.

Adapter: PL-2303HX USB-TTL (do not use the USB-A port on the head unit itself).

At the U-Boot prompt you can manually flash any partition:

```
# Load from SD card
fatload mmc 0 4000000 zImage

# Erase and write kernel
nand scrub 0x1a0000 0x400000 0x1a0000 0x400000
nand write 0x4000000 0x1a0000 ${filesize}
```
