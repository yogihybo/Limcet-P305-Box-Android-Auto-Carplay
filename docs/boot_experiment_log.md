# Boot Experiment Log — SD-Boot Progress

This document tracks the verified boot configurations, test results, and status of running the Prado head unit firmware reconstruction from the SD card.

---

## Verified Configurations

### 1. SD Kernel + NAND Rootfs (Hybrid Boot)
* **Status:**  Verified Working 
* **Date:** 2026-07-08
* **Description:** Loads the kernel (`zImage`) directly from partition 1 (FAT32) of the SD card, but mounts the root filesystem from internal NAND (`ubifs`).
* **Significance:** Confirms that the compiled/reconstructed `zImage` is fully compatible with the board's display timings, clocks, and MCU UART communication, and does not hang at display register initialization.
* **Boot Commands:**
  ```bash
  # Initialize and select SD card
  mmc dev 0
  
  # Load kernel from SD p1 FAT32 to RAM
  fatload mmc 0:1 0x1000000 zImage
  
  # Set bootargs targeting NAND rootfs
  setenv bootargs console=ttyS0,115200n8 mem=180M earlyprintk=serial ubi.mtd=6 root=ubi0:rootfs rootfstype=ubifs rootwait ro ${mtdparts} screen=${screen}
  
  # Boot the kernel
  bootz 0x1000000
  ```

### 2. Pure SD Boot (SD Kernel + SD Rootfs via Initramfs)
* **Status:**  Failed / Unsupported by Kernel 
* **Date:** 2026-07-08
* **Description:** Attempted to boot both the kernel and mount the root filesystem (`ext4` on partition 2) from the SD card using a `mkimage`-wrapped ramdisk (`uInitrd`). U-Boot loaded the ramdisk successfully, but the kernel ignored it completely because `CONFIG_BLK_DEV_INITRD` is disabled in the kernel build.
* **Note (2026-07-11):** This was originally observed with the reconstructed 4.19.192 zImage. It has since been **confirmed by direct image analysis** that the *dumped stock* Linux 3.4.0 kernel also has `CONFIG_BLK_DEV_INITRD` disabled — so **neither** kernel can load an initramfs/initrd as-is. See [Dumped Stock Kernel (Linux 3.4.0) — Direct Image Analysis](#dumped-stock-kernel-linux-340--direct-image-analysis-2026-07-11) below.
* **Boot Commands:**
  ```bash
  # Initialize and select SD card
  mmc dev 0
  
  # Load SD kernel and wrapped initramfs
  fatload mmc 0:1 0x1000000 zImage
  fatload mmc 0:1 0x2000000 uInitrd
  
  # Set working bootargs
  setenv bootargs console=ttyS0,115200n8 mem=180M earlyprintk=serial ubi.mtd=6 root=ubi0:rootfs rootfstype=ubifs wait ro ${mtdparts} screen=${screen}
  
  # Boot kernel with uInitrd (ATAG_INITRD2 passed but ignored by kernel)
  bootz 0x1000000 0x2000000
  ```

---

## Pending and Completed Experiments

| Experiment | Purpose | Status | Boot Command Setup / Findings |
|------------|---------|--------|---------------------|
| **Direct SD Rootfs Boot** | Test if the SD host controller driver `ark_dw_mmc` is built-in. | **Failed** | Hanging at `display reg init` because `rootwait` causes an infinite loop. The MMC host driver is indeed a loadable module, so `/dev/mmcblk0p2` never appears. |
| **Initramfs SD Rootfs Boot** | Test booting SD rootfs using the `initramfs.cpio.gz` to load the MMC driver. | **Failed / Unsupported** | The initramfs `uInitrd` loads successfully in U-Boot via `mkimage`, but the stock kernel ignores it completely because `CONFIG_BLK_DEV_INITRD` is disabled in the kernel build. The kernel always falls back to the NAND rootfs (or panics with `VFS: Unable to mount root fs` if the NAND rootfs is not specified). |

---

## Troubleshooting History

### Wrong Ramdisk Image Format
* **Symptom:** `Wrong Ramdisk Image Format` / `Ramdisk image is corrupt or invalid` when running `bootz 0x1000000 0x2000000:${filesize}`.
* **Cause:** U-Boot expects ramdisks passed as parameters to have a legacy U-Boot image header (`mkimage`).
* **Solution:** Wrap the raw cpio archive using `mkimage` to generate a `uInitrd`:
  ```bash
  mkimage -A arm -O linux -T ramdisk -C gzip -d initramfs.cpio.gz uInitrd
  ```
  And then boot using `bootz 0x1000000 0x2000000`. Passing raw `cpio.gz` files via `initrd=` in `bootargs` is ignored by the kernel on this ATAG-based system.


---

## Compiled 4.19.192 Kernel — Boot-Log Progression (2026-07-08 → 07-10)

Cross-review of the seven `docs/new kernel bootlog*.txt` captures. All are
`Linux 4.19.192` (gcc 12.2.0), machine model `Limcet P305/P306`, from the
`linux-arkmicro` BSP. Two series: a patched-stock/SD-boot series (`v1`–`v4`)
and a freshly-compiled-U-Boot series (`new uboot` = `nu-v1`–`nu-v3`).

| Log | Build / time | cmdline | Boot reached | Touch | NAND bad-blk | GPU+WiFi captured |
|---|---|---|---|---|---|---|
| v1 | #7  Jul-08 22:43 | full `mtdparts`, `screen=0` | **panic** — init SIGSEGV (`kill init 0x0b`) | — | — | — |
| v2 | #8  Jul-09 00:11 | full `mtdparts`, `screen=0` | **panic** — `No working init found` | `0-005d` **-121** | 1 | — |
| v3 | #13 Jul-09 09:30 | full `mtdparts`, `screen=0` | `login:` prompt | `0-005d` -121 | 1 | — |
| v4 | #13 (same) | full `mtdparts`, `screen=0` | **MSN app starts** (`init msn … ticks: 45`) | `0-005d` -121 | 1 | — |
| nu-v1 | #13 Jul-09 18:54 | minimal, **no `root=`** | died in NAND scan, no root mount | — | 1 | — |
| nu-v2 | #13 Jul-09 22:19 | full `mtdparts`, `screen=` (empty) | `/ #` shell | `1-005d` **-6** | **417** | — |
| **nu-v3** | **#15 Jul-10 00:19** | **`${mtdparts}` `screen=${screen}`** | `/ #` shell | `1-005d` -6 | 417 | **LCD+GPU+WiFi AP+client** |

*Caveat:* several captures simply stop where the serial log was cut, so "reached"
is partly capture duration, not a hard capability ceiling.

### Cross-log findings

1. **Touch has never worked, and the new U-Boot made it worse.** SD series: GT911
   on bus **0** (`0-005d`), error **-121** (`-EREMOTEIO`, device NAKs on a reachable
   bus). New-U-Boot series: GT911 on bus **1** (`1-005d`), error **-6** (`-ENXIO`,
   nothing at that address). Same kernel build #13 in both → the **DTB differs**
   between the two series, and the new-U-Boot DTB moved touch to a worse bus. In
   **no** log does a hardware ARK I2C controller ever register. Root cause pinned
   below.
2. **The latest cmdline is a regression.** `nu-v2` still passed the real
   `mtdparts=ark1680-nand:…` string; `nu-v3` passes literal `${mtdparts}` and
   `screen=${screen}` — U-Boot bootargs variable-expansion broke between the two
   builds. `nu-v1` was worse (minimal cmdline, no `root=`, never mounted root).
   Fix in the new U-Boot: build bootargs with `run` so `${…}` expands, or hardcode
   the values (the DTB already describes panel + partitions, so both tokens can
   simply be dropped).
3. **NAND bad-block count jumped 1 → 417 at the U-Boot switch.** Same kernel #13,
   same `ECC … too weak` warning in every log. The SD series trusted the on-NAND
   factory BBT (1 bad block); the new-U-Boot series does a full rescan and the
   weak ECC mis-marks ~417 good blocks. Root cause (ECC/OOB layout mismatch) is
   constant; only whether a cached BBT is trusted changes. Irrelevant for SD boot,
   but it means the new U-Boot disturbs NAND enough to invalidate the BBT.
4. **LCD framebuffer inits in every log** — most reliable subsystem. GPU
   (`galcore 6.2.4.150331`) and the full WiFi-AP-with-client-EAPOL stack appear
   **only in nu-v3** (first capture of the complete display+GPU+wireless path).
5. **Userspace provisioning gaps** in v4: `sshd: no hostkeys available -- exiting`,
   `ifconfig: … No such device`. Rootfs items, not kernel.

### Touch root cause — DTS puts GT911 on the camera's bit-bang bus

Pinned in `Limcet Hardware/ark1668-limcet-prado.dts`:

- The `gt911: touchscreen@5d` node is a **child of `i2c-gpio-0`** (a bit-banged
  `i2c-gpio` bus on `&gpio0 3` SDA / `&gpio0 2` SCL) — the **same bus as the
  ARK7116 camera decoder** (`dvr_ark7116@59`). Per `KERNEL_BUILD_REFERENCE.md` §8,
  GPIO 2/3 is the **ARK7116 camera** I2C bus; per §4 the GT911 belongs on
  **hardware I2C `&i2c0`**. So the kernel drives touch transactions out the
  camera's SDA/SCL lines — the panel never sees them → NAK / -ENXIO.
- The DTS **never references `&i2c0`** (hardware controller `i2c@e4300000`), so the
  hardware I2C bus is unused and the panel (wired to the hardware-I2C pins) is
  unreachable. This is why no hardware I2C controller appears in any boot log.
- **Compatible mismatch:** the P305 DTS `#include`s `ark1668.dtsi`, whose `i2c0` is
  `compatible = "snps,designware-i2c"` — but `KERNEL_BUILD_REFERENCE.md` §5/§9
  enabled `CONFIG_I2C_ARK`, which matches `ark1668e.dtsi`'s `arkmicro,ark-i2c`,
  **not** the designware controller the board actually includes. So even if `&i2c0`
  were referenced, the controller wouldn't probe without the designware driver.
- `i2c-gpio,scl-output-only` on the touch bus is also the source of the
  `i2c … Not I2C compliant: can't read SCL / Bus may be unreliable` warnings, and
  would break GT911 clock-stretching even on the correct bus.

**Proposed fix (two parts, apply to the build-tree DTS
`linux/arch/arm/boot/dts/ark1668_limcet_p305.dts`, not just the repo copy):**

1. Kernel config: enable `CONFIG_I2C_DESIGNWARE_PLATFORM=y` (the driver for the
   `snps,designware-i2c` controller that `ark1668.dtsi` declares). Keep or drop
   `CONFIG_I2C_ARK` — it matches `ark1668e.dtsi`, not this board.
2. DTS: move `gt911` off `i2c-gpio-0` and onto the hardware bus, and leave the
   ARK7116 camera alone on `i2c-gpio-0`:
   ```dts
   &i2c0 {
       status = "okay";
       clock-frequency = <400000>;
       gt911: touchscreen@5d {
           compatible = "goodix,gt911";
           reg = <0x5d>;
           interrupt-parent = <&gpio0>;
           interrupts = <4 IRQ_TYPE_EDGE_FALLING>;
           irq-gpios = <&gpio0 4 GPIO_ACTIVE_HIGH>;
           reset-gpios = <&gpio2 16 GPIO_ACTIVE_HIGH>;
           touchscreen-inverted-x;
       };
   };
   ```
   **Verify on next boot:** `i2c@e4300000` registers, and `Goodix-TS 0-005d`
   completes its i2c test instead of failing with -6/-121.

   *Fallback if the designware controller won't come up on this SoC* (i.e. the
   `ark1668.dtsi` node is wrong and it's really an `ark-i2c`): give touch its **own**
   dedicated `i2c-gpio` bus on the **GT911's actual SDA/SCL GPIOs** (unknown — the
   reference says hardware `&i2c0`, so the panel is wired to the hardware-I2C pins;
   bit-banging only works if done on those same physical pins muxed as GPIO). Do
   **not** leave it sharing GPIO 2/3 with the camera.

---

## Dumped Stock Kernel (Linux 3.4.0) — Direct Image Analysis (2026-07-11)

The stock kernel from the NAND dump was decompressed and analysed directly, to
settle whether the *dumped* U-Boot + *dumped* kernel can support initramfs
(independently of the 4.19.192 reconstruction).

### Extraction method

`Prado firmware dump/mtd5_kernel/extracted/zImage` is an ARM self-decompressing
zImage whose payload is **LZO-compressed** (not gzip/XZ — this is why binwalk and
`extract-vmlinux` initially failed / fell back to `cat`, and why no valid gzip/XZ
container magic is found). Recovered on a Linux VM:

```bash
# payload starts at the LZO magic just past the ~6.4 KB decompressor stub
binwalk zImage                     # reports: 6420  0x1914  LZO compressed data
tail -c +6421 zImage > payload.lzo
lzop -d -c payload.lzo > vmlinux   # (if lzop rejects the container, use the
                                   #  kernel's own decompressor / vmlinux-to-elf)
```

The decompressed image is saved as `vmlinux` (6,187,748 bytes) alongside the
`zImage` in `Prado firmware dump/mtd5_kernel/extracted/`. Verified genuine
(`VFS:`, `Kernel command line`, `Memory:` strings present).

### Build identity

```
Linux version 3.4.0 (flyound@build-server)
  (gcc version 4.9.4 (Buildroot 2018.08-rc1-00026-gaeef2a9))
  #353 Sat Feb 12 15:55:17 CST 2022
```

- ARMv7 / VFP v0.3. SoC family **ark1680** (matches U-Boot `flyound…` builds).
- Compiler is Buildroot 2018.08 GCC 4.9.4 — **different toolchain** from the 4.19
  reconstruction (GCC 12.2.0), and a different kernel line entirely.
- No `CONFIG_IKCONFIG` (`IKCFG_ST` magic absent) → no embedded `.config`; findings
  below are from decompressed-image string analysis.

### initramfs / initrd support — CONFIRMED ABSENT

`CONFIG_BLK_DEV_INITRD` is **not set**. Every string that this option compiles into
`init/initramfs.c`, `init/do_mounts_initrd.c`, and `arch/arm/mm/init.c` has a count
of **0** in the decompressed `vmlinux`:

| String (present only if `BLK_DEV_INITRD=y`) | Count |
|---|---|
| `Unpacking initramfs` / `Trying to unpack rootfs` | 0 |
| `Freeing initrd memory` | 0 |
| `initrd overwritten` | 0 |
| `Initramfs unpacking failed` | 0 |
| `rootfs image is not initramfs` | 0 |
| `unpack_to_rootfs` / `populate_rootfs` | 0 |

With the option off, the kernel is built from `init/noinitramfs.c` (none of the
above strings). The stray `initrd`/`load_ramdisk=` tokens that do appear are just
entries in a boot-parameter name list, not the unpacking machinery. The `rd_size`
hits were NTFS `record_size` substrings (false positives).

**Conclusion:** the dumped 3.4.0 kernel behaves exactly like the 4.19.192
reconstruction — U-Boot loads & passes the ramdisk (via `ATAG_INITRD2`), the
kernel silently ignores it. Booting SD-ext4-root on the **unmodified** dumped
kernel is impossible; it requires a kernel rebuild (`CONFIG_BLK_DEV_INITRD=y` +
`CONFIG_RD_LZO=y`, **or** simply building `ark_dw_mmc` in for direct SD root).

### Dumped U-Boot 2012.10 — supports ramdisk (for contrast)

`Prado firmware dump/mtd1-mtd2_uboot/extracted/uboot.bin`
(`U-Boot 2012.10 flyound2021123014`) contains the full `boot_get_ramdisk()` path:
`## Loading init Ramdisk from Legacy Image`, `RAMDisk Image`,
`Wrong Ramdisk Image Format`. It already loaded `uInitrd` successfully in the
experiments above. The bootloader is **not** the blocker — the kernel is.

### Driver / feature inventory (from image strings)

Relevant to any SD-boot or rebuild decision:

| Subsystem | Status in dumped 3.4.0 kernel |
|---|---|
| **ext4** | built-in (`EXT4-fs …`) |
| **vfat / FAT** | built-in (`FAT-fs`) |
| **UBIFS** | built-in (primary rootfs on NAND) |
| **NTFS** | built-in (read side; USB media) |
| **MMC core + `mmcblk` block** | built-in |
| **SD host controller `ark_dw_mmc`** | **module** — `/lib/modules/3.4.0/…/ark_dw_mmc.ko`, no deps. Loaded by userspace ~t=14 s; **not** available at root-mount time. This is the sole reason SD-root needs early module loading. |
| Touch | Goodix `Goodix-TS` on a bit-banged `i2c-gpio` bus |
| Camera decoder | `rn6752` (automotive AHD/CVBS decoder) |
| CAN | Bosch `d_can` controller |
| Framebuffer / LCD | `arkfb` (`LCD_PANEL_PARALLEL_16/18/24BIT`, CPU/SRGB panel types) |
| GPU | Vivante `galcore` (`galcore.ko` module) + `hx170dec` VPU module |
| Wireless | Realtek modules (`wlan_rtl8189fs/8811cu/8821cs/8821cu/8822cs`) |
| USB | MUSB gadget/host (`ark1680_musb`), mass-storage, NCM/EAP/webcam gadgets |
| Crypto | aes, arc4, cbc/ecb, des, crc32, `ansi_cprng` |
| I2C / WDT / DMA / RTC / SPI | ArkMicro on-SoC controllers built-in |

- **Default command line:** `CONFIG_CMDLINE` is not baked in — bootargs come
  entirely from U-Boot (consistent with the SD-boot `setenv bootargs …` flow).
- **NAND:** `ark1680-nand` with configurable ECC + on-flash BBT (the ECC-strength
  mismatch that inflates the 4.19 bad-block count is an ECC-layout issue, not a
  kernel-capability one).

### ELF reconstruction & symbol recovery (2026-07-11)

The decompressed `vmlinux` has **no `CONFIG_IKCONFIG`**, but it **does** have
`CONFIG_KALLSYMS`. The built-in kallsyms tables were parsed directly to recover a
full symbol table and rebuild a loadable ELF (the standard `vmlinux-to-elf` tool
couldn't be used on the Windows host — its `minilzo` dep won't build without a C
toolchain — so the tables were decoded by hand):

- Text base **`0x80008000`** (`PAGE_OFFSET=0x80000000`, `TEXT_OFFSET=0x8000`).
  `kallsyms_addresses` @ file `0x456df0`, **`kallsyms_num_syms = 34116`**.
  `kallsyms_names` @ `0x478310`, `token_table` @ `0x4d5c40`. Note every kallsyms
  sub-table is **16-byte aligned with zero padding** between them (the gotcha that
  makes naïve offset math land a few bytes off).
- Output artifacts (in `Prado firmware dump/mtd5_kernel/extracted/`):
  - `System.map` — 34,116 symbols, `addr type name` (100 % decoded cleanly).
  - `vmlinux.elf` — ELF32 LE ARM EABI5, single `.text` @ `0x80008000` +
    `.symtab`/`.strtab`. `file` reports *"not stripped"*; loads into
    Ghidra/IDA/`arm-*-objdump` with symbols for full disassembly.

**Memory map (virtual):** `_stext=0x80008200` · `_etext=0x80599624`
(~5.9 MB text) · `__init_begin=0x8059a000` · `_einittext=0x805b0a90`.

#### Symbol-level confirmation of module vs built-in

This supersedes the earlier string-based inference with hard symbol evidence:

| Driver | Symbols in vmlinux | Verdict |
|---|---|---|
| SD host controller `dw_mci_*` (probe/request/interrupt) | **0** | **module** (`ark_dw_mmc.ko`) — the board only provides platform glue (`ark1680_dwmci_init`, `ark_sdmmc_get/set_rate`) |
| MMC core (`mmc_alloc_host`, `mmc_add_host`, `mmc_attach_sd`, `mmc_blk_probe`) | present | built-in |
| ext4 (`ext4_fill_super`, `ext4_mount`) | present | built-in |
| Realtek Wi-Fi (`rtl8189/8811/8821/8822`) | 0 | modules |
| Vivante GPU (`galcore`/`gck*`) | 0 | modules |
| Goodix touch (`goodix`/`gt9xx`/`gtp_`) | 0 | modules |

The SD-root blocker is therefore proven at the symbol level: the one driver needed
before root-mount (`dw_mci`) is **not in the kernel**, and the mechanism to load it
early (initramfs) is **compiled out** — a kernel rebuild is unavoidable for SD-root.

#### Other notes from the symbol table

- NAND stack: standard `nand_*` with HW-ECC / SW-ECC / syndrome paths + on-flash
  **BBT** (`nand_scan_ident/tail`, `create_bbt`, `read_abs_bbts`,
  `nand_read_page_hwecc*`). Exact ECC strength/OOB layout is now disassemblable
  from `vmlinux.elf` (`ark1680_nand`/`ark_nand` probe) for a byte-exact match
  against the 4.19 build's ECC.
- **YAFFS** ECC/tags routines are built in (`yaffs_ecc_*`, `yaffs_*_tags_ecc`) — the
  stock kernel can drive a YAFFS NAND image in addition to UBIFS.
- On-SoC ArkMicro controllers built in: `ark1680_add_device_{nand,uart,hsuart,i2c,
  spi,dma,rtc,pwm,wdt,ts,gpio}`, display `ark_fb`/`ark_disp_*` (incl. TV-encoder
  `ark_disp_tvenc_cvbsss_init_nts`), audio `ark_pcm_*`.

### Touch (GT911) pins, I²C address & LCD panel timing — from disassembly (2026-07-11)

Recovered from `vmlinux.elf` (capstone/Ghidra) and the touch module
`mtd6_rootfs/lib/modules/3.4.0/kernel/drivers/input/touchscreen/gt9xx/gt9xx.ko`.

#### GT911 touch (module `gt9xx.ko`)

Decoded the `gtp_reset_guitar` reset/probe sequence directly from ARM code:

| Signal | GPIO | How derived |
|---|---|---|
| **INT** (`GTP_INT_IRQ`) | **GPIO 4** | driven output during reset, then `gpio_direction_input(4)`; IRQ = `gpio_to_irq(4)`, `request_threaded_irq` flags `#2` = **falling edge** |
| **RST** (`GTP_RST_PORT`) | **GPIO 80** (`0x50`) | `gpio_direction_output(80,0)` → set INT level → `(80,1)` release → left as **input** (open-drain + external pull-up) |

**I²C address select** (`gtp_reset_guitar`): reads `client->addr` (`ldrh [r4,#2]`),
`cmp #0x14`, then drives **INT high if addr==0x14, INT low otherwise (→ 0x5d)** while
RST is low — the standard Goodix GT911 address-strap protocol. Boot logs show the
client at **`0x5d`** (`0-005d` / `1-005d`), i.e. INT driven **low**. To use `0x14`
instead, register the I²C client at 0x14 (the driver then drives INT high).

Reset timing: RST low → `msleep(delay)` → set INT strap → `msleep(2)` → RST high →
`msleep(6)` → RST input → `gtp_int_sync(50)`.

**Cross-check with the 4.19 DTS:** INT=`gpio0 4` (falling), RST=`gpio2 16` = bank2·16
= **80** — the stock module and the DTS agree **exactly** on pins and IRQ polarity.
This confirms the touch failure documented above is purely the **I²C bus** mismatch
(bit-banged camera bus vs hardware `i2c0`), *not* the pin/reset config. The module
ships **no** register config table — it relies on the config flashed inside the
GT911 chip (`GTP read version` / `cfg_version`), so there is nothing panel-specific
to port from firmware beyond the pins/address above.

Config is split: pin macros (`GTP_INT_IRQ`/`GTP_RST_PORT`) are compiled into the
module; panel resolution comes from the exported kernel globals `touchinfo_param` /
`screeninfo_param` that the module imports; the GT911 scan/threshold registers live
in the chip.

#### LCD panel timing (built-in kernel `.init.data` @ `0x805e6190`)

Consumed by `ark_disp_set_lcd_cfg` (`0x802dc138`). Raw `u32` fields:

| Field | Value |
|---|---|
| Active resolution | **800 × 480** |
| Horizontal blanking (FP/BP/sync) | `40, 36, 16` → **Htotal 892** |
| Vertical blanking (FP/BP/sync) | `32, 32, 41` → **Vtotal 585** |
| Implied pixel clock | 892 × 585 × 60 ≈ **31.3 MHz** |
| Trailing config words | `00 01 01 00 · 08 0c 01 00 · 0d 01 07 00 · 20` — sync polarity / bus format / bpp / backlight; exact field→register order is in `ark_disp_set_lcd_cfg` |

(The blanking *triples* are certain from the totals/clock sanity-check; the exact
order within each triple, and the trailing flag semantics, would need tracing
`ark_disp_set_lcd_cfg`'s register writes.)

#### TV-encoder mode table (`.rodata` @ `0x805e65bc`)

Feeds `ark_disp_tvenc_*`. Five-word entries `{w, h, htiming, vtiming, interlace}`:
**720×480** (NTSC), **720×576** (PAL), **1280×720** (720p).

#### `screeninfo_param` / `touchinfo_param`

Both are **120-byte `.bss` structs** (`0x805eecd0` / `0x805eede0`), zero-initialised
and populated at runtime (`screen_id_setup` `memcpy`s 120 bytes into `screeninfo_param`).
They are **not** static geometry tables — the LCD geometry above is the real source.
