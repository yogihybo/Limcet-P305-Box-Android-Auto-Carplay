# U-Boot Build Guide — ARK1668 Prado (Limcet P305)

## Boot Chain Overview

```
ROM Boot (IRAM)
  └─► Nboot.bin   (NAND offset 0x000000)  — SoC ROM loads this
        └─► Stepldr.bin (NAND 0x20000)    — initializes DDR3
              └─► UBOOT.BIN (SD card p1)  — loaded to 0x30000, jumped to
                    └─► Kernel (NAND/SD)
```

**Critical constraint:** Stepldr fully initializes DDR3 *before* jumping to `UBOOT.BIN`.
U-Boot must **not** re-run DDR/CPU low-level init — doing so corrupts the memory controller
and hangs the system (the "Starting Uboot → no console" symptom).

---

## Stock Binary Structure (Reverse Engineered)

The stock `uboot.bin` has an ARK-proprietary header embedded in the exception vector table
area. Discovered by comparing all available firmware dumps (Prado, P306, Holden):

| Source           | Magic      | Load     | Entry Point  | File Size  |
|------------------|------------|----------|--------------|------------|
| Prado dump       | 0x12345678 | 0x30000  | 0x00054ef8   | 0x0005bc88 |
| P306 firmware    | 0x12345678 | 0x30000  | 0x00055120   | 0x0005bee8 |
| Holden firmware  | 0x12345678 | 0x30000  | 0x0005507c   | 0x0005be2c |

### Header Layout (offsets from start of file)

```
0x00–0x1f  ARM exception vector table (reset + 7 exception branches)
0x20–0x3b  Exception handler address table (7 words)
0x3c       0x12345678  ← ARK magic (must be present for valid image)
0x40       0x00030000  ← Load address (constant across all variants)
0x44       <ep>        ← Entry point (RAM address of board_init_r / _main)
0x48       <ep>        ← Entry point duplicate
0x4c       <bss_end>   ← BSS end address in RAM (for BSS zeroing)
0x50       <filesize>  ← Exact size of u-boot.bin
0x54–0x5c  0x0badc0de  ← Padding sentinels
0x60+      ...         ← Real startup code begins here
```

This header is **not produced by the open-source U-Boot build**. It must be injected
post-build using `inject_ark_header.py`.

---

## Problems Found in Previous Compiled Binary

### Problem 1 — Missing ARK magic ❌
```
Compiled offset 0x3c = 0xdeadbeef   (balignl padding from vectors.S)
Stock    offset 0x3c = 0x12345678   (required ARK magic)
```

### Problem 2 — Exception stubs bloat the header ❌
```
Stock:    _start at file offset 0x060  (96 bytes of header, then code)
Compiled: _start at file offset 0x2e8  (744 bytes — full exception stubs included)
```
Without `CONFIG_SPL_BUILD`, `vectors.S` generates full exception handler stubs
(7 × `.align 5` = 32-byte stubs). The ARK header injection handles this — the EP
field in the header points to the actual entry point regardless of offset.

### Problem 3 — DDR re-initialization hangs the system ❌
`start.S` calls `cpu_init_cp15` → `cpu_init_crit` → `lowlevel_init` → DDR init.
Since Stepldr already initialized DDR, re-running this corrupts the memory controller.
**Fix:** `#define CONFIG_SKIP_LOWLEVEL_INIT` and `CONFIG_SKIP_LOWLEVEL_INIT_ONLY`.

### Problem 4 — Init stack pointer inside the binary ❌
```c
// OLD: resolves to 0x3c00, which is INSIDE the binary at 0x30000!
#define CONFIG_SYS_INIT_SP_ADDR \
    (CONFIG_SYS_SDRAM_BASE + 16*1024 - GENERATED_GBL_DATA_SIZE)
// = 0x00000000 + 0x4000 - ~256 = ~0x3c00  ← overlaps loaded image

// NEW: safely above the binary (~370KB loaded from 0x30000)
#define CONFIG_SYS_INIT_SP_ADDR  0x80000
```

---

## Source Code Changes Made

### `configs/ark1668_limcet_p305_defconfig`

Produces a **single flat binary** — no SPL, no split image:

```diff
 CONFIG_ARM=y
 CONFIG_SYS_L2CACHE_OFF=y
-CONFIG_SPL_LDSCRIPT="arch/arm/mach-arkmicro/armv7/u-boot-spl.lds"
 CONFIG_ARCH_ARKMICRO=y
 CONFIG_SYS_TEXT_BASE=0x30000
-CONFIG_SPL_GPIO_SUPPORT=y
-CONFIG_SPL_LIBCOMMON_SUPPORT=y
-CONFIG_SPL_LIBGENERIC_SUPPORT=y
-CONFIG_SPL_SERIAL_SUPPORT=y
-CONFIG_SPL=y
+CONFIG_SKIP_LOWLEVEL_INIT=y
 CONFIG_DEBUG_UART_BASE=0xe4200000
 ...
-# CONFIG_SPL_RAW_IMAGE_SUPPORT is not set
-CONFIG_SPL_NAND_SUPPORT=y
+# CONFIG_SPL_NAND_SUPPORT is not set
 ...
-CONFIG_ENV_IS_IN_NAND=y
+CONFIG_ENV_IS_NOWHERE=y
+# CONFIG_ENV_IS_IN_NAND is not set
```

**Why `CONFIG_ENV_IS_NOWHERE`?** The NAND env partition (`0x120000`) was written by the
stock binary with different BCH ECC parameters. Every boot printed 8× ECC errors before
falling back to defaults anyway. Since `uEnv.txt` on the SD card overrides everything
needed, there is no benefit to reading from NAND. `ENV_IS_NOWHERE` uses compiled-in
defaults immediately with no NAND access, no error spam.


> **Note:** `CONFIG_SKIP_LOWLEVEL_INIT` is a `#define`, not a Kconfig option in this
> U-Boot version. It is set directly in the `.h` config below. The defconfig entry
> serves as documentation only.

### `include/configs/ark1668_limcet_p305.h`

**Change 1 — Skip low-level init** (added near top of file):
```c
/* Skip low-level CPU/DDR init — Stepldr initializes DDR before jumping to
 * UBOOT.BIN at 0x30000. Re-running cpu_init_crit here would corrupt the
 * already-configured memory controller and hang the system. */
#define CONFIG_SKIP_LOWLEVEL_INIT
#define CONFIG_SKIP_LOWLEVEL_INIT_ONLY
```

**Change 2 — Fix init stack pointer** (replaced the ifdef block):
```diff
-#ifdef CONFIG_SPL_BUILD
-#define CONFIG_SYS_INIT_SP_ADDR   0xc0008000
-#else
-#define CONFIG_SYS_INIT_SP_ADDR \
-    (CONFIG_SYS_SDRAM_BASE + 16 * 1024 - GENERATED_GBL_DATA_SIZE)
-#endif
+/* Init stack — placed above the U-Boot binary (loaded at 0x30000, ~370KB).
+ * Previously was (SDRAM_BASE + 16K) = 0x3c00 which is INSIDE the binary! */
+#define CONFIG_SYS_INIT_SP_ADDR   0x80000
```

---

## Build Instructions

### Prerequisites

```bash
# Cross compiler (ARM hard-float)
sudo apt install gcc-arm-linux-gnueabihf binutils-arm-linux-gnueabihf

# Python 3 (for header injection)
python3 --version  # must be 3.6+
```

### Full Build Procedure

```bash
cd /home/osboxes/Downloads/linux-arkmicro/u-boot

# Step 1: Clean everything
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- mrproper

# Step 2: Apply configuration (no-SPL flat binary)
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- ark1668_limcet_p305_defconfig

# Step 3: Verify key settings
grep -E "^CONFIG_SPL\b|CONFIG_SKIP|CONFIG_SYS_TEXT_BASE" .config
# Expected:
#   # CONFIG_SPL is not set
#   CONFIG_SYS_TEXT_BASE=0x30000

# Step 4: Build
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- -j$(nproc) 2>&1 | tee build.log

# Step 5: Confirm success
echo $?        # should be 0
ls -lh u-boot.bin u-boot
```

**Expected build outputs:**
- `u-boot` — ELF file (needed by injection script for EP + BSS addresses)
- `u-boot.bin` — Flat binary (no ARK header yet, ~370KB)
- `u-boot.map` — Link map (useful for debugging)

### Step 6: Automated SD Card Population (Recommended)

The main build script `build_bootable_sdcard.sh` has been updated to fully automate U-Boot installation. When the `--new-uboot` flag is passed, it will:
1. Detect your freshly compiled `u-boot.bin` and the DTB file `ark1668_limcet_p305.dtb`.
2. Automatically run `inject_ark_header.py` on the raw `u-boot.bin` to produce a correctly formatted `UBOOT.BIN` in the `sd_bootable/` directory.
3. Mount the target device/image and copy `UBOOT.BIN` and `ark1668_limcet_p305.dtb` to Partition 1 (BOOT).
4. Auto-generate the correct `uEnv.txt` pointing to both the kernel and DTB.

To write directly to an SD card (e.g. `/dev/sdb`):
```bash
sudo ./build_bootable_sdcard.sh --device /dev/sdb --new-uboot --new-kernel
```

To build a raw image file (`sd_bootable/sd_boot.img`):
```bash
./build_bootable_sdcard.sh --image sd_bootable/sd_boot.img --new-uboot --new-kernel
```

---

### Step 7: Manual Copy to SD Card (Fallback)

If copying manually, you must perform the header injection first to generate `UBOOT.BIN`:

```bash
# 1. Inject header (run from the u-boot build directory)
python3 /media/sf_GitHub/prado-firmware-reconstruction/inject_ark_header.py \
    u-boot.bin \
    UBOOT.BIN

# 2. Copy the three required boot files to SD card FAT partition (p1)
sudo cp UBOOT.BIN /mnt/sd/UBOOT.BIN
sudo cp /media/sf_GitHub/prado-firmware-reconstruction/sd_bootable/ark1668_limcet_p305.dtb /mnt/sd/ark1668_limcet_p305.dtb
sudo cp /media/sf_GitHub/prado-firmware-reconstruction/sd_bootable/uEnv.txt /mnt/sd/uEnv.txt
sudo sync && sudo umount /mnt/sd
```

---

## What `inject_ark_header.py` Does

Reads the companion ELF (`u-boot`) for entry point and BSS end, then writes
the ARK header fields into the flat binary at fixed offsets:

```
Binary offset 0x3c ← 0x12345678           (ARK magic)
Binary offset 0x40 ← 0x00030000           (load address, constant)
Binary offset 0x44 ← <ELF entry point>    (where _main lives in RAM)
Binary offset 0x48 ← <ELF entry point>    (duplicate)
Binary offset 0x4c ← <ELF BSS end addr>   (end of zero-init data in RAM)
Binary offset 0x50 ← <u-boot.bin size>    (exact file size)
```

The bytes at these offsets in the unpatched binary are `0xdeadbeef` / `0x0badc0de`
padding words from `arch/arm/lib/vectors.S` — safe to overwrite.

---

## Boot Verification

### ✅ Confirmed First Boot — 2026-07-09

```
Ark1680 SoC Remapped And DDR Init End . . .
Launch Stepldr...
NandStepLoader...
change_clk enable clock successful
SD get card RCA:0x%x
change_clk enable clock successful
Launch UBOOT rom sd...

U-Boot 2018.07-linux4ark_1.0 (Jul 09 2026 - 17:52:10 +1000)

DRAM:  64 MiB
NAND:  128 MiB
MMC:   ARK_MMC0: 0
Loading Environment from NAND...
!!Read Data err more than 8 bit and Group = 0 status:0x4
...
NAND read from offset 120000 failed -74
*** Warning - readenv() failed, using default environment

Failed (-5)
In:    serial
Out:   serial
Err:   serial
Hit space to stop autoboot:  0
=>
```

**All hardware detected correctly:**
- DRAM: 64 MiB ✓
- NAND: 128 MiB ✓
- MMC (SD card): ARK_MMC0 ✓
- UART console: In/Out/Err = serial ✓
- Full U-Boot shell available ✓

### NAND Environment ECC Error (non-fatal)

```
!!Read Data err more than 8 bit and Group = 0 status:0x4
NAND read from offset 120000 failed -74
*** Warning - readenv() failed, using default environment
```

**Root cause:** The U-Boot environment partition at NAND offset `0x120000` either:
- Was previously written with different BCH ECC parameters (from the stock binary)
- Is blank/erased (ECC returns all-0xFF which fails ECC decode)

**Impact:** None for now — default environment is sufficient to boot. U-Boot starts
and operates normally. The NAND env will need to be re-written (`saveenv`) once kernel
boot is confirmed working.

**NAND partition layout** (from `CONFIG_MTDPARTS_DEFAULT`):
```
0x0000000 - 0x0020000  (S-Loader,    128K)
0x0020000 - 0x00a0000  (U-boot,      512K)
0x00a0000 - 0x0120000  (U-boot_back, 512K)
0x0120000 - 0x0160000  (U-boot-Env,  256K)  ← env offset, ECC error here
0x0160000 - 0x01a0000  (arkdata,     256K)
0x01a0000 - 0x05a0000  (kernel,        4M)
0x05a0000 - ...        (rootfs,      106M)
```

### Troubleshooting — hanging after "Starting Uboot"

| Symptom | Likely Cause | Action |
|---------|-------------|--------|
| Hangs immediately | Wrong UART port | UART0 = `0xe4200000`, 115200 8N1 |
| Prints a few chars then hangs | Stack corruption | Raise SP addr to `0x100000` |
| Hangs at NAND init | NAND driver issue | Ignore NAND, check SD boot path |
| No output at all | `SKIP_LOWLEVEL_INIT` not active | Check: `xxd UBOOT.BIN \| head -6` — offset 0x3c must be `78 56 34 12` |

**Quick binary check:**
```bash
python3 -c "
import struct
with open('UBOOT.BIN','rb') as f:
    d = f.read(0x54)
print('magic   :', hex(struct.unpack_from('<I',d,0x3c)[0]))
print('load    :', hex(struct.unpack_from('<I',d,0x40)[0]))
print('ep      :', hex(struct.unpack_from('<I',d,0x44)[0]))
print('filesize:', hex(struct.unpack_from('<I',d,0x50)[0]))
"
```

---

## File Reference

| File | Location | Purpose |
|------|----------|---------|
| `ark1668_limcet_p305_defconfig` | `u-boot/configs/` | Build config (no-SPL, flat binary) |
| `ark1668_limcet_p305.h` | `u-boot/include/configs/` | Board config (SP addr, skip lowlevel) |
| `vectors.S` | `u-boot/arch/arm/lib/` | ARM vector table + exception stubs |
| `start.S` | `u-boot/arch/arm/cpu/armv7/` | CPU startup (lowlevel init — now skipped) |
| `spl_ark1668.c` | `u-boot/arch/arm/mach-arkmicro/` | SPL DDR init (NOT used in this build) |
| `inject_ark_header.py` | repo root | Post-build ARK header injection |
| `patch_uboot.py` | repo root | U-Boot environment variable patching |

---

## Build History / Attempt Log

| Date | Config | Result | Notes |
|------|--------|--------|-------|
| Earlier | SPL enabled, placed `u-boot.bin` | ❌ Hung after "Starting Uboot" | SPL not loaded; DDR re-init failed |
| 2026-07-09 | SPL removed, SKIP_LOWLEVEL_INIT, SP=0x80000, ARK header injected | ✅ **U-Boot console confirmed** | Full boot to prompt, DRAM/NAND/MMC detected |


### Build Output Comparison

| Field | Stock Prado | Our Build |
|-------|-------------|-----------|
| magic | `0x12345678` ✓ | `0x12345678` ✓ |
| load  | `0x00030000` | `0x00030000` |
| ep (board_init_r) | `0x00054ef8` | `0x0003fdd8` |
| bss_end | `0x001d6a78` | `0x00096498` |
| filesize | `0x0005bc88` (375,944 B) | `0x0005a840` (370,752 B) |
| _start offset | `0x60` (96 B header) | `0x2e8` (full stubs) |

The `_start` offset difference (0x60 vs 0x2e8) is expected — our build includes full
exception handler stubs because we're not building with `CONFIG_SPL_BUILD`. This is
harmless: Stepldr jumps to `0x30000` which hits the reset vector, which branches forward
to the actual startup code at `0x2e8`.

