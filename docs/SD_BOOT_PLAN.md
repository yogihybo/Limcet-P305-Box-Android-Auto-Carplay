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

There are two ways to achieve this:

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

U-Boot reads its env from a fixed NAND offset (typically matching the
mtd3 partition address from the mtdparts string, e.g. `0x60000`).
The env read code in the binary contains this offset as a literal
constant. If we can find and patch that constant to point to an
address that is invalid or empty, the NAND env read will fail CRC and
U-Boot will fall back to our patched compiled-in defaults.

**Approach:**
1. In the NAND env, the env partition is defined in `mtdparts`:
   `nand0: 64k@0(nboot),320k@64k(uboot),64k@384k(env),...`
   → env is at NAND offset `0x60000` (384 KiB).
2. Search UBOOT.BIN for the 4-byte big-endian or little-endian
   constant `0x00060000`. One of the matches will be in the NAND env
   driver.
3. Patch that constant to an address beyond the end of NAND (e.g.
   `0xFF000000`). The read will fail. U-Boot falls back to
   compiled-in defaults.

**Risk:** this is fragile. The constant `0x00060000` may appear
multiple times in the binary. Patching the wrong one can crash U-Boot.

**Mitigation:** test on a device with JTAG or where a full NAND
restore is possible before committing. Keep the original UBOOT.BIN.

---

### Option 2 — Build U-Boot from source (unlikely — source not available)

If the ARK1680 U-Boot source is obtainable (it may be in an SDK
release or on GitHub as `ark1680-uboot`), rebuild with:

```
CONFIG_ENV_IS_IN_MMC=y         (read env from SD instead of NAND)
CONFIG_SYS_MMC_ENV_DEV=0       (SD = mmc device 0)
CONFIG_SYS_MMC_ENV_PART=1      (env in FAT partition 1)
CONFIG_BOOTCOMMAND="run sdboot"
CONFIG_EXTRA_ENV_SETTINGS= \
    "sdbootargs=setenv bootargs console=ttyS0,115200n8 mem=180M "
    "root=/dev/mmcblk0p2 rootfstype=ext4 rootwait rw\0"
    "sdboot=run sdbootargs; fatload mmc 0 1000000 zImage; bootz 1000000\0"
```

This is the cleanest solution. The resulting UBOOT.BIN never looks at
NAND for its env.

---

### Option 3 — Try boot.scr first (zero risk, test quickly)

Some U-Boot builds auto-execute a compiled script file `boot.scr` from
the boot FAT partition. This is worth a 5-minute test before any
patching:

1. Install `u-boot-tools` on Linux/WSL: `sudo apt install u-boot-tools`
2. Create `sd_boot.cmd`:
   ```sh
   setenv bootargs console=ttyS0,115200n8 mem=180M root=/dev/mmcblk0p2 rootfstype=ext4 rootwait rw
   fatload mmc 0 1000000 zImage
   bootz 1000000
   ```
3. Compile: `mkimage -T script -C none -A arm -n boot -d sd_boot.cmd boot.scr`
4. Place `boot.scr` on SD p1 FAT32 alongside `UBOOT.BIN`
5. Boot and watch serial console — if U-Boot loads and executes it,
   the env problem is solved with no patching at all.

**This should be tried first.** It is non-destructive and immediately
tells us whether the ARK1680 U-Boot has boot.scr support.

---

## SD card partition layout

```
┌──────────────────────────────────────────────────────┐
│  SD card raw image                                   │
│                                                      │
│  p1: FAT32   64 MB                                   │
│      UBOOT.BIN    ← Stepldr loads this               │
│      zImage       ← U-Boot fatloads this             │
│      boot.scr     ← Option 3: auto-executed script   │
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

1. **Try `boot.scr`** (Option 3 above) — 5 minutes, zero risk
2. If not supported, **binary patch** (Option 1) — primary fallback
3. **Build from source** (Option 2) — cleanest but unlikely; ARK1680 U-Boot source not available

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
[ -f sd_boot.scr ] && cp sd_boot.scr /tmp/sd_p1/boot.scr

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

## Phase 6 — Known risks

| # | Risk | Likelihood | Mitigation |
|---|------|-----------|------------|
| 1 | boot.scr not supported → env cannot be overridden | Medium | Try binary patch or build from source |
| 2 | Binary patch corrupts U-Boot | Medium | Keep original; test with serial console attached |
| 3 | `ark_dw_mmc.ko` module-only — kernel can't mount SD root | Medium | Initramfs (Phase 4) |
| 4 | MMC device index wrong (mmc 0 vs mmc 1) | Medium | Confirm from serial console |
| 5 | App has hardcoded `/dev/ubi0` or `/dev/mtd*` paths | Low | Monitor first boot serial; patch if found |
| 6 | ext4 not in kernel | Very low | ext4 built-in since Linux 2.6.28 |
| 7 | App writes depend on /data being available | Medium | SD /data mount is first in rcS — same order as NAND |

---

## Implementation order

1. **Prepare SD p1** — put existing `UBOOT.BIN` + `zImage` on FAT32;
   try `boot.scr` approach (Option 3) — costs nothing to test
2. **Serial console attached** — watch U-Boot output to determine:
   - Whether boot.scr is executed
   - Which mmc index is the SD slot
   - Whether MMC is built-in (kernel panic or successful mount)
3. **If boot.scr works**: write `sdbootargs` and `sdboot` in the
   script; add ext4 rootfs to p2 and test root mount
4. **If boot.scr fails**: proceed with Option 1 (binary patch);
   Option 2 (source build) is unlikely without access to ARK1680 U-Boot source
5. **Modify `rcS`** for SD /data mount (Phase 3)
6. **Build `build_sdimage.sh`** and produce full `sd_boot.img`
7. **Full boot test** — serial console, verify app init, BT, WiFi AP

---

## Files to create / modify

| File | Change |
|------|--------|
| `sd_boot.cmd` | New — U-Boot script source (compiled to `boot.scr`) |
| `build_sdimage.sh` | New — assembles raw SD image |
| `Prado firmware reconstructed/mtd6_rootfs/rootfs/etc/rc.d/rcS` | SD /data mount path (no NAND change) |
| `initramfs/` | New directory — only if MMC is module-only |
| `docs/SD_BOOT_PLAN.md` | This document |

**NAND is never written to at any stage.**
