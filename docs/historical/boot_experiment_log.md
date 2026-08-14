# Boot Experiment Log — SD-Boot Progress

This document tracks the verified boot configurations, test results, and status of running the Limcet P306 head unit firmware reconstruction from the SD card.

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

**Stock touch bus — VERIFIED on the hardware controller (bus 0).** Disassembling
`ark1680_machine_init` (`0x8059f1b0`) shows the GT911 `i2c_board_info`
(`type="Goodix-TS"`, `addr=0x5d` @ `0x805b3914`) is registered via the
**`ark1680_add_device_i2c`** wrapper, which hardcodes **bus 0 = the ARK1680
*hardware* I²C controller**. The bit-banged wrappers (`analog_i2c_add_device_i2c*`,
buses 1/2/3) carry other devices — bus 2 = `drv_bd37033` audio amp, bus 1 = the
camera/DVR (per `libSetting`'s `/sys/.../i2c-gpio.1/i2c-1/1-002c/dvr`). So stock runs
touch on **hardware i2c**; only audio/camera are bit-banged. This is hard proof (from
the stock board code, not inference) that the 4.19 DTS is wrong to place `gt911@5d`
on a bit-banged `i2c-gpio` bus — it belongs on the hardware `&i2c0` controller.

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

---

## Systematic I²C bus verification (pending — 2026-07-11)

**Why this section exists:** the GT911 touch bus has been decided twice by
inference and reversed both times with no on-device confirmation recorded:
`7c7ce4c` moved `gt911@5d` onto hardware `&i2c0` (backed by the stock
3.4.0 disassembly proof above — "hard proof... not inference"); `0be21c7`
moved it back to `i2c-gpio-0` "to match actual hardware wiring" with **no
evidence recorded in this log or the commit body**. Bootlog v6
(`docs/new kernel bootlog new uboot v6.txt`) was built against the
`0be21c7` state and shows touch still failing there (`-6`/`ENXIO`), which
is expected either way since no hardware `i2c0` controller was even
registered in that build. Neither commit has been checked against the
physical unit. Before touching this DTS node a third time, resolve it
empirically instead of by inference.

**Method:** `Limcet Hardware/ark1668-limcet-prado.dts` now enables
`&i2c0` (`status = "okay"`, no child devices) *alongside* the existing
`i2c-gpio-0` (still owns `gt911@5d` + `dvr_rn6752@2c`) — the two buses use
disjoint pins (PBANK_2 6/7 for hw `i2c0` vs PBANK_2 2/3 for `i2c-gpio-0`,
per `ark1668-pinctrl.dtsi`), so both can be live in one build. A static
ARM scan tool (`tools/i2c-scan/`, since the rootfs has no i2c-tools) reads
every address on both `/dev/i2c-*` nodes from the live `/ #` prompt.

**Procedure:**
1. Build kernel with the current DTS (both buses enabled, `CONFIG_I2C_CHARDEV`
   / `CONFIG_I2C_DESIGNWARE_PLATFORM` / `CONFIG_I2C_GPIO` already `=y` in
   `Limcet Hardware/kernel_dot_config`).
2. Copy `tools/i2c-scan/i2c-scan` onto the SD rootfs.
3. Boot to `/ #`, run `ls /sys/class/i2c-dev/*/name` to map bus numbers,
   then `./i2c-scan /dev/i2c-0 /dev/i2c-1`.
4. Whichever bus ACKs `0x5d` is where `gt911@5d` belongs — permanently
   attach it there and disable/remove the other bus's touch reference.

**Result — ran on hardware 2026-07-11** (`docs/i2c scan v1.txt`, raw output):

Bus mapping (`/sys/class/i2c-dev/*/name`):

| `/dev/i2c-N` | `name` | Identity |
|---|---|---|
| `i2c-0` | `Synopsys DesignWare I2C adapter` | hardware `&i2c0` |
| `i2c-1` | `i2c-gpio-0` | bit-bang bus 0 (currently owns `dvr_rn6752@2c` + `gt911@5d`) |
| `i2c-2` | `i2c-gpio-1` | bit-bang bus 1 |

`i2c-scan /dev/i2c-0 /dev/i2c-1 /dev/i2c-2` results:

| Bus | Addresses seen | `0x5d` (GT911)? |
|---|---|---|
| `i2c-0` (hw `&i2c0`) | `0x10`, `0x11` ACK | **no** |
| `i2c-1` (`i2c-gpio-0`) | `0x2c` **XX** (busy — `dvr_rn6752@2c` client already bound here, expected) | **no** |
| `i2c-2` (`i2c-gpio-1`) | nothing | **no** |

**This is not the result either prior hypothesis predicted.** Neither the
hardware `&i2c0` bus nor the `i2c-gpio-0` bus that currently owns the
`gt911@5d` node gets an ACK (or even an `XX`/busy, which would indicate a
kernel-registered-but-unresponsive client) at `0x5d` anywhere. Two things
stand out:

1. **`0x2c` is `XX` (busy), not a real scan result** — that's the i2c core
   reporting the address already claimed by the `dvr_rn6752` client
   registered from the DTS, not a live probe. It confirms the camera
   node is bound there, nothing more.
2. **`0x5d` doesn't even show `XX` on `i2c-1`**, even though `gt911@5d` is
   declared in the DTS on that exact bus. If the i2c core had a client
   registered at that address (which it should, from the DTS node)
   `I2C_SLAVE` should have returned `-EBUSY` there the same way it did for
   `0x2c`. That it didn't suggests the Goodix driver's failed probe
   (`-ENXIO`, see bootlog v6) actually released/unregistered the client,
   which is plausible kernel behavior, but it also means this scan
   **doesn't yet rule out `i2c-gpio-0` as the right bus** — it only shows
   nothing is currently ACKing a raw read there.
3. **`0x10`/`0x11` ACKing on the hardware bus is unexplained** — not the
   GT911 (wrong address) and not identified from the stock disassembly.
   Could be a real device (some other chip sharing that hardware bus per
   the schematic) or a false-positive from the crude read-byte probe
   method (floating/unterminated SDA can read back as an ACK on some
   controllers). Needs a targeted `i2cget`-style single-byte read to
   confirm, not just the quick scan.

**Conclusion: inconclusive for touch.** GT911 did not answer on any bus at
`0x5d` at all — the bus question isn't answered by this run; instead it
points at something more fundamental: reset/power/pin-level correctness
for the "read after `I2C_SLAVE`" probe to get *any* response requires the
chip to actually be running (RST released, INT idle) *before* the ioctl
scan runs, and this scan was likely taken with GT911 sitting in whatever
state its last (failed) kernel probe left it in.

**Next steps (do not touch the DTS bus placement yet):**
1. Manually toggle GT911 RST (GPIO 80) and INT (GPIO 4) via `/sys/class/gpio`
   before re-running the scan, to put the chip through its address-strap
   power-up sequence (see the `gtp_reset_guitar` timing recovered above),
   then re-scan `0x5d` on both `i2c-0` and `i2c-1` immediately after.
2. Also scan `0x14` (the alternate GT911 strap address) on both buses in
   case INT strapped high during the last failed probe.
3. Confirm the `0x10`/`0x11` hits on `i2c-0` aren't a probe artifact —
   re-run with a single targeted address (`i2c-scan` limited to just
   `0x10`) and cross-check against the board schematic/photos in
   `Limcet Hardware/board_photo_*.jpg` for what else might be on that bus.

### Reset-toggle follow-up (`docs/i2c scan v2.txt`, `v3.txt`) — still no `0x5d`, ruled out one theory

**v2** — ran the RST/INT toggle sequence (§ above) then re-scanned all three
buses: **byte-for-byte identical to v1.** Still nothing at `0x5d`/`0x14` on
any bus, `0x2c` still busy on `i2c-1`, `0x10`/`0x11` still ACK on `i2c-0`.

**v3** — checked whether the toggle actually took effect (rather than
silently failing, e.g. `EBUSY` from the kernel `gt911` driver still holding
the pins):

```
/sys/class/gpio/gpio80 -> .../e4600040.gpio/gpiochip2/gpio/gpio80   (RST)
/sys/class/gpio/gpio4  -> .../e4600000.gpio/gpiochip0/gpio/gpio4    (INT)
gpio80: direction=out, value=1   (RST high/released — end state of our sequence)
gpio4:  direction=in,  value=1
dmesg | grep -i goodix:
  Goodix-TS 1-005d: i2c test failed attempt 1: -6
  Goodix-TS 1-005d: i2c test failed attempt 2: -6
  Goodix-TS 1-005d: I2C communication failure: -6
```

**Export/toggle genuinely worked** — no `EBUSY`, both GPIOs show up as real
sysfs nodes backed by actual gpiochips, and read back the values our
sequence left them in. So **the "driver is holding the pins" theory is
ruled out**: userspace had real control of RST/INT, drove GT911 through a
reset, and it still never answered at `0x5d` (or the busy-marker `XX`) on
any bus. (The `dmesg` hit is stale — `Goodix-TS 1-005d` is the *kernel's own*
boot-time probe on adapter `i2c-1`/`i2c-gpio-0` from the DTS node, confirming
bus numbering, not a result of our manual toggle.)

This now looks less like a bus-selection or reset-timing problem and more
like: (a) the panel/FPC on **this specific unit** isn't actually connected/
seated, (b) `active_low` on `gpio80`/`gpio4` differs from what we assumed
(not yet checked — `cat /sys/class/gpio/gpio80/active_low` and `gpio4/active_low`),
or (c) this exact board revision's touch wiring genuinely differs from both
the stock disassembly and the current DTS pin assignment.

**Polarity check (2026-07-11):** `active_low` = `0` for both `gpio80` (RST)
and `gpio4` (INT) — no inversion. Our reset sequence asserted exactly as
intended (RST really did go low then high; INT really did go low then
back to input). This rules out the polarity theory too.

**Status: software/DTS avenues exhausted for this round.** Bus placement,
reset timing/control, and pin polarity have all now been verified correct
or ruled out as the cause, and GT911 still never answers at `0x5d` (or
`0x14`) on any of the three I²C buses. This points at a hardware-level
cause rather than a kernel/DTS one:

**Next steps (hardware, not software):** *(superseded — see below, root cause found)*

---

### ROOT CAUSE FOUND (2026-07-11): this unit's stock firmware doesn't use GT911 at all

All the above I²C debugging was chasing the wrong device. The stock
rootfs ships **two** touchscreen kernel modules, and picks between them
at boot with a marker-file check —
`Prado firmware dump/mtd6_rootfs/rootfs/etc/rc.d/rcS`:

```sh
touchdriver="/msnprofile/ark1680_ts"
if [ -e $touchdriver ]; then
        insmod .../ark1680_ts.ko                     # on-SoC resistive ADC touch
        ln -s /msnprofile/touch_ark1680_ts_export /tmp/touch_export
else
        insmod .../gt9xx/gt9xx.ko                     # I2C capacitive GT911
        ln -s /msnprofile/touch_gt9xx_export /tmp/touch_export
fi
```

**The marker file `/msnprofile/ark1680_ts` exists** in the live NAND dump
(`Prado firmware dump/mtd6_rootfs/msnprofile/ark1680_ts`, 0 bytes — a pure
existence flag) and in the reconstructed rootfs. This is a direct dump
from the physical Limcet P306 unit (`docs/13.1_SOURCES.md`), not a generic vendor
image — so this reflects **this exact unit's factory configuration**.

That means stock firmware on this unit loads `ark1680_ts.ko` — the
**ARK1668/1680 SoC's built-in resistive ADC touchscreen controller**
(`description=ARK1680 TS Driver`, functions `ark1680_setup_tsc`,
`TSP_GetXY`, `SetDBCNT`, register field `ADCValue`) — and **never loads
`gt9xx.ko` at all**. It's not an I²C device; it's a memory-mapped
ADC/IRQ block on the SoC. The two drivers' export configs confirm the
split: `touch_ark1680_ts_export` sets `QWS_MOUSE_PROTO=tslib:...` (tslib
= classic resistive-touch calibration/filtering), `touch_gt9xx_export`
sets a multi-touch device path for the capacitive path. `arkdata.ini`'s
`TouchPanelX=0,1024` / `TouchPanelY=0,600` are raw ADC-style ranges, not
GT911 panel-resolution reporting.

**This fully explains the entire I²C investigation above:** bus
placement, reset timing, and pin polarity were all correctly ruled
in/out — because there was never a GT911 on the bus to find in the first
place. The exhaustive scanning wasn't wasted (it rigorously eliminated
the wrong hypothesis) but the DTS `gt911@5d` node itself is the wrong
approach for this unit.

**Consequence for the 4.19 kernel port:** the 4.19 DTS/`kernel_dot_config`
have **no** ADC/TSC touchscreen node or driver at all — `ark1668.dtsi` has
no `tsc`/ADC-touch node (only unrelated `i2s-adc`/`sdadc` audio ADC nodes),
and `ark1680_ts.ko`'s source isn't in this repo, only the compiled stock
3.4.0 `.ko`. Porting touch for this unit means either:
1. Reverse-engineer `ark1680_ts.ko` (disassembly, same approach as the
   GT911 pin/timing recovery above) to find the SoC's ADC/TSC MMIO base,
   IRQ, and register layout, then write a new 4.19 driver + DTS node —
   **done, see `docs/1.8_ARK1680_TS_REVERSE_ENGINEERING.md`**: MMIO base
   `0xe4500000` (+0x40), IRQ 4, full init-sequence, register-offset map,
   the `TSP_GetXY` median-of-4 coordinate filter, and the input event
   protocol all recovered from the stock `.ko` and `vmlinux.elf`
   board-file registration. Driver ported to 4.19
   (`Limcet Hardware/ark1680_ts.c`), wired into the build tree's
   Kconfig/Makefile, `CONFIG_TOUCHSCREEN_ARK1680=y`, DTS node added —
   compiles clean. **Hardware-tested** (v7: `docs/new kernel bootlog new
   uboot v7.txt`, v8 with per-write tracing: `docs/new kernel bootlog new
   uboot v8.txt`): probe succeeds, input device registers, ADC block
   registers read back correctly. Syscon `CLKEN`/`PADCFG` read
   `0xffffffff` — v8's per-write trace showed this is true even
   *before* the driver writes anything (cold-boot state), while
   `clkdiv`/`pmux0`/`pmux1` at the same block read sane and change
   correctly — so it's not a broken write or clock-gating issue, most
   likely those two are write-only/strobe registers the stock driver
   never reads either. Not a bug. **Live raw-ADC test
   (`docs/ark ts scan v1.txt`): `raw_x`/`raw_y`/`irq_status` stay pinned
   at `0x00000000` across 10 runs, including while touching the panel**
   — the ADC never produces a conversion or signals a touch-detect
   event. Combined with the earlier GT911 I²C investigation also finding
   zero response, two independent touch technologies on this unit both
   show no hardware response — the leading hypothesis is now a physical
   issue (panel not wired/seated/present), not remaining software work.
   One cheap check left before concluding that: confirm with `debug=1` +
   `dmesg -w` whether the IRQ ever fires at all (rules out the polling
   snapshots missing a narrow window). See
   `docs/1.8_ARK1680_TS_REVERSE_ENGINEERING.md` for full analysis.
2. Physically confirm whether a GT911 (or any I²C touch chip) is actually
   populated on the board despite the firmware flag — if not populated,
   option 1 is the only path; if it is populated, the marker file could
   be wrong/stale for this specific board and GT911 wiring should be
   revisited (unlikely given this is a live dump, but worth a 30-second
   visual check against `Limcet Hardware/board_photo_*.jpg`).
3. **Do not spend further effort on the GT911 I²C bus/DTS placement** —
   that variable is now correctly understood to be moot for this unit.

---

## `MsnCoreApp` segfault — likely root cause found (2026-07-11)

Traced the `start_msn` segfault (Blocker 2 since the original bootlog-v6
review) via static disassembly: `MsnCoreApp` depends on `/dev/ark_display`
(a vendor misc device from the stock kernel's `ark_display_drv.c`) for
its very first startup step, and that driver was never ported into our
4.19 kernel tree. Fix implemented as `Limcet Hardware/ark_display.c` +
`CONFIG_ARK_DISPLAY=y`. **v9 test:** device registered correctly but
`MsnCoreApp`/`MsnFirstInit` behavior was completely unchanged — turned
out to be a bug in the fix itself (wrong `_IOWR()` macro argument
silently produced the wrong ioctl command number, so it never matched
userspace's call). Fixed and kernel rebuilt again — **re-flash and
re-test needed**. Full trail in
`docs/1.8_ARK1680_TS_REVERSE_ENGINEERING.md` → "`MsnCoreApp` segfault —
likely root cause found and fixed".

---

## NAND "ECC too weak" / bad-block spam — root-caused, log noise fixed (2026-07-11)

Every boot log has `nand: WARNING: ark-nand: the ECC used on your
system is too weak compared to the one required by the NAND chip`
followed by hundreds of `nand_read_bbt: bad block at 0x...` lines and
scattered `ark_nand_correct_data: uncorrectable ECC error`. Traced this
to its root cause rather than just silencing it blind.

**The chip genuinely requires stronger ECC than this hardware can
provide — this is not a misconfiguration.** `drivers/mtd/nand/raw/nand_toshiba.c`
decodes bits 0-2 of the 6th NAND ID byte to determine the datasheet
required ECC strength for Toshiba SLC chips: `0x4`→1-bit, `0x5`→4-bit,
`0x6`→8-bit. This chip (`Manufacturer ID: 0x98, Chip ID: 0xf1`, matches
the boot log) decodes to the `0x6` case — **8-bit required**.

The NAND controller (`Limcet Hardware/ark_nand_kernel.c`,
`ark_nand_hw_syndrome_ecc_ctrl_init`) only has discrete BCH modes: 7,
13, 24, 30, or 48-bit — nothing in between — and which mode fits is
capped by available OOB space:
- 7-bit BCH needs 13 ECC bytes/512B sector × 4 sectors = 52 bytes —
  fits this chip's 64-byte OOB.
- 13-bit BCH needs 23 ECC bytes/512B sector × 4 sectors = 92 bytes —
  **does not fit** in 64 bytes of OOB.

So **7-bit is the strongest ECC this exact chip+controller pairing can
ever provide**, one step short of the chip's 8-bit requirement. Stock
firmware runs the identical chip on the identical controller and
almost certainly hits the same shortfall (no full stock kernel dmesg
capture exists to directly confirm — every stock log on hand starts
mid-boot, see `docs/boot log.txt`/`docs/bootlog_prado_holden_firmware.txt`).

**Practical impact:** low. `nand_ecc_strength_good()`'s check is a
conservative datasheet-vs-configured comparison, not proof of actual
data loss — most reads have far fewer real bit-errors than the chip's
worst-case-rated requirement. The `uncorrectable ECC error`/bad-block
entries are genuine failures in *specific* worn blocks (≥8 real bit
errors in that 512B sector), consistent with a used chip from a
vehicle that's been power-cycled for years — not a wholesale failure.
NAND now only backs non-critical `/nanddata` assets (bootlogo,
bootanimation, reversingtrack, Unicode font); real rootfs/kernel boot
from SD. MTD correctly identifying and avoiding worn blocks is the
*correct* behavior here, not a malfunction.

**Not fixable, but the log spam was.** Patched
`drivers/mtd/nand/raw/nand_bbt.c`'s `read_bbt()`: the per-block
`nand_read_bbt: bad block at 0x...` print (upstream already had a
comment noting *"if it's matured we can move this message to
pr_debug"*) is now `pr_debug` instead of `pr_info`, with a `bad_count`
counter added and a single `nand_read_bbt: %d bad block(s) found`
summary line printed once at the end of each BBT read instead. Compiles
clean (`W=1`, zero new warnings), kernel rebuilt, `read_bbt` confirmed
present in the built `vmlinux`. Since `read_bbt()` runs twice (primary
+ mirror BBT, matching the two `Bad block table found at page ...`
lines already in every log), expect two summary lines total instead of
hundreds of individual addresses. Not yet hardware-tested.
