> **SUPERSEDED — kept for historical reference only.**
> This was the original planning document written before SD boot was implemented.
> The approach it recommended (Option 2, building U-Boot from source) was not
> taken — SD boot ships today via the binary-patch approach (Option 1),
> implemented in `patch_uboot.py` and `build_bootable_sdcard.sh`.
> The current, accurate description of how SD boot actually works — partition
> layout, `/nanddata/` symlinks, rcS userdata fallback, RSTK format, and
> `build_bootable_sdcard.sh` usage — lives in the README's
> [Booting from SD Card or USB](../../README.md#70-booting-stock-kernel-from-sd-card-or-usb-non-destructive)
> section. Some details below (partition sizes, script name, env-patch
> mechanics) no longer match the shipped implementation.

# SD Boot Plan — Option B

Boot the ARK1680 entirely from SD card. **NAND is never written to.**
The SD card provides U-Boot, kernel, rootfs, and userdata as a
self-contained image. NAND content is left intact and is not read at
runtime once booted.

---

## Boot chain overview

```
Nboot (S-Loader, NAND)          ← runs from ROM, untouched
  └─ checks SD for ARKSDLDR.bin → not found, continues
  └─ launches Stepldr (NAND)    ← untouched

Stepldr (NAND)                  ← untouched
  └─ checks SD for UBOOT.BIN    → FOUND → loads from SD into RAM

U-Boot (SD p1, patched binary)
  └─ compiled-in env has bootcmd=run sdboot
  └─ fatload mmc 0 zImage from SD p1
  └─ boots kernel with root=/dev/mmcblk0p2

Kernel
  └─ mounts SD p2 ext4 as /
  └─ rcS mounts SD p3 ext4 as /data
```

---

## The env problem — why this is the hardest part

When Stepldr loads `UBOOT.BIN` from SD and executes it, that U-Boot
binary tries to load its environment from **NAND mtd3**. The NAND env
contains `bootcmd=run nandboot`. The U-Boot from SD obeys the NAND env
and boots from NAND anyway — SD boot never happens.

**We cannot modify the NAND env (no NAND writes). So we must make
the SD U-Boot not depend on the NAND env.**

There are two ways to achieve this (boot.scr auto-execute was tested and is not supported by this U-Boot build):

---

### Option 1 — Binary patch UBOOT.BIN (no source required)

U-Boot stores its compiled-in default env as a null-terminated string
block in the binary. When the NAND env is **invalid** (bad CRC or
erased), U-Boot falls back to these compiled-in defaults.

The approach:

1. **Find the env block** in the UBOOT.BIN binary using `strings`:
   ```
   strings -t x uboot.bin | grep bootcmd
   ```
   This locates the offset of the compiled-in `bootcmd=run nandboot`
   string.

2. **Find the env CRC check** — U-Boot has a function that reads the
   NAND env partition and validates a CRC32. If we can identify the
   NAND offset it reads from in the binary, we can redirect it to a
   location that will always fail CRC (forcing fallback to compiled-in
   defaults), without touching NAND itself.

3. **Patch the compiled-in defaults** — overwrite the string block:
   ```
   bootcmd=run sdboot\0
   sdbootargs=setenv bootargs console=ttyS0,115200n8 mem=180M root=/dev/mmcblk0p2 rootfstype=ext4 rootwait rw\0
   sdboot=run sdbootargs; fatload mmc 0 1000000 zImage; bootz 1000000\0
   ```
   The total byte count of each patched string must not exceed the
   original string length (pad with null bytes to fill).

4. **This only works if NAND env is invalid.** Since NAND env is
   intact and has a valid CRC, the binary patch to compiled-in
   defaults alone is not enough. We also need to redirect where U-Boot
   reads its env from.

#### Redirecting the env read

U-Boot reads its env from a fixed NAND offset matching the mtd3
partition start — `0x120000` on this device (128K S-Loader + 512K
U-boot + 512K U-boot_back = 0x120000).
The env read code in the binary contains this offset as a literal
constant. If we can find and patch that constant to point to an
address that is invalid or empty, the NAND env read will fail CRC and
U-Boot will fall back to our patched compiled-in defaults.

**Approach:**
1. The Limcet P306 NAND layout from the live `mtdparts` env variable:
   `128k(S-Loader),512k(U-boot),512k(U-boot_back),256K(U-boot-Env),...`
   → U-boot-Env (mtd3) starts at NAND offset `0x120000`.
2. Search UBOOT.BIN for the 4-byte little-endian constant
   `0x00120000`. One of the matches will be in the NAND env driver.
3. Patch that constant to an address beyond the end of NAND (e.g.
   `0xFF000000`). The read will fail. U-Boot falls back to
   compiled-in defaults.

**Risk:** this is fragile. The constant `0x00120000` may appear
multiple times in the binary. Patching the wrong one can crash U-Boot.

**Mitigation:** test on a device with JTAG or where a full NAND
restore is possible before committing. Keep the original UBOOT.BIN.

---

### Option 2 — Build U-Boot from source ⚠ SOURCE LOCATED, NOT AN EXACT MATCH

**Update:** the `~/Downloads/linux-arkmicro` path below was on a previous session's machine and was
never actually in this repo — that made the "✓ SOURCE AVAILABLE" claim below unverified for a long
time. It's since been tracked down for real (`RD_Software/linux-arkmicro`, a live public Gogs repo)
and a relevant slice copied into [`linux-arkmicro Reference/`](../linux-arkmicro%20Reference/README.md).
**It is not an exact source match** — it's U-Boot 2018.07 with SPL+FDT, while the Limcet P306's actual stock
U-Boot is 2012.10, legacy ATAG, no devicetree. Same SoC family, later BSP generation. The details below
(largely still accurate) are superseded by the full build plan and risk list in
[`docs/UBOOT_BUILD_GUIDE.md`](../UBOOT_BUILD_GUIDE.md) — read that before acting on this section.

The chip is confirmed as **ARK1668** (marked on the physical package).
This BSP contains a real U-Boot board target for this SoC family. Building it is still the cleanest
approach in principle — the resulting UBOOT.BIN never consults the NAND env at all — but treat it as
"a new, compatible-family U-Boot to test via SD card," not "the exact stock binary, recompiled."

#### Changes needed from the BSP defaults

The BSP `ark1668_defconfig` targets a reference board with 640K
bootloader partitions (env at 0x160000). The Limcet P306 uses 512K
bootloader partitions (env at 0x120000). Only two things differ:

1. **`CONFIG_ENV_OFFSET`** — set to `0x120000` (not `0x160000`)
2. **`CONFIG_BOOTCOMMAND`** — replace NAND boot with SD boot

In `linux-arkmicro/u-boot/include/configs/ark1668.h`, change:
```c
// Before:
#define CONFIG_ENV_OFFSET  0x160000

// After (Limcet P306 partition layout):
#define CONFIG_ENV_OFFSET  0x120000
```

And in `CONFIG_EXTRA_ENV_SETTINGS` / `CONFIG_BOOTCOMMAND`, add:
```c
"sdboot=run sdbootargs; fatload mmc 1 ${loadaddr} zImage; bootz ${loadaddr}\0"
"sdbootargs=setenv bootargs console=ttyS0,115200n8 mem=180M "
    "root=/dev/mmcblk0p2 rootfstype=ext4 rootwait rw\0"
```

SD slot is `mmc 1` — confirmed by `board_mmc_init()` in
`board/arkmicro/ark1668/ark1668.c` which registers `ARK_MMC0`
(internal, index 0) then `ARK_MMC1` (SD slot, index 1).

#### Build

See [`docs/UBOOT_BUILD_GUIDE.md`](../UBOOT_BUILD_GUIDE.md) for the actual clone URL, full config-delta
table (this section only had 2 of the several deltas that doc identifies), toolchain, and — critically
— the SD-only test plan before any NAND flashing is considered.

```bash
git clone http://121.15.164.102:3000/RD_Software/linux-arkmicro.git
cd linux-arkmicro/u-boot
source ../env.source
make ark1668_defconfig
# (apply the CONFIG_ENV_OFFSET and bootcmd changes — full delta table in UBOOT_BUILD_PLAN.md)
make -j$(nproc)
# Output: u-boot.bin — rename to UBOOT.BIN for Stepldr
```

This is the **preferred approach** over binary patching — no risk of
patching the wrong constant, no compiled-in env size constraints.

---

## SD card partition layout

```
┌──────────────────────────────────────────────────────┐
│  SD card raw image                                   │
│                                                      │
│  p1: FAT32   64 MB                                   │
│      UBOOT.BIN    ← Stepldr loads this               │
│      zImage       ← U-Boot fatloads this             │
│      initramfs.cpio.gz  ← if MMC module-only         │
│                                                      │
│  p2: ext4   300 MB                                   │
│      full rootfs tree                                │
│      kernel mounts as /                              │
│                                                      │
│  p3: ext4    64 MB                                   │
│      userdata (msncfg/, feasycom/, etc.)             │
│      rcS mounts as /data                             │
└──────────────────────────────────────────────────────┘
```

---

## Phase 1 — Verify kernel capabilities (no NAND writes, safe to test)

### 1a. Is ark_dw_mmc.ko built into the kernel or module-only?

`ark_dw_mmc.ko` exists as a loadable module. If the kernel does NOT
have MMC built in, it cannot mount an SD rootfs because the driver is
on the SD card being mounted — chicken-and-egg.

**Test:** boot with `root=/dev/mmcblk0p2`. If MMC is built-in, kernel
mounts it. If not, kernel panics with "unable to mount root fs" and we
add an initramfs (Phase 4).

### 1b. Confirm SD mmc device index

`mmcdev=1` in the NAND env. The Holden update script used `mmc 0`
and read from SD successfully. The SD U-Boot may index the SD slot
differently. Read the U-Boot serial output line:
```
MMC: ARK_MMC0: 0, ARK_MMC1: 1
```
to determine which index maps to the physical SD slot.

---

## Phase 2 — U-Boot (env strategy, in order of preference)

1. **Build from source** (Option 2) — preferred; `linux-arkmicro/u-boot` is our exact chip
2. **Binary patch** (Option 1) — fallback if toolchain setup is not feasible

U-Boot bootargs for SD (whichever method):

```
console=ttyS0,115200n8 mem=180M root=/dev/mmcblk0p2 rootfstype=ext4 rootwait rw
```

Note: drop `ro` — /data and app logs need to write to the filesystem.

---

## Phase 3 — rootfs changes (rcS)

Replace the UBIFS /data mount block with ext4 from SD p3, with NAND
UBIFS as fallback so the same rootfs image boots from either SD or NAND:

```sh
# Mount userdata — try SD p3 first, fall back to NAND UBI
if mount -o sync -t ext4 /dev/mmcblk0p3 /data 2>/dev/null; then
    echo "userdata: SD ext4"
else
    USERDATAFS=$(cat /proc/mounts | grep ubifs)
    if [ "${USERDATAFS}" != "" ]; then
        mtd_partition=7
        resetenv=$(fw_printenv factory_reset 2>/dev/null)
        if [ "${resetenv##*=}" = "1" ]; then
            flash_eraseall /dev/mtd$mtd_partition
            ubiattach /dev/ubi_ctrl -m $mtd_partition
            ubimkvol /dev/ubi1 -N userdata -m 14
            ubidetach /dev/ubi_ctrl -m $mtd_partition -d 1
            fw_setenv factory_reset 0 2>/dev/null
        fi
        ubiattach /dev/ubi_ctrl -m $mtd_partition
        mount -o sync -t ubifs ubi1_0 /data
        echo "userdata: NAND UBI"
    fi
fi
```

No change to NAND. The SD path is tried first; NAND path is unchanged
for fallback compatibility.

---

## Phase 4 — Initramfs (only if MMC is module-only)

If Phase 1 shows the kernel panics without the MMC driver, build a
minimal initramfs that loads the module before pivoting to SD rootfs.

### Contents

```
initramfs/
├── init
├── bin/busybox             ← ARMv5T static binary
└── lib/modules/3.4.0/kernel/drivers/ark/sdmmc/
    └── ark_dw_mmc.ko
```

### init script

```sh
#!/bin/sh
/bin/busybox insmod /lib/modules/3.4.0/kernel/drivers/ark/sdmmc/ark_dw_mmc.ko
sleep 1
/bin/busybox mount -t ext4 /dev/mmcblk0p2 /mnt
exec /bin/busybox switch_root /mnt /sbin/init
```

### U-Boot sdboot with initramfs

```
sdboot=run sdbootargs; fatload mmc 0 4000000 initramfs.cpio.gz; fatload mmc 0 1000000 zImage; bootz 1000000 4000000
```

Files on SD p1: `UBOOT.BIN`, `zImage`, `initramfs.cpio.gz`.

---

### U-Boot direct boot (no initramfs — requires custom kernel with built-in MMC driver)

If you compile a custom kernel with the DesignWare MMC driver compiled directly into the kernel (`CONFIG_MMC_DW=y` and `CONFIG_MMC_DW_PLTFM=y`), you do not need an initramfs at all. The kernel will read the SD card immediately at boot.

Manual boot commands:
```bash
# 1. Select the SD card and load the custom kernel to RAM
mmc dev 0
fatload mmc 0:1 0x1000000 zImage

# 2. Set bootargs targeting the SD card partition 2 (ext4) directly
setenv bootargs console=ttyS0,115200n8 mem=180M earlyprintk=serial root=/dev/mmcblk0p2 rootfstype=ext4 rootwait rw ${mtdparts} screen=${screen}

# 3. Boot the kernel
bootz 0x1000000
```

---

### U-Boot initramfs boot (requires custom kernel with initramfs support)

If you keep the MMC host driver as a loadable module but compile a custom kernel with initramfs support enabled (`CONFIG_BLK_DEV_INITRD=y`), you must load the wrapped `uInitrd` alongside the kernel:

Manual boot commands:
```bash
# 1. Select the SD card and load the custom kernel and wrapped ramdisk
mmc dev 0
fatload mmc 0:1 0x1000000 zImage
fatload mmc 0:1 0x2000000 uInitrd

# 2. Set the working bootargs (excluding initrd= since U-Boot passes it via ATAGs)
setenv bootargs console=ttyS0,115200n8 mem=180M earlyprintk=serial ubi.mtd=6 root=ubi0:rootfs rootfstype=ubifs wait ro ${mtdparts} screen=${screen}

# 3. Boot passing the ramdisk address to bootz
bootz 0x1000000 0x2000000
```

---

## Phase 5 — Build script: build_sdimage.sh (Linux/WSL)

Produces `sd_boot.img` — a single raw image, write with dd or Etcher.

```bash
#!/bin/bash
set -e
IMG=sd_boot.img
SIZE_MB=512

dd if=/dev/zero of=$IMG bs=1M count=$SIZE_MB

parted -s $IMG mklabel msdos
parted -s $IMG mkpart primary fat32 1MiB 65MiB
parted -s $IMG mkpart primary ext4  65MiB 365MiB
parted -s $IMG mkpart primary ext4  365MiB 100%

LOOP=$(losetup -Pf --show $IMG)

mkfs.fat -F32 ${LOOP}p1
mkfs.ext4 -L rootfs   ${LOOP}p2
mkfs.ext4 -L userdata ${LOOP}p3

mkdir -p /tmp/sd_p1 /tmp/sd_p2 /tmp/sd_p3
mount ${LOOP}p1 /tmp/sd_p1
mount ${LOOP}p2 /tmp/sd_p2
mount ${LOOP}p3 /tmp/sd_p3

# p1: bootloader + kernel
cp "Prado firmware reconstructed/mtd1-mtd2_uboot/uboot.bin" /tmp/sd_p1/UBOOT.BIN
cp kernel/zImage /tmp/sd_p1/zImage
[ -f initramfs/initramfs.cpio.gz ] && cp initramfs/initramfs.cpio.gz /tmp/sd_p1/
# p2: rootfs
rsync -a --exclude=proc/ --exclude=sys/ --exclude=dev/ --exclude=tmp/ \
    "Prado firmware reconstructed/mtd6_rootfs/rootfs/" /tmp/sd_p2/
mkdir -p /tmp/sd_p2/{proc,sys,dev,tmp}

# p3: userdata
rsync -a "Prado firmware reconstructed/mtd7_userdata/userdata/" /tmp/sd_p3/

umount /tmp/sd_p1 /tmp/sd_p2 /tmp/sd_p3
losetup -d $LOOP
echo "Done: $IMG"
```

### Requirements

```bash
sudo apt install parted dosfstools e2fsprogs rsync
```

---

## Phase 6 — NAND runtime partition data

Several NAND partitions are not part of the rootfs or userdata but are
accessed by the application at runtime via `/dev/mtdN` character devices:

| MTD | Partition | Size | Content | Status |
|-----|-----------|------|---------|--------|
| 8 | bootlogo | 512 KB | U-Boot splash image | Dumped — `mtd8_bootlogo/bootlogo` (31 KB used) |
| 9 | bootanimation | 3 MB | Boot animation sequence | **Erased during Holden flash** — placeholder only |
| 10 | reversingtrack | 3 MB | Reverse camera guide line overlays | Dumped — `mtd10_reversingtrack/reversingtrack` (1.2 MB) |
| 11 | Unicode | 256 KB | Font data for UI text rendering | Dump not yet obtained — placeholder only |

### Why these can be served from SD

A search of all rootfs binaries for MTD ioctl constants (`MEMGETINFO`,
`MEMERASE`) confirmed **no application binary calls `MEMGETINFO`** on
these partitions. Access is read-only via `open()` + `read()` — plain
file I/O. A symlink from `/dev/mtdN` to a regular file is transparent
to the application.

### RSTK format (reversingtrack)

The `reversingtrack` file uses a custom **RSTK** container format:

```
Offset  Size  Content
0x00    4     Magic: "RSTK"
0x04    4     Total file size
0x0C    4     Entry count (41)
0x14    4     Steering positions (100)
0x24    4     Image width (800)
0x28    4     Image height (480)
0x2C+       Guide line zone parameters (Y coordinates, widths)
0x90+       Index table: 41 × 20-byte entries
              [index, index, file_offset, compressed_size, flag]
data         41 zlib-compressed overlay images
```

The 41 frames represent steering wheel positions from full-left to
full-right. Frame sizes form a bell curve (31 KB at extremes, 17 KB at
centre), consistent with symmetric guide line geometry at the centre
position compressing smaller. The app decompresses the frame matching
the current steering angle and composites it over the camera feed.

### How the SD image handles these partitions

`build_bootable_sdcard.sh` copies the partition files into `/nanddata/`
on the rootfs partition (p2) during the build. The `patch_rcs()` step
inserts the following block into `rcS` after `mdev -s`:

```sh
# Replace MTD data partition devices with symlinks to SD-stored files.
for mtdmap in "8:bootlogo" "9:bootanimation" "10:reversingtrack" "11:unicode"; do
    num="${mtdmap%%:*}"
    name="${mtdmap##*:}"
    rm -f /dev/mtd${num}
    ln -sf /nanddata/${name} /dev/mtd${num}
    echo "mtd${num}: /nanddata/${name}"
done
```

The symlinks are unconditional — any NAND device node created by mdev
is removed first. SD is always authoritative for these partitions.

### Replacing placeholders

When dumps are obtained, drop the raw binary into the matching folder
and rebuild the SD image:

```
Prado firmware reconstructed/mtd9_bootanimation/bootanimation  ← replace with real dump
Prado firmware reconstructed/mtd11_unicode/unicode              ← replace with real dump
```

To dump from a running device via serial console:
```sh
dd if=/dev/mtd9  of=/tmp/bootanimation && # copy via USB/SSH
dd if=/dev/mtd11 of=/tmp/unicode
```

---

## Phase 7 — Known risks

| # | Risk | Likelihood | Mitigation |
|---|------|-----------|------------|
| 1 | Binary patch corrupts U-Boot | Medium | Keep original; test with serial console attached |
| 2 | `ark_dw_mmc.ko` module-only — kernel can't mount SD root | Medium | Initramfs (Phase 4) |
| 3 | MMC device index wrong (mmc 0 vs mmc 1) | Medium | Confirm from serial console |
| 4 | App uses MTD ioctls (e.g. `MEMGETINFO`) on data partitions | Very low | Confirmed absent in all rootfs binaries — plain read() only |
| 5 | ext4 not in kernel | Very low | ext4 built-in since Linux 2.6.28 |
| 6 | App writes depend on /data being available | Medium | SD /data mount is first in rcS — same order as NAND |

---

## Implementation order

1. **Serial console attached** — confirm mmc device index and whether MMC
   driver is built-in (kernel panic = module-only → add initramfs)
2. **Binary patch U-Boot** (Option 1) — use `patch_uboot.py` to
   invalidate the NAND env offset and patch compiled-in defaults;
   Option 2 (source build) is unlikely without ARK1680 U-Boot source
3. **Prepare SD p1** — write patched `UBOOT.BIN` + `zImage` to FAT32;
   add ext4 rootfs to p2 and test root mount
4. **Modify `rcS`** for SD /data mount (Phase 3)
5. **Build full image** using `build_bootable_sdcard.sh` and produce `sd_boot.img`
6. **Full boot test** — serial console, verify app init, BT, WiFi AP

---

## Files to create / modify

| File | Change |
|------|--------|
| `build_bootable_sdcard.sh` | Assembles bootable SD image — interactive, patches rcS and populates /nanddata/ |
| `patch_uboot.py` | Patches compiled-in U-Boot env for SD boot |
| `Prado firmware reconstructed/mtd6_rootfs/rootfs/etc/rc.d/rcS` | **Not modified** — patch applied at build time to the SD copy only |
| `Prado firmware reconstructed/mtd8_bootlogo/bootlogo` | Raw bootlogo partition dump |
| `Prado firmware reconstructed/mtd9_bootanimation/bootanimation` | Placeholder — erased, replace when dump available |
| `Prado firmware reconstructed/mtd10_reversingtrack/reversingtrack` | Raw reversingtrack partition dump (RSTK/zlib format) |
| `Prado firmware reconstructed/mtd11_unicode/unicode` | Placeholder — replace when dump available |
| `initramfs/` | New directory — only if MMC is module-only |
| `docs/historical/SD_BOOT_PLAN.md` | This document |

**NAND is never written to at any stage.**
