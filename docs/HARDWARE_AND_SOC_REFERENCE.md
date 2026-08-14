# Hardware And Soc Reference

**Status:** Reference
**Last Updated:** 2026-07-15

## Overview
Consolidated document containing: SOC_ARK1668_CROSSREF.md, PIN_MASTER_LIST.md, PIN_BLOCK_DIAGRAM.txt



## SOC_ARK1668_CROSSREF.md

# SoC Identity Cross-Reference & Ghidra Analysis — `mtd5_kernel` (dump build)

Analysed by: LZO extraction + Ghidra 12.1.2 headless disassembly/decompilation, cross-referenced
against the public ArkMicro vendor kernel tree `linux-arkmicro` (ARK1668/ARK1668E/ARKN141 BSP)
and (§7) userspace RE of `MsnCoreApp`/`libMcuCenter.so` from the rootfs.

**⚠️ This document's primary analysis covers a different zImage than `docs/KERNEL_REFERENCE.md`.** See [Build Discrepancy](#build-discrepancy-vs-kernelmd) below — **§6 now cross-checks the `KERNEL.md` build (#383) directly and confirms the findings hold on both.**

**§7 answers the "where does the backup-camera GPIO come from" open item** left in §5: it doesn't — on this product (`McuType=6`, `MCUAdapter_BoxP300`) the signal comes from the companion MCU over the `arktool` UART protocol, not any SoC-side GPIO.

**§8: a synthetic device tree combining every finding here with the physical board inspection in `hardware/BOARD_ANALYSIS.md`** now exists at [`hardware/ark1668-limcet-prado.dts`](../hardware/ark1668-limcet-prado.dts) — not a real boot artifact (this board has no device tree, see §2), but a structured, confidence-tagged writeup of the confirmed hardware layout.

**§9 cross-checks §§2–5 against real ASTRI vendor source** (`ArkPro Reference/`, a public leak, not the public `linux-arkmicro` tree used elsewhere in this doc) — confirms the `config.c` board-init device roster and the `CLCD_TIMING`/`CLKDIV1` register field names, and surfaces a third (unused-on-Limcet-P306) candidate mechanism for backcar detection.

**§10 works out the full pin-mux table** from the real, now-vendored `linux-arkmicro` devicetree source, filling in and correcting `hardware/ark1668-limcet-prado.dts`'s `pinctrl0` node — and finds that an earlier draft's `can0`/`can1` pin-mux entry was wrong: that capability only exists on the newer ARK1668E generation, not this chip.

---

## Build Discrepancy vs `KERNEL.md`

Two different kernel builds exist in this repo's trees:

| | `Prado firmware dump/mtd5_kernel/extracted/zImage` (this doc's primary subject) | `Prado firmware reconstructed/mtd5_kernel/zImage` (`KERNEL.md`, also verified in §6) |
|---|---|---|
| Size | 3,166,512 bytes | 3,255,536 bytes |
| MD5 | `00a7ce78972f56fa47f35817aa531a6e` | `78782daea22d5e22ad90c6e660da75e1` |
| Build # | `#353` | `#383` |
| Build date | Sat Feb 12 15:55:17 CST 2022 | Tue Dec 5 10:50:38 CST 2023 |
| Build host | `flyound@build-server` | `root@build-server` |

These are **not the same kernel** — 30 build numbers and ~22 months apart. The "dump" tree is the raw MTD5 pull straight from a live physical unit; the "reconstructed" tree appears to carry a later build (from a firmware update package or a different source — see `docs/SOURCES.md`). Both target the same SoC/driver family (RN6752, Goodix-TS, ARK1680 identifiers all present in both), so the findings below likely transfer, but **any GPIO/register-offset claim here is only confirmed against build #353**, not #383. Worth re-running the same Ghidra pass against the `reconstructed` zImage to confirm no board-file changes crept in between builds.

---

## 1. Extraction Methodology

The zImage wasn't extractable with the kernel's own `scripts/extract-vmlinux` out of the box because `lzop` wasn't installed on the analysis machine. Fixed by pulling the binary straight out of the Debian package without a full install:

```bash
apt-get download lzop
dpkg-deb -x lzop_*.deb ./lzop_extract
export PATH="$(pwd)/lzop_extract/usr/bin:$PATH"
```

The zImage's decompressor stub embeds **two** LZO magic-byte hits:
- `0x1914` (6420) — a decoy inside the decompressor's own LZO routine object code, **not** the payload (`lzop -d` reports "header corrupted" here).
- `0x1A68` (6760) — the real payload. `lzop -d` on `tail -c+6761 zImage` produces a clean 6,187,748-byte raw binary — this is `vmlinux` **after** `objcopy -O binary` (no ELF header, no symbol table — this is why `extract-vmlinux`'s `readelf` sanity check fails on it even though decompression succeeded).

```
Linux version 3.4.0 (flyound@build-server) (gcc version 4.9.4 (Buildroot 2018.08-rc1-00026-gaeef2a9)) #353 Sat Feb 12 15:55:17 CST 2022
```

Build path embedded in `__FILE__`-derived strings: `/workspace/ark0618system/kernels/linux-3.4/arch/arm/mach-ark1680/clock.c`

No embedded IKCONFIG (`CONFIG_IKCONFIG_PROC` not set) — nothing below comes from a recovered `.config`, all of it is derived from strings/disassembly/decompilation.

---

## 2. SoC Identity: "ARK1680" (software) vs "ARK1668" (chip package marking)

**The physical SoC package on this board is marked ARK1668, but both the kernel and the bootloader self-identify internally as ARK1680.** This was independently confirmed in two separate binaries from the same firmware dump:

**Kernel** (`arch/arm/mach-ark1680/*`, `ark1680_machine_init`, `ark1680-uart`, `ark1680-nand`, `ark1680-spi`, `ark1680-rtc`, `musb-ark1680`)

**U-Boot** (`Prado firmware dump/mtd1-mtd2_uboot/extracted/uboot.bin`):
```
U-Boot 2012.10 (flyound2021123014 - 14:57:05)
nand0=ark1680-nand
mtdparts=ark1680-nand:128k(S-Loader),512k(U-boot),512k(U-boot_back),256K(U-boot-Env),256K(arkdata),4m(kernel),106m(rootfs),6m(userdata),512K(bootlogo),3m(bootanimation),3m(reversingtrack),256K(Unicode)
```

This is consistent across the whole software stack — not a typo. The public `linux-arkmicro` vendor tree (a different ArkMicro BSP export, unrelated to this OEM's `ark0618system` fork) has **no** `ark1680` anywhere; it only knows `ark1668`, `ark1668e`, `arkn141`, `arkn141s`, `amt630h`.

### Machine descriptor (via Ghidra decompilation)

`FUN_8059c13c` in the Limcet P306 kernel is a byte-for-byte match for `setup_machine_tags()` in `arch/arm/kernel/setup.c` — it walks the `__arch_info_begin..__arch_info_end` `struct machine_desc` array comparing the ATAG-supplied machine ID (register r1) against `.nr`, then prints `"Machine: %s"` with `.name`. `FUN_8059c0e4` is the companion "unrecognized machine ID" panic path (`"Available machine support: ID (hex) NAME..."`).

The array has **exactly one entry** (kernel built for a single board, legacy `MACHINE_START(ARK1680,...)`/`MACHINE_END`, no device tree):

```
nr (machine ID) = 0x1068  (4200 decimal)
name             → "ARK1680"
boot_params      = 0x100
fixup            = 0x80008474  (confirmed: calls ioremap-style setup consistent with ATAG fixup signature)
```

Machine ID `0x1068` does **not** appear in `linux-arkmicro/linux/arch/arm/tools/mach-types` — it's a vendor-private ID, never submitted to the official ARM registry (expected/common for closed OEM SoCs, especially post-devicetree-era when that registry effectively froze).

### Architectural alignment: ARK1680 tracks ARK1668, not ARK1668E

The Limcet P306 kernel boots via legacy ATAG/machine-ID (no device tree) and uses a **VIC** interrupt controller:
```
"ARK interrupt controller virtual address VICL %x VICH %x"
```
confirmed live in `FUN_8059f188` (see §4), which does `ioremap(0xe0b00000, 0x200)` / `ioremap(0xe0c00000, 0x200)` for VICL/VICH.

In `linux-arkmicro/linux/arch/arm/mach-arkmicro/Kconfig`:
- `SOC_ARK1668` → `select ARM_VIC` (matches Limcet P306 exactly)
- `SOC_ARK1668E` → `select ARM_GIC`, `HAVE_ARM_ARCH_TIMER` (newer generation, does **not** match)

**Conclusion:** ARK1680 aligns architecturally with **ARK1668** (not the newer ARK1668E), and — per §3 — shares its exact peripheral register map. Most likely explanation: ARK1680 is ArkMicro's internal/pre-devicetree engineering name for the same silicon later formalized and upstreamed as "ARK1668"; this OEM's OS integrator (`flyound`, building for Toyota) never migrated off the old internal name or the legacy ATAG boot path onto the newer `mach-arkmicro/ark1668.c` device-tree support present in the public tree. A remarked/rebadged-die explanation can't be fully ruled out without a datasheet or decap, but is less likely given how deep the software-level match goes (see below).

---

## 3. Peripheral Memory Map — Diff vs `linux-arkmicro/linux/arch/arm/boot/dts/ark1668.dtsi`

Every peripheral base address in `ark1668.dtsi` was checked against the Limcet P306 ARK1680 binary two ways: (a) raw literal-pointer occurrence count, (b) direct `struct resource {start, end, name, flags=IORESOURCE_MEM}` pattern scan (Linux platform-device resource declarations).

| Peripheral | `ark1668.dtsi` address | Found in ARK1680 binary? |
|---|---|---|
| L2 cache controller | `0x70000000` | ✅ (250 refs) |
| VIC (low) | `0xe0c00000` | ✅ |
| VIC (high) | `0xe0b00000` | ❌ (likely computed as offset from VICL, not a separate literal) |
| sregs / pinctrl | `0xe4900000` | ✅ |
| GPIO bank 0 | `0xe4600000` | ✅ |
| GPIO banks 1–3 | `0xe4600020/40/60` | ❌ (likely computed as `bank0_base + i*0x20` at runtime) |
| PWM0 | `0xe4d00000` | ✅ |
| **NAND controller** | `0xec000000` | ✅ (100 refs — heavily used, matches NAND boot design) |
| DMA (dwdma0) | `0xe0000000` | ✅ — exact `struct resource{start=0xe0000000,size=0x400,IORESOURCE_MEM}` match |
| **UART0** | `0xe4200000` | ✅ exact resource-struct match |
| **UART1** | `0xe4e00000` | ✅ exact resource-struct match |
| **UART2** | `0xe8000000` | ✅ exact resource-struct match |
| **UART3** | `0xe8100000` | ✅ exact resource-struct match |
| **UART4** | `0xe4f00000` | ✅ exact resource-struct match |
| **UART5** | `0xe4800000` | ✅ exact resource-struct match |
| Timer | `0xe4a00000` | ✅ |
| Watchdog | `0xe4b00000` | ✅ |
| RTC | `0xe4c00000` | ✅ — including exact size (`0x100`) |
| MMC0 | `0xec400000` | ✅ (1 ref) |
| MMC1 | `0xec800000` | ✅ (33 refs) |
| GPU | `0xe0f00000` | ❌ not referenced at all — likely no GPU driver built for this board (not needed for a basic dash/camera compositor), or the IP block is fused off on this SKU |
| VDEC0 | `0xe0900000` | ✅ (14 refs) |
| I2S DAC / ADC | `0xe4000000` / `0xe8200000` | ✅ both |
| I2C0 | `0xe4300000` | ✅ |

**24 of 26 addresses match exactly** — 8 of them (all 6 UARTs + RTC + DMA) matched as complete `struct resource` entries with identical base **and size**. A 26-address match to this degree across a 32-bit address space is not plausible by coincidence. Combined with §2, this is strong evidence ARK1680/ARK1668 share the literal register layout, not just family lineage.

The 2 misses (GPIO banks 1-3, VIC-high) are explained by runtime offset computation from a base register rather than separate compiled-in literals — not a contradiction, just a scan-method blind spot. GPU is the one genuine absence and is architecturally plausible (unused peripheral on this board variant).

---

## 4. Methodology: Ghidra Headless Setup (for reproducing this analysis)

No GUI available in the analysis environment, and neither Java nor Ghidra were preinstalled. Set up entirely in scratch space, no system/sudo changes:

```bash
# Portable JDK 21 (Ghidra 12.1.2 requires JDK ≥21)
curl -sL -o jdk21.tar.gz "https://api.adoptium.net/v3/binary/latest/21/ga/linux/x64/jdk/hotspot/normal/eclipse"
tar xzf jdk21.tar.gz

# Ghidra 12.1.2 (latest at time of analysis)
curl -sL -o ghidra.zip "https://github.com/NationalSecurityAgency/ghidra/releases/download/Ghidra_12.1.2_build/ghidra_12.1.2_PUBLIC_20260605.zip"
unzip -q ghidra.zip
```

### Base address determination

`vmlinux.bin` is a raw binary (no ELF headers/symbols), so Ghidra needs the correct load address to disassemble cleanly. Determined by testing candidate `PAGE_OFFSET`/`TEXT_OFFSET` combinations against literal pointer references to known strings:

```python
needle = b"Linux version 3.4.0"
off = data.find(needle)          # 0x413078
# test candidate bases; only 0x80008000 produces a hit:
va = 0x80008000 + off            # 0x8041b078
data.count(struct.pack("<I", va))  # == 1  (all 0xC0xxxxxx candidates == 0)
```
Cross-validated with a second string (`"ARK1680"`) — also exactly 1 hit at the same base. This means `PAGE_OFFSET = PHYS_OFFSET = 0x80000000`, `TEXT_OFFSET = 0x8000` — i.e. RAM is mapped 1:1 starting at physical `0x80000000` (no 3G/1G split), typical for embedded ARM SoCs without highmem needs.

Confirmed a third way post-import: Ghidra's auto-analysis found **19,976 clean functions** with correct struct-walk decompilation of the machine_desc array (§2) — a wrong base would produce garbage disassembly, not this.

### Import command

```bash
analyzeHeadless <project_dir> prado_kernel \
  -import vmlinux.bin \
  -loader BinaryLoader -loader-baseAddr 0x80008000 -loader-blockName kernel \
  -processor "ARM:LE:32:v7"
```

Auto-analysis took ~393 seconds. Post-analysis scripts (Java `GhidraScript` subclasses, `-postScript`) were used to enumerate cross-references to known string addresses and decompile the containing functions — see §5 for the GPIO-tracing scripts specifically.

---

## 5. GPIO Pin Assignments

### Numbering scheme

Global GPIO number = `bank*32 + pin` (4 banks × 32 pins = 0–127 range). Confirmed against `ark1668.dtsi` (4 `gpioN` nodes, `0xe4600000` + `N*0x20`) and against `doc/USB使用说明.txt` in `linux-arkmicro`, which documents example pin numbers (76, 127) that fit exactly within this range.

### Hardcoded, unconditional GPIO found

Only **9 functions in the entire kernel** call `gpio_request()` (`FUN_801db61c`, identified by signature/error-string usage) with anything other than generic gpiolib-internal wrappers. Of those, only **one** passes a compile-time literal pin number — everything else reads the pin from a `platform_data` struct field populated elsewhere (not yet located — see Open Items).

```c
// customer_gpio_init() @ 0x80018834 — called unconditionally from
// FUN_8059f188 @ 0x8059f188, the same board bring-up routine that
// ioremaps VICL/VICH and registers the Goodix touchscreen. Runs every boot.
gpio_request(0x5f /* = 95 */, "apple_encpy_ic_rst");
gpio_direction_output(95, 1);   // driven high = held out of reset
```

**GPIO 95 = `PBANK_2`, pin 31.** Label `"apple_encpy_ic_rst"` = reset line for an Apple MFi/CarPlay authentication IC — consistent with this being a Toyota OEM head unit (`ProductId=Limcet-P306`, `HomeIconLabel=TOYOTA`, per `docs/SOURCES.md`) needing wired CarPlay support.

**Cross-check against `linux-arkmicro/linux/arch/arm/boot/dts/ark1668-pinctrl.dtsi`:** `ARK_PBANK_2` pin 31 has **no** alternate-function mux entry anywhere in that file (pins 0–20 and 31 of PBANK_2 are claimed by uart1/2/3, i2c0, LVDS, pwm1-3, i2s1 — pin 31 specifically is absent). On the generic ArkMicro reference dev-board, this pin is left spare; on the Limcet P306 production PCB, the same physical pin is wired to the CarPlay auth-chip reset. This is the expected kind of difference between a reference dev-board and a customer production board — not a conflict, just board-specific wiring on top of identical silicon.

### GPIOs referenced but not resolved to literals — and why

These call `gpio_request()` with a pin number read from a `platform_data`/state-struct field rather than an inline constant:

| Label | Function | Context |
|---|---|---|
| `"backcar"` | `FUN_802efe04` @ `0x802efe04` | Reverse-camera detect GPIO; sets up `carback_waiq`/`app_enter_waiq`/`app_exit_waiq` work-queues and a `carback_queue` kthread |
| `"rn6752_reset"` | `FUN_802ee860` @ `0x802ee860` | RN6752 AHD camera decoder reset line |
| `"rn6752_irq"` | `FUN_802ee8f0` @ `0x802ee8f0` | RN6752 AHD camera decoder IRQ line (probe function, also registers `ark_itu656` device + `rn6752_eq_queue` workqueue). Initializes its GPIO field to a sentinel `-1` and only calls `gpio_request` `if (-1 < piVar1[1])` — i.e. the pin is conditionally present, not fixed |
| `"spi_cs_gpio"` | `FUN_80416dd4` @ `0x80416dd4` | Generic `spi-cs-gpio` framework helper (not board-specific by itself) |
| `"sda"` / `"scl"` | `FUN_80415f64` @ `0x80415f64` | Generic bit-banged `i2c-gpio` driver probe (matches `i2c-gpio-0`/`i2c-gpio-1` DT nodes seen in `linux-arkmicro`'s `ark1668e_devb.dts` for the ARK1668E variant — same driver, this board just supplies its own pin numbers via platform data) |

**Traced to ground:** `FUN_8059f188` (the board bring-up routine, see §5.1 below) registers **27 platform devices** via `FUN_8020de0c` (= `platform_device_register`), including `carback` (struct at `0x805c7dc8`). Dumping that struct's raw bytes (offsets `0x00`–`0x5c`, covering `name`/`id`/the front of embedded `struct device`) shows **every field is zero** except a `0x5` at `+0x50` (plausibly `num_resources` or similar) — there is no static `platform_data` pointer baked into the kernel image for this device at all.

This, combined with the `rn6752` probe's `-1` sentinel pattern, means these GPIO numbers are **not compiled into the kernel** — they're populated at runtime, after the static device registration, by something else. Two candidate mechanisms, checked and one ruled out:

- ❌ **`arkdata` NAND partition (MTD4)** — checked `docs/DISPLAY_SUBSYSTEM.md`; this partition is confirmed to carry only LCD panel timing (resolution, `CLKDIV1`, `VBP`/`HBP`/`VSW`/`HSW`, touch-key config), nothing GPIO-related. Ruled out.
- ✅ **Likely candidate: userspace, via `/proc/ark_gpio`** — `docs/KERNEL_REFERENCE.md` already documents this debug/control interface exposed by the `ark_gpio` driver. A userspace daemon (`MsnCoreApp` or similar, reading `MsnProductInfo.ini`'s `ResourceName`/`McuType`/`ProductId` fields — see `docs/SOURCES.md`) most plausibly writes the camera/backup-camera GPIO numbers into the kernel at boot through this interface, based on which product variant is running. This is consistent with the whole project's established pattern of board variants being selected by userspace config rather than separate kernel builds.

This means further progress on these specific pins requires **rootfs/userspace binary analysis** (disassembling `MsnCoreApp` or whichever binary opens `/proc/ark_gpio` at boot), not more kernel-binary work — a different, follow-on task from what Ghidra-on-`vmlinux.bin` can resolve.

### Full platform-device registration roster (board bring-up order)

Recovered by resolving the `.name` field (offset 0 of `struct platform_device`) for each of the 27 `platform_device_register()` calls in `FUN_8059f188`:

| # | Name | id | Struct addr |
|---|---|---|---|
| 1 | `gpio-ark` | -1 | `0x805c6a40` |
| 2 | `ark1680-uart` | -1 | `0x805c6658` |
| 3 | `ark1680-hsuart` | -1 | `0x805c6720` |
| 4 | `ark_dw_dmac` | 0 | `0x805c6590` |
| 5 | `dw_mmc` | 0 | `0x805c67e8` |
| 6 | `dw_mmc` | 1 | `0x805c68b0` |
| 7 | `ark1680-nand` | -1 | `0x805c6978` |
| 8 | `ark_i2s_dev` | 0 | `0x805c6ef0` |
| 9 | `ark_i2s_dev` | 1 | `0x805c6fb8` |
| 10 | `ark-audio` | -1 | `0x805c7080` |
| 11 | `ark_sddac_dev` | -1 | `0x805c7148` |
| 12 | `ark_sddac_dai` | -1 | `0x805c7210` |
| 13 | `ark_cs4334_dev` | -1 | `0x805c72d8` |
| 14 | `ark-display` | -1 | `0x805c73a0` |
| 15 | `ark-prescaler` | -1 | `0x805c7468` |
| 16 | `ark-jpeg` | -1 | `0x805c7530` |
| 17 | `ark-deinterlace` | -1 | `0x805c75f8` |
| 18 | `ark-itu656` | -1 | `0x805c76c0` |
| 19 | `ark-pwm` | -1 | `0x805c7788` |
| 20 | `ark-wdt` | -1 | `0x805c7850` |
| 21 | `ark1680-ts` | -1 | `0x805c7918` |
| 22 | `musb-ark1680` | 0 | `0x805c79e0` |
| 23 | `musb-ark1680` | 1 | `0x805c7aa8` |
| 24 | `pwm-backlight` | -1 | `0x805c7c38` |
| 25 | `ark1680-spi` | 0 | `0x805c7d00` |
| 26 | `ark1680-rtc` | -1 | `0x805c6b08` |
| 27 | `carback` | -1 | `0x805c7dc8` |
| 28 | `ark_nec_sw_remote` | -1 | `0x805c7e90` |

Note `musb-ark1680` and `dw_mmc` are each registered twice (`id=0`/`id=1`) — confirms **2 USB OTG controller instances** and **2 MMC/SD controller instances** are present on this SoC/board, both consistent with `ark1668.dtsi`'s `mmc0`/`mmc1` nodes in the public tree. `ark_nec_sw_remote` (NEC IR protocol steering-wheel remote receiver) is a device not previously called out in `docs/KERNEL_REFERENCE.md` and worth folding into that inventory.

### Open items / next steps

1. Disassemble the rootfs userspace binary responsible for writing to `/proc/ark_gpio` at boot (most likely `MsnCoreApp`) to recover the actual `backcar`/`rn6752_reset`/`rn6752_irq` pin numbers — this is rootfs/userspace RE, not kernel RE, and is a natural follow-on task.
2. ~~Re-run this same pass against the `reconstructed`-tree zImage (build #383)~~ — **done, see §6.**
3. `fixup` field decoding of the single `machine_desc` struct beyond `nr`/`name` was a rough field-order guess based on mainline layout and should not be trusted without further verification — this vendor's exact 3.4 struct layout may differ.
4. The remaining unnamed `FUN_80009xxx`/`FUN_8020dxxx` helper functions (`ioremap`, `platform_device_register`, `gpio_request`, etc., all identified so far purely by call signature/error-string context, not real symbols) could be formally labeled in the saved Ghidra project for anyone continuing this work interactively.

---

## 6. Cross-check against the `KERNEL.md` build (#383, Dec 2023)

Ran the identical extraction + Ghidra pipeline against `Prado firmware reconstructed/mtd5_kernel/zImage` (MD5 `78782daea...`, the build `docs/KERNEL_REFERENCE.md` documents) to check whether everything above still holds 22 months and 30 build numbers later.

**Extraction:** Same LZO magic-byte offsets (`0x1914` decoy, `0x1A68` real payload). Decompresses cleanly to 6,384,420 bytes — exactly matching the uncompressed size `KERNEL.md` already recorded. Version banner confirms `#383 Tue Dec 5 10:50:38 CST 2023`, build host `root@build-server` (vs `flyound@build-server` for #353 — the OEM integrator's build-server login changed between these builds, `root` instead of a named user, otherwise unremarkable).

**Base address:** Same `0x80008000` — confirmed via the same literal-pointer method (1 hit for the version-string pointer, 0 for `0xC0xxxxxx` alternatives).

**Identity strings:** `ARK1680`, `ark1680_machine_init`, `ark1680-nand`, `apple_encpy_ic_rst`, `customer_gpio_init`, `backcar`, `rn6752_reset`, `rn6752_irq`, `Goodix-TS`, `TOYOTA388`, `TOYOTA794`, and the `/workspace/ark0618system/kernels/linux-3.4/arch/arm/mach-ark1680/clock.c` build path are **all present and unchanged**.

**Peripheral memory map:** Ran the same `struct resource{start,end,name,flags=IORESOURCE_MEM}` scan — **byte-for-byte identical 16-entry table**, same addresses and sizes, just shifted to a later file offset (`+0x2e000`, consistent with the larger binary having more code ahead of this data). One low-confidence discrepancy: the bare literal `0xe4a00000` (timer) showed 1 raw-pointer hit in build #353 and 0 in build #383 — most likely a compiler codegen/register-allocation artifact (e.g. a literal-pool merge shifting how the constant is loaded) rather than a real hardware/driver change, since the timer isn't part of the `struct resource` table in either build anyway (it's set up some other way in both). Not treated as a real finding.

**`setup_machine_tags()` / machine_desc:** `FUN_805ca13c` in build #383 is structurally identical to `FUN_8059c13c` in build #353 — same 18-word-stride single-entry array walk, same `"Machine: %s"` print, same panic path. Function shifted address (`0x8059c13c` → `0x805ca13c`) but logic is unchanged.

**`customer_gpio_init()` — the key finding holds exactly:**
```c
// @ 0x80018834 — IDENTICAL ADDRESS in both builds (this function didn't move
// at all despite ~89KB of other code changes elsewhere in the kernel)
int FUN_80018834(void)
{
  iVar1 = FUN_801db89c(0x5f, s_apple_encpy_ic_rst_805096f0);   // gpio_request(95, "apple_encpy_ic_rst")
  ...
  iVar1 = FUN_801dbbbc(0x5f, 1);                                 // gpio_direction_output(95, 1)
}
```
**GPIO 95 (`PBANK_2`, pin 31) for the Apple/CarPlay auth-chip reset is unchanged between Feb 2022 and Dec 2023 builds.**

**Other GPIO call sites:** all 9 `gpio_request()` callers found in build #353 (`backcar`, `rn6752_reset`, `rn6752_irq`, `spi_cs_gpio`, `sda`/`scl`, plus 4 generic gpiolib-wrapper callers) are present in build #383 with identical call patterns, just at shifted `gpio_request` address (`0x801db61c` → `0x801db89c`). Same `platform_data`-driven-not-literal situation for all of them — the conclusion in §5's "Open items" about needing rootfs/userspace RE (`/proc/ark_gpio`, `MsnCoreApp`) to resolve those pins applies equally to both builds.

**Function count:** 20,517 (build #383) vs 19,976 (build #353) — +541 functions, consistent with ~89KB of additional code from 22 months of vendor patches. None of the growth touched the board-init/GPIO/machine-descriptor code paths examined here.

**Conclusion:** Everything in §§1–5 of this document — SoC identity, peripheral memory map, the CarPlay-chip GPIO finding, and the unresolved-platform_data situation for the camera GPIOs — holds identically across both kernel builds in this repo. The `dump` vs `reconstructed` build discrepancy noted at the top of this doc is real (different build numbers/dates/hosts) but has **no bearing on hardware wiring or SoC identity** — it looks like ordinary incremental kernel maintenance, not a board revision or hardware change.

---

## 7. Userspace trace: where the `backcar`/camera GPIO pin actually comes from

§5's open item was: the kernel's `carback`/`rn6752_reset`/`rn6752_irq` GPIO pins are read from `platform_data`, not compiled in, and `/proc/ark_gpio` (documented in `KERNEL.md`) was the leading hypothesis for how userspace supplies them at boot. Traced this properly instead of leaving it as a guess.

**`MsnCoreApp` itself is a dead end for this.** It's a non-stripped ELF with full DWARF debug info (`Prado firmware dump/mtd6_rootfs/usr/bin/MsnCoreApp`, imported into Ghidra directly — no base-address guessing needed, unlike the kernel binary). It has a `CarSignalsWatch` class (`onWatchGPIOThreadProc`, `gpioValueChange(GPIOOperater*)`) but **zero string references to `carback`, `backcar`, or `rn6752` anywhere in the binary** — this class watches other vehicle signals (illumination, handbrake, etc.), not the reverse camera.

**Traced `GPIOOperater` (the C++ RAII GPIO wrapper class used throughout this codebase) to its real implementation:** `nm -D` across every library `MsnCoreApp` links (`libCanBus.so`, `libFMRadio.so`, `libMcuCenter.so`, `libSetting.so`, `libMsnCommons.so`, `libMsnSound.so`) shows all of them *import* `GPIOOperater`'s constructor/methods as undefined (`U`) except **`libMsnCommons.so`**, which defines them (`T`) — that's the actual GPIO-sysfs wrapper (`gpio_fd_open(uint,uint)` + `GPIOOperater(unsigned short)`). But the literal pin numbers live in the *callers*, not here.

**`libMcuCenter.so`** (the MCU-serial-comms library — matches `docs/KERNEL_REFERENCE.md`'s documented `arktool`/UART protocol) is where it gets interesting. It contains "Reverse camera", "Reverse condition", "Reverse Radar", "Reverse Track" strings, and a whole family of `MCUAdapter_*` C++ classes — one per product/box model (`MCUAdapter_Bagoo`, `_ZhongHang`, `_BoxC230` through `_BoxC290`, `_BoxP100` through `_BoxP900`, `_CarA200/300/301`, `_HUD`, `_NV17`, `_D107`, `_RuiYuanSWC`, `_IM60BC`, `_MsnDecoder`, `_Box_Encryption` — 27 variants total). Several directly construct a `GPIOOperater` with a hardcoded pin number in their constructor:

| Adapter class | GPIO (hex → dec) | `PBANK`/pin |
|---|---|---|
| `MCUAdapter_BoxP400` | `9` | PBANK_0 pin 9 |
| `MCUAdapter_CarA300` (conditional, app-state == 4) | `0x23` = 35 | PBANK_1 pin 3 |
| `MCUAdapter_ZhongHang` | `0x25` = 37 | PBANK_1 pin 5 |
| `MCUAdapter_BoxP700` | `0x25`=37 **and** `0x60`=96 (two GPIOs) | PBANK_1 pin 5; PBANK_3 pin 0 |
| `MCUAdapter_Bagoo` | `0x66` = 102 | PBANK_3 pin 6 |
| `MCUAdapter_BoxC270` | `0x7d` = 125 | PBANK_3 pin 29 |

**Found the factory/dispatcher:** `MCUAdapter::getAdapterInstance(MCUAdapter::McuType)` (exact demangled symbol name — no guessing needed) is a 30-case `switch` on the numeric `McuType` value, `case 1` through `case 0x1e`, each `operator_new`-ing and constructing the matching adapter class. This is the mechanism `docs/SOURCES.md`'s `McuType` field (16 for Holden, **6 for Limcet P306**) actually drives.

**`case 6` → `MCUAdapter_BoxP300`.** This is the Limcet P306's real, active MCU adapter class. Decompiled its full constructor (and confirmed — by scanning literally every method with `BoxP300` in the name, only 4 exist total: ctor/dtor pairs — **`GPIOOperater` does not appear anywhere in this class**. Instead, its constructor calls `MsnApplication::getFactorySetting(...)` twice — once to read a list of radar-sensor IDs (parsed via `QString::split` into a `uint` list — matches `KERNEL.md`'s documented multi-radar/PDC support) and once for another numeric setting. Both are **software config reads**, not GPIO hardware access.

### Conclusion

**The Limcet P306 unit does not read the reverse-camera/backup signal via a raw SoC GPIO pin in userspace at all.** The `MCUAdapter_BoxP300` class handling this product's MCU communication never touches `GPIOOperater`. This closes the loop with two things already documented independently:

1. `docs/KERNEL_REFERENCE.md` already recorded that the `arktool` MCU-UART binary protocol carries a **`backcar enable/disable`** command.
2. §5 of this document found the kernel's `carback` platform_device has an **entirely zero `platform_data` struct** — no GPIO configured for it at the kernel level either.

Both facts point to the same conclusion: on this product, reverse-gear/backup-camera detection happens on the **companion MCU** (which has its own firmware, wired directly to the vehicle's reverse-light circuit or a physical switch — not analyzed here, would require dumping/RE'ing the MCU's own firmware image separately), and the MCU simply *tells* the ARK1680 SoC "enter backcar mode" over the HS-UART `arktool` link. There's no GPIO pin on the SoC side to find for this signal on the Limcet P306 — it was never a compiled-kernel-literal, a userspace `/proc/ark_gpio` write, *or* an `McuType`-specific board GPIO, because the whole detection path lives outside the SoC entirely. The GPIO literals found in `MCUAdapter_Bagoo`/`ZhongHang`/`BoxP400`/`BoxP700`/`BoxC270`/`CarA300` above are real and board-specific, just **for different, non-Limcet-P306 products** built from the same shared `libMcuCenter.so` — a good illustration of how much shared-codebase archaeology this firmware requires: the code path that's actually load-bearing for one product is dead weight (or vice versa) for another, and only the `McuType` factory switch tells you which is which.

**Independently confirmed after the fact:** `hardware/BOARD_ANALYSIS.md` (physical board inspection, done separately from this software RE) identifies the companion MCU as an **STM32F105RBT6** running **Limcet-V1.0-1302** firmware, and explicitly lists "ACC/IGN detection, reverse trigger input, panel button inputs" among its GPIO responsibilities, plus an onboard NXP TJA1042 CAN transceiver wired to the STM32's own bxCAN peripheral for reading Toyota-specific steering-wheel CAN messages. Two completely independent methods (kernel/userspace binary RE vs. physically inspecting the board) converged on the same architecture: MCU owns the vehicle-signal GPIOs and CAN bus, SoC just listens over UART.

This also means the earlier open items about `rn6752_reset`/`rn6752_irq`/`spi_cs_gpio` (the actual camera decoder chip's own reset/IRQ lines, as opposed to the reverse-signal trigger) remain genuinely open — those are a separate, still-unresolved question from the "how does the system know to enter backcar mode" question this section answers. They'd need the same `platform_data` static-struct tracing approach in the kernel binary that was inconclusive in §5 (all-zero struct), so likely also resolve to an MCU-reported or otherwise non-kernel-literal source, but that hasn't been directly confirmed.

---

## 8. Synthetic device tree

Wrote [`hardware/ark1668-limcet-prado.dts`](../hardware/ark1668-limcet-prado.dts) — a structured, `.dts`-syntax reconstruction of this board's hardware, combining every finding in this document with the physical inspection in `BOARD_ANALYSIS.md` (chip markings, the STM32 MCU, RN6752 camera decoder, Bluetooth module, confirmed LCD timings from `docs/DISPLAY_SUBSYSTEM.md`).

**This is explicitly not a real boot artifact** — §2 already established this board's actual firmware has no device tree at all (legacy ATAG boot, confirmed via absent FDT magic in both kernel builds and U-Boot's `bootz`/`bootnand` calls never passing an fdt address). It exists purely as documentation, with every node/property tagged by evidence source:

- **`[SW-RE]`** — confirmed via Ghidra RE of the kernel or userspace binaries (this document, §§2–7)
- **`[HW]`** — confirmed via physical board inspection (`BOARD_ANALYSIS.md`)
- **`[REF]`** — inherited from the public `linux-arkmicro` tree's `ark1668.dtsi`/`ark1668-pinctrl.dtsi` because the silicon is confirmed identical (§3), but *not* independently verified for this board's actual PCB wiring — treated as a plausible default, not a fact, unless also tagged `[SW-RE]` or `[HW]`

Notably it now carries **three confirmed GPIO numbers** (up from one): GPIO 95 (`apple_encpy_ic_rst`, from kernel RE), GPIO 91 (Bluetooth module enable, from `blueware.properties` — found only via the physical-inspection side, not the kernel RE side), and GPIO 34 (ambiguous, tentatively tied to an unrelated external-CAN-adapter SDK, flagged low-confidence in both source documents). It also documents, with an explicit dedicated comment block, why this SoC's CAN pin-mux capability (`can0`/`can1` in `pinctrl0`, inherited from `[REF]`) is present in silicon but genuinely unused on this board — the real vehicle CAN bus runs entirely through the companion STM32 MCU instead.

---

## 9. Cross-check against real ASTRI vendor source (`ArkPro Reference/`)

Everything above §§1–8 was derived purely from Ghidra decompilation — no vendor source was available.
Found and cloned a public leak of ASTRI's (Hong Kong Applied Science and Technology Research Institute)
reference BSP for this exact SoC (`cphatt/ArkPro`, commit `e743744`, a Qt AVService SDK that bundles a
`mach-ark1680` kernel slice for its backlight/display kernel modules). Copied the relevant files into
[`ArkPro Reference/`](../ArkPro%20Reference/README.md) — see that folder's README for full provenance
and scope notes. This is a **generic reference BSP, not the Limcet P306's actual OEM board file** — it has
none of this OEM's customer-specific additions (`apple_encpy_ic_rst`, `carback`, `rn6752_*`, etc.) —
but it confirms several things §§2–5 could only infer from disassembly:

- **`ark1680_add_device_*()` roster in [`config.c`](../ArkPro%20Reference/kernel/arch/arm/mach-ark1680/config.c)** registers platform devices under the same names found in the Limcet P306 binary's `FUN_8059f188` (§5's table): `gpio-ark`, `ark1680-uart`, `ark1680-nand`, `ark_i2s_dev`, `ark-display`, `ark-prescaler`, `ark-jpeg`, `ark-deinterlace`, `ark-itu656`, `ark-pwm`, `ark-wdt`, `pwm-backlight`. Real source confirming what was previously just a decompiled device-name string list.
- Same file's USB bring-up does `ioremap(VICH_BASE, ...)` + sets `BIT(7)|BIT(8)` to gate MUSB IRQs into the VIC — confirms the VICL/VICH ioremap pattern §4 found in the Limcet P306 binary's board-init routine is a real, intentional step (not a decompiler artifact), just for USB IRQ routing specifically.
- **LCD timing register fields** — see the new section added to `docs/DISPLAY_SUBSYSTEM.md`: `ark_display_lcd.c` and `uboot/ark_lcd.c` confirm `CLKDIV1`/`VBP`/`HBP`/`VSW`/`HSW`/`IVS` are ArkMicro's real register field names (`CLCD_TIMING0/1/2`), not RE-guessed labels, and explain `CLKDIV1` as a system-PLL clock divider.
- **A third candidate mechanism for backcar detection**, not previously considered: [`userspace/display.h`](../ArkPro%20Reference/userspace/display.h) defines `ARKDISP_GET_BACKCAR_STATUS` (ioctl `0xa0`/`25`) on the `ark-display`/framebuffer device itself. This doesn't change §7's conclusion — the Limcet-P306-specific trace of `MCUAdapter_BoxP300` independently and directly confirmed the MCU-UART `arktool` path is what's actually used on this product — but it's worth recording that ArkMicro's own reference stack has a kernel-ioctl-based backcar path as a generic option, in case a future variant or product on this platform turns out to use it instead.

No `clock.c`, GPIO driver, NAND/touchscreen driver, or `MsnCoreApp`/`libMcuCenter`-equivalent userspace source was found anywhere in the upstream `ArkPro` repo (checked `Launcher/`, `MultimediaService/`, `AutoConnect/`, and the other Qt-service directories) — so the open items in §5 (platform_data-sourced GPIO pins for `backcar`/`rn6752_reset`/`rn6752_irq`) remain open; this vendor source doesn't resolve them.

---

## 10. Pinout — full pin-mux table from real vendor source, and a CAN correction

Located and vendored the actual `linux-arkmicro` kernel devicetree source (`RD_Software/linux-arkmicro`,
see [`linux-arkmicro Reference/README.md`](../linux-arkmicro%20Reference/README.md) for provenance —
this is the same public BSP `docs/UBOOT_BUILD_GUIDE.md` uses for the U-Boot side). Its
`arch/arm/boot/dts/ark1668-pinctrl.dtsi` is ArkMicro's own pin-mux table for this exact SoC — every
GPIO pad, which peripheral function(s) it can be muxed to, and the alt-function value to select each.
Used it to fill in and correct the `pinctrl0` node in
[`hardware/ark1668-limcet-prado.dts`](../hardware/ark1668-limcet-prado.dts).

**What this resolves:** a full silicon-level pin-mux table (LCD RGB888/hi-Z/LVDS on PBANK_0 pins
2–29, NAND on PBANK_1 pins 7–20, UART0–5 spread across PBANK_1/2/3, PWM1–3 and I2C0 and part of the
LVDS lanes on PBANK_2, SPI and I2S1-MCLK on PBANK_3, the remaining LVDS lanes and I2S1-SADATA_IN on
PBANK_4) — all now [REF]-tagged against real source instead of being reconstructed from the
resource-struct/string scan alone. GPIO bank layout also confirmed directly from `ark1668.dtsi`'s
`gpio@e460NNNN` nodes: `gpio-ranges` show PBANK_0 only exposes pins 6–7 and 10–31 as general GPIO
(0–5, 8–9 are LCD-dedicated), while PBANK_1–3 each expose the full 32 pins — matching the
`bank*32+pin` global numbering scheme §5 already established.

**A real correction, not just an addition:** the synthetic dts previously included `can0`/`can1`
pin-mux nodes tagged `[REF]`, on the assumption they lived in `ark1668-pinctrl.dtsi` like everything
else in that file. They don't. Confirmed by direct inspection: `can0`/`can1` (`ARK_PBANK_4` pins 26–29)
exist **only** in `ark1668e-pinctrl.dtsi`, the newer/GIC generation §2 already ruled out as not
matching this SoC. The base ARK1668/ARK1680 silicon this board actually uses has **no CAN pin-mux
capability in ArkMicro's own reference source at all** — not "present but unused," genuinely absent.
This makes the existing §7/BOARD_ANALYSIS.md conclusion (CAN runs entirely through the companion
STM32F105RBT6 MCU) stronger, not weaker. Fixed in the synthetic dts; see its corrected CAN comment
block for the full explanation.

**What's still a real gap:** MMC0/MMC1 (SD/eMMC) pins aren't in the named pinctrl table in *either*
ark1668 or ark1668e — neither `mmc@` node in `ark1668.dtsi` carries a `pinctrl-0` phandle. Cross-checked
against real source on both sides (`linux-arkmicro Reference/u-boot/board/arkmicro/ark1668/ark1668.c`'s
`dwmci_select_pad()`, and independently, the ArkPro kernel's `config.c` `ark_sys_pad_config(SYS_PAD_CTRL06,
0xFFFFFFF, 0, 0x2222222)` call, §9) — both poke the same raw `SYS_PAD_CTRL05`/`06`/`0B` registers
(`0xe49001d4`/`0x1d8`/`0x1ec`) directly rather than going through the pinctrl framework, confirming this
is a deliberate hardware design choice (MMC pins are on a separate, non-GPIO-bank pad group), not a
documentation gap. Which register bit corresponds to which individual signal (CLK/CMD/D0-D3) isn't
named in either source — resolving that further would need an actual datasheet, not available here.

---

## Tooling Note

This analysis was performed in an ephemeral scratch environment (portable JDK/Ghidra downloaded to a session-local temp directory, not committed anywhere) — the Ghidra project database and `vmlinux.bin` are not preserved as repo artifacts. Anyone reproducing this should re-run §4's extraction + import steps against the zImage of interest.


## PIN_MASTER_LIST.md

# Master pin list — everything known, 2026-07-13

Consolidates every pin assignment confirmed this session (live debugfs +
DTS cross-check + stock-kernel disassembly), plus the hard lesson from
the GPIO brute-force incident: **the pinctrl DTS and live debugfs
pinmux-pins view are not a complete picture.** Several real, actively-used
peripherals never appear there at all because they're configured via
fixed/early pad routing outside the standard Linux pinctrl framework.
Treat any pin not explicitly listed here as **unverified, not confirmed
free** — "no claim found" is not the same as "safe."

Global pin numbering = `bank * 32 + offset` (confirmed via
`pinctrl-ark.c:25,773`, `MAX_PIN_PER_BANK=32`, `ARK_PBANK_0=0` etc.).
This matches `/sys/class/gpio/gpioN` 1:1 on this board (confirmed via
live `gpio-ranges` debugfs — direct mapping, no offset).

## Driver source reference (2026-07-14)

For every peripheral above/below: which driver claims it, where that driver's
source actually lives, and how solid that is. `linux-arkmicro` paths are
relative to `/home/osboxes/Downloads/linux-arkmicro/linux/`. **Checked
against the tree directly this session** (grepped for each DTS `compatible`
string against the actual driver's `of_match_table`, not assumed from
filename) — where two similarly-named files existed for one peripheral
(e.g. two carback drivers), the table lists the one whose compatible string
actually matches this project's DTS, not the other SoC variant.

**"Confirmed by operation" column — status key** (added 2026-07-14, cross-checked
against `docs/new kernel bootlog new uboot v11.txt`, the most recent full boot
log, plus dedicated docs where the boot log alone wasn't enough).

**Important caveat on the boot log itself, added after user pushback
2026-07-14: a clean dmesg probe line is weak evidence, not proof of correct
operation.** A driver can print a success message and register a device node
while the actual hardware behind it is silently misbehaving — wrong data,
no physical response, a downstream failure that never reaches the kernel
log at all. "No error in dmesg" only means the *driver's own init path*
didn't hit a failure condition it knows to check for; it says nothing about
whether the peripheral is functionally correct. Treat **CONFIRMED** as the
only status here that means "actually verified," and read **PROBES OK**
as "unknown, leaning slightly more hopeful than NOT CONFIRMED" — not as
"basically working."
- **CONFIRMED** — physically observed actually working (video visibly rendering,
  audio audibly playing, a real SD card/WiFi AP/USB device enumerating), not
  just "driver loaded without an error."
- **PROBES OK** — the driver initializes without a logged error, but this is
  **not evidence of correct operation** — no functional/physical test has
  confirmed the feature actually works, and a real problem could easily be
  invisible to dmesg. Treat as unverified, closer to NOT CONFIRMED than to
  CONFIRMED.
- **NOT CONFIRMED** — no success evidence found either way; don't assume it works.
- **DORMANT** — intentionally inactive on this hardware (nothing loads it, or
  the hardware isn't populated) — not a bug, just not in use.
- **N/A** — no kernel driver/operation concept applies.

| Peripheral | DTS `compatible` | Driver source | Confirmed by operation | Notes |
|---|---|---|---|---|
| LCD framebuffer | `arkmicro,ark1668-lcdc` | `drivers/video/fbdev/arkmicro/ark1668_lcdfb.c` | **CONFIRMED** | video physically observed on screen across many sessions; boot log: `ark1668_lcdfb e0500000.lcd: fb0: Atmel LCDC at 0xe0500000... irq 25` |
| `/dev/ark_display` misc ioctl shim | `ark_display` (misc device, not a DT node) | `hardware/ark_display.c` | PROBES OK | boot log: `ark_display: registered /dev/ark_display` — confirms the shim loads and fixes the specific ioctl it targets; whether it also fully unblocks `MsnCoreApp` end-to-end wasn't separately re-verified this session |
| NAND | (`ark1668,nand`-style, see `ark1668.dtsi`) | `drivers/mtd/nand/raw/ark_nand.c` | **CONFIRMED** (boot chain) | full stock UI boots from NAND via the `bootstock` U-Boot path (project memory, 2026-07-13); this specific boot log predates/differs from that ECC fix and still shows `ark_nand_correct_data: uncorrectable ECC error` spam — don't take *this log* as proof the ECC fix is deployed everywhere, only that NAND boot itself is a proven-working path |
| UART0-3 | `arkmicro,ark-uart` | `drivers/tty/serial/ark_uart.c` | uart0: **CONFIRMED** (console); uart1-3: NOT CONFIRMED | uart0 is the serial console this entire investigation has been conducted over; uart1-3 have no evidence of any attached function being tested |
| hsuart uart4 (MCU link, `/dev/ttyHS0`) | `arkmicro,ark-hsuart` | `drivers/tty/serial/ark_hsuart.c` | PROBES OK | boot log: `e4f00000.serial: ttyHS0 at MMIO 0xe4f00000 (irq = 38...) is a ARK HS UART` — tty node exists and driver loads; no evidence in this session of actual MCU protocol traffic (key events, `recv track:`, etc.) confirmed flowing over it on this kernel build |
| hsuart uart5 (Bluetooth, `/dev/ttyHS1`) | `arkmicro,ark-hsuart` | `drivers/tty/serial/ark_hsuart.c` | **CONFIRMED** | boot log: `e4800000.serial: ttyHS1 at MMIO 0xe4800000 (irq = 44...) is a ARK HS UART`, plus `Bluetooth: HCI UART driver ver 2.3` loading; `wireless_and_init_documentation.md` documents this as an already-working, tested setup (RTL8762BTV/BT825) |
| HW I2C0 | `snps,designware-i2c` | `drivers/i2c/busses/i2c-designware-{master,platdrv}.c` | PROBES OK | bus itself comes up; the only device on it in the DTS (Goodix-TS) fails to ACK (`Goodix-TS 1-005d: I2C communication failure: -6` — expected/consistent with GT911 being dormant, not a bus fault) |
| i2c-gpio1/2/3 (bit-banged) | `i2c-gpio` | `drivers/i2c/busses/i2c-gpio.c` | PROBES OK | boot log confirms both buses register (`i2c-gpio i2c-gpio-0: using lines 3 (SDA) and 2 (SCL)...`, `i2c-gpio-1: using lines 9 (SDA) and 121 (SCL)...`) |
| rn6752 camera decoder | `arkmicro,ark1668_rn6752` | `drivers/soc/arkmicro/itu656/rn6752.c` | NOT CONFIRMED | only evidence in the latest boot log is the recurring `### rn6752_eq_work reset` line — no clear probe-success message found; actual camera video not confirmed working on this kernel build this session |
| BD37033 audio codec | `arkmicro,drv_bd37033` | `sound/soc/arkmicro/BD37033.c` | NOT CONFIRMED (codec control); I2S path itself CONFIRMED | no `bd37033` probe line found at all in the latest boot log (worth checking whether the module actually loaded on that boot); however basic I2S audio output has its own dedicated confirmed-working session (`new kernel audio working log v1.log`, `AUDIO_SUBSYSTEM_INVESTIGATION.md`'s dai-link fix) — so audio *plays*, but BD37033's own I2C-controlled volume/effects path status is unclear from this session's evidence |
| GT911 touch (dormant on this unit) | `goodix,gt911` | `drivers/input/touchscreen/goodix.c` | **DORMANT** (confirmed inactive, not a failure) | boot log's `I2C communication failure: -6` is the *expected* signature of this being unpopulated hardware, consistent with prior conclusions — not evidence of a bug |
| Resistive touch (actually used) | `arkmicro,ark1680-ts` | `drivers/input/touchscreen/ark1680_ts.c` | PROBES OK; physical touch input inconclusive | boot log: `input: ark1680-ts as .../input0`, `ARK1680 resistive touchscreen registered, irq=20` — driver probes cleanly; whether physical touches actually register as input events on *this* kernel build is not confirmed by this evidence alone (see `ARK1680_TS_REVERSE_ENGINEERING.md`'s mixed findings, largely gathered on stock firmware, not this kernel) |
| PWM1-3 | `arkmicro,ark-pwm` | `drivers/pwm/pwm-ark.c` | **CONFIRMED** (backlight) | screen visibly lit/backlight functional across every working-video session |
| I2S1 + DAC/ADC codecs | (ark1668 sound bindings) | `sound/soc/arkmicro/ark_i2s.c`, `ark1668-sddac-codec.c`, `ark1668-sdadc-codec.c`, `ark_dac_codec.c`, `ark_adc_codec.c` | **CONFIRMED** | dedicated working session/log (`new kernel audio working log v1.log`) plus the dai-link-order fix documented in `AUDIO_SUBSYSTEM_INVESTIGATION.md` |
| SPI | `arkmicro,ark-ecspi` (ARK1668, non-`e` variant) | `drivers/spi/spi-ark.c` | NOT CONFIRMED | no evidence any SPI-attached device has been tested |
| `ark_carback` (reverse-gear trigger, GPIO 5) | `arkmicro,ark-carback` | `drivers/soc/arkmicro/ark-carback.c` | NOT CONFIRMED | no `carback` probe line found in the latest boot log; GPIO 5 wiring itself is triple-confirmed (stock disasm, Ghidra, and this driver's own `devm_gpiod_get(pdev, "detect")` source), but an actual reverse-gear trigger test hasn't been evidenced this session |
| `ark_nec_sw_remote` (IR) | n/a | n/a | **N/A** | no IR sensor populated on this hardware revision — confirmed by the user, 2026-07-13 |
| GPIO 95 `apple_encpy_ic_rst` | none (no DT node) | none | **N/A** | stock-3.4-only board-init GPIO poke; not modeled in the 4.9 DTS at all, chip presence on this unit unconfirmed |
| GPIO 91 `BTEN` (Bluetooth enable) | none (plain sysfs) | none | **CONFIRMED** | `wireless_and_init_documentation.md` documents this as part of an already-working, tested Bluetooth setup |
| Bluetooth stack (RTL8762BTV/BT825 over `/dev/ttyHS1`) | n/a (userspace) | Feasycom/`rtkbt` userspace BT stack (rootfs, not kernel) | **CONFIRMED** | see uart5 row above |
| WiFi (RTL8811CU, USB) | n/a (USB autoload) | `drivers/net/wireless/realtek/rtl8811cu/` | **CONFIRMED** | user-confirmed working; boot log: `wlan0: interface state UNINITIALIZED->ENABLED`, `wlan0: AP-ENABLED`, SSID `carplay_wifi`. **Default boot behavior changed 2026-07-14**: `rc.d/rcS` now calls `etc/wifi_client.sh` (join a local network, for easier SSH/testing access) instead of `etc/wifi_ap.sh` (the `carplay_wifi` AP) — a deliberate choice, since one radio can't do both at once. The AP script is untouched and still works if run manually; see its own header comment and `docs/DRIVER_TEST_PLAN.md`. |
| MMC0 (SD card slot) | `snps,dw-mshc` | `drivers/mmc/host/dw_mmc.c` | **CONFIRMED** | user-confirmed working; boot log: `mmc0: new SD card at address e126`, `mmcblk0: mmc0:e126 SU01G 969 MiB` |
| MMC1 | `snps,dw-mshc` | `drivers/mmc/host/dw_mmc.c` | NOT CONFIRMED / purpose unclear | boot log shows bus-speed negotiation only (`mmc_host mmc1: card is non-removable`), no explicit device bind message captured this session — DTS comments it as "SDIO WiFi Controller," but the WiFi that's actually confirmed working is the USB RTL8811CU, so `mmc1`'s real role on this unit needs re-checking, not assumed from the DTS comment |
| USB (MUSB) | `arkmicro,ark-musb` | `drivers/usb/musb/musb_ark.c` | **CONFIRMED** | user-confirmed working; boot log shows hub enumeration on both controllers and the WiFi dongle attached via USB |

**Net takeaway**: almost every peripheral already has real, in-tree driver
source in the buildable `linux-arkmicro` kernel tree — the stock 3.4
disassembly work in this doc was about recovering **wiring/pin ground
truth** (which the 4.9 *driver* code doesn't tell you), not about missing
driver code. The two genuine gaps are IR (hardware not populated, moot) and
GPIO 95's CarPlay/MFi chip (presence on this unit still unconfirmed).

**Second net takeaway, from the "confirmed by operation" column**: source
existing and a driver probing cleanly is a materially weaker claim than
something being *observed working*. Only WiFi, USB, MMC0, the LCD/backlight,
I2S audio, uart0 console, and Bluetooth are actually confirmed operating
end-to-end. Several other peripherals with clean driver source and a
successful probe in the boot log — the MCU link (`ttyHS0`), carback, rn6752,
BD37033's own I2C control path, and resistive touch's actual finger-touch
behavior — have **not** been confirmed to functionally work on this exact
4.19 kernel build this session, and shouldn't be assumed to just because
their driver loads without an error. Worth prioritizing an actual functional
test for these before treating them as "done."

## Confirmed claimed — visible in pinctrl DTS + live debugfs

| Pins (global) | Bank/offset | Function | Source |
|---|---|---|---|
| 2-25 | PBANK_0 2-25 | LCD RGB888 r0-r7/g0-g7/b0-b7 (**active**) | `ark1668-pinctrl.dtsi:45-70`, live-confirmed muxed to `e0500000.lcd` |
| 26-29 | PBANK_0 26-29 | LCD base: de/clk/vsync/hsync (**active**) | `ark1668-pinctrl.dtsi:37-42` |
| 39-52 | PBANK_1 7-20 | NAND data/cle/ale/ren/wen/rb0/cen0 | `ark1668-pinctrl.dtsi:89-106` — **not shown as claimed in live debugfs** (see caveat below) |
| 62-63 | PBANK_1 30-31 | **uart0 — the console UART this entire investigation used** | `ark1668-pinctrl.dtsi:109-114` — **also invisible in live debugfs** |
| 64-65 | PBANK_2 0-1 | uart1 rx/tx | `ark1668-pinctrl.dtsi:117-123` |
| 66-67 | PBANK_2 2-3 | uart2 rx/tx | `ark1668-pinctrl.dtsi:125-131` |
| 68-69 | PBANK_2 4-5 | uart3 rx/tx | `ark1668-pinctrl.dtsi:133-139` |
| 70-71 | PBANK_2 6-7 | **hw `&i2c0`** (DesignWare, clean/unconflicted; ACKs seen at 0x10/0x11) | live-confirmed, `boot_experiment_log.md` |
| 72-75, 77-78 | PBANK_2 8-11,13-14 | LCD LVDS group (defined but **inactive** — RGB888 selected instead; electrically free right now but reserved if LVDS mode is ever selected) | `ark1668-pinctrl.dtsi:73-86` |
| 79 | PBANK_2 15 | pwm1 (backlight-adjacent; treat as live given "Open backlight pwm" at every `start_msn` run) | `ark1668-pinctrl.dtsi:158-161,213-216` |
| 80 | PBANK_2 16 | pwm2 | `ark1668-pinctrl.dtsi:218-221` |
| 81 | PBANK_2 17 | pwm3 — **also actively GPIO-claimed as `lcd-power-control-gp`** | `ark1668-pinctrl.dtsi:223-226`, live `/sys/kernel/debug/gpio` |
| 82-84 | PBANK_2 18-20 | i2s1 sync/sadata/bclk (**active** — I2S/audio confirmed working) | `ark1668-pinctrl.dtsi:174-191`, `AUDIO_SUBSYSTEM_INVESTIGATION.md` |
| 97-99 | PBANK_3 1-3 | SPI clk/rxd/txd | `ark1668-pinctrl.dtsi:164-171` |
| 119-120 | PBANK_3 24,23 | uart4 rx/tx | `ark1668-pinctrl.dtsi:141-146` |
| 123-124 | PBANK_3 28,27 | uart5 rx/tx | `ark1668-pinctrl.dtsi:149-154` |
| 127 | PBANK_3 31 | i2s1 mclk | `ark1668-pinctrl.dtsi:193-196` |
| 128-129 | PBANK_4 0-1 | LCD LVDS cn/cp (inactive, same caveat as 72-78) | `ark1668-pinctrl.dtsi:81-82` |
| 130 | PBANK_4 2 | i2s1 sadata-in (**active**) | `ark1668-pinctrl.dtsi:198-201` |

## Confirmed claimed — GPIO-level only (not a pinctrl "function", plain `gpio_request`)

From live `/sys/kernel/debug/gpio` (2026-07-13):

| GPIO | Consumer | Notes |
|---|---|---|
| 0 | `rn6752_reset` | camera decoder reset |
| 1, 76 | `usb_id` | USB OTG ID pin, two GPIOs |
| 2 | `i2c-gpio-0` SCL | **= LCD r0, conflicted, see `I2C_GPIO0_LCD_PIN_CONFLICT.md`** |
| 3 | `i2c-gpio-0` SDA | **= LCD r1, conflicted** |
| 5 | `detect` | **reverse-gear/carback trigger input** — confirmed via Ghidra decompile of `ark_carback_probe`, 2026-07-13 session 3 (see below) |
| 81 | `lcd-power-control-gp` | also pwm3's pinctrl pin, dual use |
| 91 | `BTEN`/`gpio91` | **Bluetooth module enable pin** (RTL8762BTV/BT825) — confirmed working, `docs/WIRELESS_AND_INIT.md` §1,4 (`BTEN_INTERFACE=gpio91` in `/etc/blueware-bw121.properties`, toggled via plain `/sys/class/gpio/gpio91` export/direction/value). **Falls inside the previously-flagged 85-96 "unverified" range — now resolved for this one pin**: 91 is a real, safe, in-use GPIO, not unknown/unsafe. |
| 117, 126 | `usb_pwr` | USB power switch, two GPIOs |

## MMC/SD and USB — very likely dedicated, non-multiplexed pads, not in the PBANK_0-4 space at all

**Updated 2026-07-13, cross-checked against U-Boot source** (which also
has working LCD/USB/MMC, giving an independent second implementation to
compare against). `board_mmc_init()`
(`u-boot/board/arkmicro/ark1668_limcet_p305/ark1668.c:85-89`) calls only
`ark_dwmci_init("ARK_MMC0", 0xec400000, 4, 0)` — base address and bus
width, **zero pin/pad configuration code**. `drivers/usb/musb/ark_musb.c`
shows the identical pattern — no pad/pinmux config at all. Neither
codebase ever touches a pinmux for MMC or USB, in either U-Boot or
Linux, on top of Linux's `mmc0`/`mmc1` (`ark1668.dtsi:532-558`) having no
`pinctrl-0` property and no `cd-gpios` anywhere in the DTS.

**Most likely explanation: MMC and USB are on dedicated, non-multiplexed
silicon pads, physically outside the shared `PBANK_0-4` GPIO/pinmux
matrix entirely** — a common SoC pattern for high-speed/critical
interfaces. If true, this means the earlier theory in this doc (that the
incident hit a `PBANK_2`/`PBANK_3` pin adjacent to MMC's padcfg register
offset) was **likely wrong** — MMC's real pins probably aren't
reachable via `/sys/class/gpio` at all, so the brute-force scanner
couldn't have driven them directly.

**This means the actual cause of the 2026-07-13 incident
(`mmc0: card e126 removed` → EXT4 journal abort → full filesystem
corruption, recovered by power-cycle) is now genuinely uncertain, not
narrowed down.** Candidates: a transient electrical disturbance from
rapid GPIO switching activity (current draw / ground bounce affecting a
marginal SD card connection on the dev setup), or a coincidental
mechanical/socket contact issue unrelated to which pins were driven.
**Do not treat this as resolved or as evidence the tested range is
safe** — the honest state is "cause unknown," which is worse for
planning purposes than either a confirmed pin conflict or a confirmed
false alarm would be.

**Practical consequence unchanged: treat the entire
85-96/100-116/118/121-122/125 range as unverified-unsafe**, and don't
attempt further live GPIO brute-forcing there without a real
schematic/datasheet or a backed-up SD card to make failures cheap to
recover from.

## The core lesson (why this doc exists)

Three different peripherals — **uart0 (console!), NAND, and MMC** — are
all real, always-critical, and all **invisible** to both:
1. The static `ark1668-pinctrl.dtsi` group list (uart0/NAND are in
   there but never *selected*/applied via `pinctrl-0` on any node we
   found, or use a different application path — TODO: confirm how
   they're actually applied if not via the standard mechanism); MMC
   isn't in the pinctrl DTS at all.
2. Live `/sys/kernel/debug/pinctrl/*/pinmux-pins` debugfs, which only
   tracks pins claimed via the standard late `pinctrl_select_state()`
   API.

**"MUX UNCLAIMED" in debugfs means "not claimed via the standard
pinctrl API" — it does not mean "electrically free."** Any future live
GPIO probing must not rely on that signal alone. The only pins in this
document with genuine confidence behind "actively used, don't touch"
are the ones with a second, independent confirmation (a live
`/sys/kernel/debug/gpio` GPIO-level claim, a working feature we can
observe — audio, video, this exact serial link — or stock-kernel
disassembly).

## Still fully unverified (no claim found anywhere, but not incident-tested)

Global pins not listed above at all: 30-38, 53-61, 85-96, 100-116, 118,
121-122, 125 minus whatever MMC actually uses within 85-125ish (see
above — do not treat this whole remaining set as safe either, given the
demonstrated blind spot).

## Stock 3.4 kernel board-file static disassembly (2026-07-13, new method)

**New, safer methodology validated this session**: disassembling the stock 3.4
kernel's `vmlinux.elf` (`Prado firmware dump/mtd5_kernel/extracted/vmlinux.elf`
— unstripped, has `System.map`) directly gives the board file's hardcoded
GPIO/bus/platform-data assignments, with **zero hardware risk** — no live
probing, no telnet session, no risk of repeating the mmc-corruption incident.
It fully reproduced every fact the live `i2c-scan` investigation found, from
the binary alone:

- `ark1680_machine_init` (`0x8059f188`) calls `ark1680_add_device_i2c` (hw
  `i2c0`, 1 device) and `analog_i2c_add_device_i2c`/`_i2c3` (bus "1"/"3") with
  pointers into one static `struct i2c_board_info[]` table at `0x805b389c`
  (stride `0x28`/40 bytes — confirms the struct layout for this kernel build).
  `ark1680_add_device_audio` (`0x8059efb4`) separately calls
  `analog_i2c_add_device_i2c_2` (bus "2") pointing at the **same table's index
  0**.
- Reading the raw bytes of that table (`objdump -s -j .text
  --start-address=0x805b3894 --stop-address=0x805b3a60 vmlinux.elf`):
  - index 0 (`0x805b389c`): `"drv_bd37033"`, addr `0x41` → registered on
    **bus "2" (i2c-gpio2)**, count 3 — matches live `i2c-2: 0x41 XX` exactly.
  - index 3 (`0x805b3914`, table+0x78): `"Goodix-TS"`, addr `0x5d` →
    registered on **hw i2c0**, count 1 — matches the stock board file
    unconditionally declaring GT911 there (already known), now confirmed via
    raw struct bytes not just symbol names.
  - index 8 (`0x805b39dc`, within table+0xa0, count 6): `"rn6752"`, addr
    `0x2c` → registered on **bus "1" (i2c-gpio1)** — matches live `i2c-1:
    0x2c XX` exactly. Indices 4-7,9 in this 6-entry range are zero/unused.
  - index 10 (table+0x190, `analog_i2c_add_device_i2c3`): count 0 — bus "3"
    is a no-op, nothing registered.
  
  **Net effect: this single static disassembly, done cold with no live
  device access, reproduces 100% of the bus-placement ground truth the live
  `i2c-scan` session spent a whole investigation establishing.** This should
  now be the *first* step for any remaining unresolved peripheral
  (touch/MCU-UART/SWC/backlight/etc.) before any live probing is considered.

- **New finding, previously undocumented**: `customer_gpio_init`
  (`0x80018834`, called at the end of `ark1680_machine_init`) does
  `gpio_request(95, "apple_encpy_ic_rst")` then
  `gpio_direction_output(95, 1)` — **GPIO 95 is a reset line for an "apple
  encryption IC"** (likely a CarPlay/MFi auth chip), driven high
  unconditionally at boot. Not in any DTS or prior doc. Global pin 95 = bank 2
  offset 31 (95 = 2*32+31), i.e. `PBANK_2` offset 31 — **not yet
  cross-checked against the LCD/pinctrl pin-group tables**, do that before
  assuming it's free in the 4.9 DTS.

- **Structural finding — touch (`ark1680_ts`, the resistive-ADC driver)
  bypasses GPIO/pinctrl entirely**, per the reconstructed source
  (`hardware/ark1680_ts.c:56-67,246,263`): it maps the shared
  syscon/pinmux block directly at fixed physical address `0xe4900000` and
  clears specific bits — `ARK_SYS_PINMUX0` (offset `0x140`) bit 22, and
  `ARK_SYS_PINMUX1` (offset `0x144`) bit 14 — to route its ADC pads, with no
  `gpio_request`/`pinctrl_select_state` call anywhere. Its IRQ comes from a
  platform resource (`platform_get_irq`), not a GPIO. **This is the same
  "invisible to both static pinctrl DTS and live debugfs" pattern already
  flagged above for NAND/uart0/MMC** — confirms that pattern is common on
  this SoC for early/critical blocks, not a one-off. Any 4.9 port of touch
  needs to replicate this exact register poke (not a pinctrl group), or
  touch will silently fail to route to its pads even if the driver otherwise
  loads and probes cleanly.

### Continued (2026-07-13, session 2): hsuart/carback/IR device identities + a fourth mux layer

Disassembled `ark1680_add_device_hsuart` (`0x8059ee30`), `_carback`
(`0x8059f160`), `_ir` (`0x8059f174`) and their `platform_device` structs.

**Confirmed device names (high confidence — read directly from the string
table, same technique as the i2c names above):**
- `ark1680_add_device_hsuart`'s device is named **`"ark1680-hsuart"`** — this
  is the actual UART hardware controller behind `/dev/ttyHS0` (the MCU link
  used by `libMcuCenter.so`/`McuType=6`, see `MCU_ADAPTERS.md`). Its pad
  setup calls `ark_sys_pad_config(reg=0x1f0, mask=0xf, shift=2, val=0xf)`
  and `(reg=0x1f0, mask=0xf, shift=6, val=0xf)`.
- `ark1680_add_device_carback`'s device is named **`"ark_carback"`** —
  reverse-camera trigger.
- `ark1680_add_device_ir`'s device is named **`"ark_nec_sw_remote"`** — a
  software (bit-banged/edge-IRQ) NEC-protocol IR remote decoder, not a
  hardware IR block.

**New structural finding — a fourth, distinct pin-mux mechanism.**
`ark_sys_pad_config(reg, mask, shift, val)` (called throughout the board
file for i2c/uart/hsuart/audio/pwm/mci) does a read-modify-write of a
multi-bit field at a fixed `reg` offset (`0x1e0`, `0x1e4`, `0x1f0` seen so
far) within the same fixed physical `sys`/pinctrl0 MMIO block `ark1680_ts.c`
already showed being poked directly for touch (`ARK_SYS_PINMUX0`/`_1` at
`0x140`/`0x144`). So there are now **four** separate places pin/pad routing
lives on this SoC, and stock 3.4 only ever uses the first three — the 4.9
port's generic Linux pinctrl framework (`pinctrl-ark.c`, PBANK/DTS groups) is
a *different, newer* software abstraction over what is presumably the same
underlying hardware registers:
1. `ark_sys_pad_config()` raw field writes (i2c/uart/hsuart/audio/pwm/mci) —
   stock 3.4's primary mechanism, invisible to both the 4.9 DTS and live
   pinctrl debugfs.
2. Direct `ARK_SYS_PINMUX0/1` bit-clears (touch ADC pads) — same block,
   different offsets, also invisible to both.
3. Plain `gpio_request()`/`gpio_direction_*()` (e.g. GPIO 95
   `apple_encpy_ic_rst`) — visible in live `/sys/kernel/debug/gpio`, but not
   in pinctrl debugfs.
4. The 4.9 kernel's `pinctrl-ark.c` + DTS `ark,pins` groups — visible in
   both the DTS and live pinmux-pins debugfs, but **only for pins some DTS
   node actually selects via `pinctrl-0`** (this is the mechanism already
   covered everywhere else in this doc).

**Practical implication: "not found in the DTS or live pinctrl debugfs" is
not evidence a peripheral's pins are unconfigured or free — it may just mean
stock configures them through mechanism 1 or 2 instead**, which the 4.9 port
needs to replicate as raw register pokes (in the driver, or in an early
board-init hook) rather than expecting a pinctrl DT group to cover it.

**Unconfirmed — flagged rather than guessed, following this project's own
evidence-level convention (see `MCU_ADAPTERS.md`'s header):** attempts to
read GPIO/IRQ numbers out of the raw bytes of the `carback`/`ir`/`hsuart`
`platform_device` structs themselves (beyond `name`/`id`) are **not
reliable by hand** — `struct device`'s exact size on this kernel build is
config-dependent (`CONFIG_NUMA`/`CONFIG_CMA`/etc. all change field offsets),
so a byte offset that looks like a plausible small int (e.g. a `98` or `5`
found near these structs) could be genuine platform data or could be an
adjacent, unrelated static array — indistinguishable without either the
actual board-file source or a type-aware disassembler (Ghidra with the
kernel's struct definitions imported) rather than raw `objdump -s`. **Do not
treat any specific GPIO/IRQ number for carback/ir/hsuart's platform_data as
confirmed until re-derived that way.** No stock 3.4 board-file *source* was
found anywhere in this repo or the `linux-arkmicro` reference/build trees to
shortcut this (both only contain 4.x-era DT-based sources) — the compiled
`vmlinux.elf` is the only ground truth available for stock 3.4.

### Continued (2026-07-13, session 3): Ghidra-verified facts — GPIO 5 resolved, hsuart resources confirmed against the 4.9 DTS

Set up Ghidra 12.1.2 headless (`/home/osboxes/tools/ghidra`, `JAVA_HOME=/home/osboxes/tools/jdk/jdk-21.0.11+10`) against `vmlinux.elf`, ran full auto-analysis, then decompiled the actual driver **probe functions** (not just the board-file registration calls) for `ark_carback` and `ark1680-hsuart`. This is a materially stronger technique than reading raw struct hex by hand — the decompiler shows *how the driver code itself uses each field*, which resolves ambiguity no amount of hex-reading can: a byte offset is proven meaningful the moment you see the driver read it and hand it to `gpio_request`/`platform_get_irq`/etc., instead of being merely "a plausible small integer nearby."

**GPIO 5 resolved — reverse-camera trigger input.** `ark_carback_probe` (`0x80416880`) contains
`piVar1[0x19] = *(int *)(param_1 + 0x50);` followed by `__gpio_get_value()`,
`__gpio_to_irq()`, and `request_threaded_irq(irq, ark_carback_intr_handler, 0, 3, "carback", ...)`
on that value. Combined with the raw hex read that already found the literal value `5` at
`platform_device + 0x50` for this device, this **confirms GPIO 5 is the reverse-gear/carback
trigger input** — resolving the open item this doc has carried since the live-GPIO investigation
(`/sys/kernel/debug/gpio` labeled it only `"detect"`, unidentified). It's edge/level-triggered via
`gpio_to_irq`, not polled.

**hsuart resources decoded and independently confirmed by the 4.9 DTS.** `ark1680_hsuart_probe`
(`0x802fcf08`) loops `platform_get_resource(pdev, IORESOURCE_MEM, i)` /
`platform_get_irq(pdev, i)` for `i = 0, 1` — i.e. **one `platform_device` registers two
independent UART instances** (matches the `num_resources=4` found earlier by hex-reading the
struct). Reading the actual 4-entry `struct resource[]` array (`0x805c86c8`, `objdump -s`):

| Index | Type | Value |
|---|---|---|
| 0 | MEM | `0xe4f00000`-`0xe4f000ff` |
| 1 | IRQ | 22 |
| 2 | MEM | `0xe4800000`-`0xe48000ff` |
| 3 | IRQ | 28 |

**Cross-checked against the 4.9 DTS (`linux-arkmicro Reference/linux/arch/arm/boot/dts/ark1668.dtsi:488-505`) — it matches exactly**: `uart4: serial@e4f00000` and `uart5: serial@e4800000`, both `compatible = "arkmicro,ark-hsuart"` (the same custom driver, not generic 8250 — confirms `ark1680_hsuart_probe` is this driver's ancestor), aliased as `hsserial0`/`hsserial1` in the DTS's `aliases` node. **This means the 4.9 port's hsuart/MCU-UART node addressing is already correct** — `uart4` (`0xe4f00000`) is the MCU link (`/dev/ttyHS0` equivalent). If MCU communication isn't working on the 4.9 kernel, the bug is elsewhere (IRQ routing, clock setup, pinctrl group `pinctrl_uart4`, or the driver itself) — not the base address/IRQ mapping, which is now proven right.

**`uart5` (hsuart port 1, `/dev/ttyHS1`) confirmed — this is the Bluetooth UART, not a spare.**
Originally flagged here only as "very likely" Bluetooth from stock rootfs config
(`blueware-bw121.properties`/`rtkbt.conf` referencing `/dev/ttyHS1`). That's now
upgraded to confirmed: `docs/WIRELESS_AND_INIT.md` independently
documents this exact interface **already working** on a real build (Realtek
RTL8762BTV/BT825 module, `UART_INTERFACE=/dev/ttyHS1`, `UART_BAUDRATE=1500000`),
plus a GPIO enable pin (**GPIO 91**, `BTEN_INTERFACE=gpio91`, toggled via plain
`/sys/class/gpio/gpio91` export — no DTS `gpio-hog`/consumer node needed, it's
driven entirely from userspace/the Feasycom daemon). So the full picture for
`uart5`/`ttyHS1` is now: MMIO `0xe4800000`/IRQ 28 (Ghidra/board-file, this
session) + `compatible = "arkmicro,ark-hsuart"` (4.9 DTS, matches exactly) +
GPIO 91 enable (`wireless_and_init_documentation.md`, independently confirmed
working) — all three sources agree, and Bluetooth is a fully accounted-for
peripheral, not an open item. GPIO 91 also **resolves one pin out of the
"unverified 85-96" unsafe range** below — see the GPIO table above.

**`ark_nec_sw_remote` (IR) — not pursued further, not applicable to this unit.** Its
platform_data GPIO wasn't found via Ghidra this session (no data cross-reference to its name
string, unlike carback/hsuart, suggesting its driver is a `.ko` not present in this `vmlinux.elf`
or wired up differently) — but this is now moot: **confirmed this hardware revision has no IR
sensor populated**, so `ark_nec_sw_remote` is dead/inapplicable code on this unit and not worth
any further reverse-engineering time.

### Next steps for this method
- [x] Device *names* decoded for `carback`/`ir`/`hsuart` (see above) — `id`
      field and pad-config calls also confirmed.
- [ ] Get real GPIO/IRQ numbers for `carback`/`ir`/`_pwm_backlight`'s
      platform_data — requires loading `vmlinux.elf` into Ghidra/IDA with
      ARM Linux 3.4 `struct device`/`struct platform_device`/`struct
      resource` type definitions imported, so field offsets are computed
      correctly instead of guessed from raw hex. Manual `objdump -s` hex
      reading is not reliable enough for this (see caveat above) — don't
      repeat that approach without type info backing it.
- [ ] Confirm what `Goodix-TS`/index 3's `platform_data` pointer holds —
      resistive touch is what's actually `insmod`ed per `rcS`, but the board
      file *does* register a Goodix client unconditionally; its platform_data
      likely has an IRQ-GPIO/reset-GPIO pair worth extracting even though it's
      dormant on this unit. Same Ghidra-with-types caveat applies.
- [ ] Find the two more `ark_sys_pad_config` reg values already seen
      (`0x1e0` fields at shift 4/8/12/16, `0x1e4` fields at shift 18/20) and
      map every shift/val combination for every peripheral that calls it
      (mci, i2c, uart, audio, pwm) into one consolidated table — would fully
      characterize mechanism 1 (the primary stock pin-mux mechanism) instead
      of the partial picture gathered so far.
- [ ] Cross-check GPIO 95 against the `PBANK_2` pinctrl groups already in
      this doc (pwm1/pwm2/pwm3/i2s1 all live in `PBANK_2` too) before relying
      on it being free.

## Open items

- [ ] Confirm MMC/USB really are dedicated non-multiplexed pads (e.g.
      via a SoC datasheet/pad table) rather than assuming from the
      absence of pinmux code in U-Boot/Linux.
- [ ] The 2026-07-13 incident's actual cause is unresolved — worth
      revisiting if it's safe to retry a *much* more cautious live test
      (one pin pair at a time, SD card backed up first) to determine
      whether it reproduces at all.
- [ ] Confirm how/whether `pinctrl_nand`/`pinctrl_uart0` actually get
      applied if not through the mechanism live debugfs tracks — could
      reveal the same "early/fixed pad, not pinctrl-API-visible" pattern
      applies more broadly than currently documented.
- [x] Trace GPIO `5`'s `detect` consumer — resolved 2026-07-13 session 3: reverse-gear/carback trigger input (Ghidra decompile of `ark_carback_probe`).
- [ ] Revisit the GT911/rn6752/BD37033 real-wiring question with this
      corrected understanding once a safer verification method
      (schematic, or single-pin tests with the SD card backed up) is
      available.

## Cross-references

- `docs/HARDWARE_AND_SOC_REFERENCE.md` — plain-text block diagram of every
  peripheral/pin/GPIO in this doc, for a quick at-a-glance view. Keep both
  files in sync when either changes.
- `docs/DRIVER_TEST_PLAN.md` — concrete, per-peripheral test plan for every
  `PROBES OK`/`NOT CONFIRMED` row in the table above.
- `docs/DISPLAY_SUBSYSTEM.md` — the LCD/i2c-gpio-0 conflict.
- `docs/AUDIO_SUBSYSTEM_INVESTIGATION.md` — BD37033 investigation that
  led to all of this.


## PIN_BLOCK_DIAGRAM.txt

ARK1668 / Limcet-P306 (Prado head unit) -- pin & block map, plain text
======================================================================
Generated 2026-07-13. Companion to PIN_MASTER_LIST.md, which has full
sourcing/evidence detail for every line here -- this file is the quick
at-a-glance version, kept in plain ASCII so it's easy to hand-edit as
new pins get confirmed. Update both files together.

Global pin numbering = bank*32 + offset (e.g. PBANK_2 offset 17 = 81).
Matches /sys/class/gpio/gpioN 1:1 (confirmed live, no offset).

Confidence tags used below:
  [ACTIVE]     - confirmed in use right now (live debugfs / working feature)
  [BOARD-FILE] - confirmed via stock 3.4 board-file disasm (name+addr only,
                 or name+addr+resource if Ghidra-decompiled)
  [GHIDRA]     - confirmed via decompiled driver probe code (strongest)
  [DORMANT]    - board file registers it, but nothing loads/uses it on this unit
  [UNSAFE]     - do not live-probe, see incident note at bottom

Note (2026-07-14): [ACTIVE] here means "wiring/pin confirmed genuinely in
use," NOT "confirmed working end-to-end on the current 4.19 kernel build."
Those are different claims -- see PIN_MASTER_LIST.md's "Driver source
reference" table, whose "Confirmed by operation" column cross-checks each
peripheral against the actual boot log. Notably: rn6752 and BD37033 are
wiring-[ACTIVE] but NOT operationally confirmed this session.

A clean boot-log probe line is weak evidence, not proof, of correct
operation (user correction, 2026-07-14) -- a driver can log success while
the hardware behind it is silently wrong (no real response, bad data,
a failure mode dmesg never surfaces). Only that table's CONFIRMED status
means "actually verified working"; its PROBES OK status means "unverified,"
not "probably fine" -- don't read boot-log silence as reassurance.


Driver column key: "vendor, in-tree" = real vendor driver source already
present in the buildable linux-arkmicro tree (path given). "mainline" =
standard upstream Linux driver, nothing ARK-specific needed. "n/a" = no
kernel driver involved (userspace-only or stock-3.4-only). Paths are
relative to /home/osboxes/Downloads/linux-arkmicro/linux/ unless noted.
Full detail: PIN_MASTER_LIST.md's "Driver source reference" table.


ARK1668 SoC
|
|-- LCD controller (e0500000.lcd)                                    [ACTIVE]
|     RGB888 data  r0-r7/g0-g7/b0-b7 ---------------- pins 2-25
|     base (de/clk/vsync/hsync) ---------------------- pins 26-29
|     LVDS alt-mode (defined, NOT selected) ---------- pins 72-75,77-78,128-129
|     driver: vendor, in-tree -- drivers/video/fbdev/arkmicro/ark1668_lcdfb.c
|     also: /dev/ark_display misc ioctl shim (separate from the fb driver,
|     this project's own reimplementation) -- hardware/ark_display.c
|
|-- NAND controller ------------------------------------ pins 39-52
|     data/cle/ale/ren/wen/rb0/cen0
|     NOTE: not visible in live pinctrl debugfs; stock 3.4 doesn't route
|     it through the generic pinctrl API either -- exact stock mux
|     mechanism still unconfirmed, treat these pins as reserved/live.
|     driver: vendor, in-tree -- drivers/mtd/nand/raw/ark_nand.c
|     (ECC/OOB bug already found+fixed by this project, see HANDOFF_nand_ecc_uboot_vs_kernel.md)
|
|-- UART0 (console, the link this whole investigation used) -- pins 62-63
|     NOTE: same invisibility caveat as NAND -- works, but mux path unconfirmed
|-- UART1 ------------------------------------------------ pins 64-65
|-- UART2 ------------------------------------------------ pins 66-67
|-- UART3 ------------------------------------------------ pins 68-69
|     driver (uart0-3): vendor, in-tree -- drivers/tty/serial/ark_uart.c
|
|-- UART4 == hsuart port 0 == MCU link (/dev/ttyHS0)     [GHIDRA] -- pins 119-120
|     MMIO 0xe4f00000-0xe4f000ff, IRQ 22
|     matches 4.9 DTS uart4 node exactly (ark1668.dtsi:488-496)
|     pad mux: ark_sys_pad_config(reg=0x1f0, mask=0xf, shift=2, val=0xf)
|     driver: vendor, in-tree -- drivers/tty/serial/ark_hsuart.c
|
|-- UART5 == hsuart port 1 == Bluetooth (/dev/ttyHS1)     [ACTIVE][GHIDRA]
|     MMIO 0xe4800000-0xe48000ff, IRQ 28 -- pins 123-124
|     matches 4.9 DTS uart5 node exactly (ark1668.dtsi:498-505)
|     pad mux: ark_sys_pad_config(reg=0x1f0, mask=0xf, shift=6, val=0xf)
|     Realtek RTL8762BTV/BT825, UART_BAUDRATE=1500000, enable pin = GPIO 91
|     confirmed working -- docs/WIRELESS_AND_INIT.md
|     driver: kernel side = ark_hsuart.c (same as uart4, above); the BT
|     protocol stack itself is userspace (Feasycom/rtkbt, in the rootfs)
|
|-- HW I2C0 (DesignWare, e4300000.i2c) ------------------- pins 70-71   [ACTIVE]
|     pad mux: ark_sys_pad_config(reg=0x1e0, mask=0xf, shift=16, val=0xa)
|     driver: mainline -- drivers/i2c/busses/i2c-designware-{master,platdrv}.c
|     |-- Goodix-TS (GT911 touch), addr 0x5d      [BOARD-FILE][DORMANT]
|     |     registered unconditionally by stock board file, but gt9xx.ko
|     |     is never insmod'd on this unit (rcS loads ark1680_ts.ko instead)
|     |     driver (if ever needed): mainline -- drivers/input/touchscreen/goodix.c
|     `-- unidentified devices, addr 0x10, 0x11   [live i2c-scan only, not board-file]
|
|-- i2c-gpio1 (bit-banged bus "1") -- shares GPIO2/GPIO3 with LCD r0/r1 --+
|     bus driver: mainline -- drivers/i2c/busses/i2c-gpio.c (generic bit-bang)
|     `-- rn6752 camera decoder, addr 0x2c   [GHIDRA/BOARD-FILE][ACTIVE] |
|         needs the recurring rn6752_eq_work reset workaround seen      |
|         in every boot log -- plausible symptom of the pin-sharing     |
|         below, not necessarily fatal to it working at all             |
|         driver: vendor, in-tree -- drivers/soc/arkmicro/itu656/rn6752.c|
|                                                                        |
|-- i2c-gpio2 (bit-banged bus "2") -- sda_pin=9, scl_pin=121 ------------+
|     `-- BD37033 audio codec, addr 0x41    [GHIDRA/BOARD-FILE][ACTIVE]
|         write-timeout log lines on every boot are a benign probe-time
|         quirk present even in stock's own firmware, not a wiring bug
|         driver: vendor, in-tree -- sound/soc/arkmicro/BD37033.c
|
|-- i2c-gpio3 (bit-banged bus "3") -- registered, EMPTY (count=0) -- no devices
|
|-- PWM1 ------------------------------------------------- pin 79      [ACTIVE]
|     backlight ("Open backlight pwm" logged every start_msn run)
|     pad mux: ark_sys_pad_config(reg=0x1e4, mask=0x3, shift=18, val=1)
|-- PWM2 ------------------------------------------------- pin 80
|-- PWM3 ------------------------------------------------- pin 81
|     == GPIO81 lcd-power-control-gp (same physical pin, dual use)
|     driver (pwm1-3): vendor, in-tree -- drivers/pwm/pwm-ark.c
|
|-- I2S1 (audio) ------------------------------------------ pins 82-84, 127, 130   [ACTIVE]
|     sync/sadata/bclk (82-84), mclk (127), sadata-in (130)
|     pad mux: ark_sys_pad_config(reg=0x1e4, mask=0x3, shift=20, val=1)
|     driver: vendor, in-tree -- sound/soc/arkmicro/ark_i2s.c,
|     ark1668-sddac-codec.c, ark1668-sdadc-codec.c, ark_dac_codec.c, ark_adc_codec.c
|
|-- SPI -------------------------------------------------- pins 97-99
|     clk/rxd/txd
|     driver: vendor, in-tree -- drivers/spi/spi-ark.c (compatible
|     "arkmicro,ark-ecspi" -- NOT spi-arke.c, that's the ARK1668e variant)
|
|-- ark_carback (reverse-gear trigger) ------ GPIO 5       [GHIDRA]
|     ark_carback_probe reads platform_device+0x50 as this GPIO, then
|     gpio_get_value() + gpio_to_irq() + request_threaded_irq("carback")
|     resolves the previously-unidentified "detect" consumer
|     driver: vendor, in-tree -- drivers/soc/arkmicro/ark-carback.c
|     (uses devm_gpiod_get(pdev, "detect") -- independently confirms GPIO 5
|     straight from the real 4.9 driver source, not just Ghidra/disasm)
|
|-- ark_nec_sw_remote (IR remote decoder) --- GPIO not resolved
|     N/A on this hardware revision -- CONFIRMED no IR sensor populated,
|     dead code on this unit, not worth further RE time
|     driver: n/a
|
|-- customer_gpio_init() extra GPIOs:
|     GPIO 95 -- "apple_encpy_ic_rst" -- reset line for a CarPlay/MFi
|               auth chip, driven HIGH unconditionally at boot
|               [BOARD-FILE, new 2026-07-13] -- not yet cross-checked
|               against PBANK_2's pwm/i2s groups for pin conflicts
|               driver: n/a (stock-3.4-only board-init poke; no known
|               4.9 driver/binding needed unless this chip is confirmed present)
|
|-- Other plain-GPIO consumers (live /sys/kernel/debug/gpio, not pinctrl):
|     GPIO 0        -- rn6752_reset            (camera decoder reset)
|     GPIO 1, 76    -- usb_id                  (USB OTG ID pin x2)
|     GPIO 2        -- i2c-gpio-0 SCL == LCD r0 pin  [CONFLICT, see below]
|     GPIO 3        -- i2c-gpio-0 SDA == LCD r1 pin  [CONFLICT, see below]
|     GPIO 91       -- BTEN (Bluetooth enable) -- confirmed working, plain
|                       sysfs export/direction/value, no DTS node needed
|                       driver: n/a (userspace-only, Feasycom BT daemon)
|     GPIO 117, 126 -- usb_pwr                 (USB power switch x2)
|
|-- MMC0 -- 0xec400000, 4-bit bus -- likely dedicated non-multiplexed pads,
|           outside the PBANK_0-4 matrix entirely (no pinmux code found in
|           either U-Boot or Linux for it) -- NOT PROVEN, just likely
|           driver: mainline -- drivers/mmc/host/dw_mmc.c
|           (compatible "snps,dw-mshc", standard Synopsys DW-MSHC IP)
|
`-- USB (ark_musb.c) -- same "likely dedicated pads" caveat as MMC0
            driver: vendor, in-tree -- drivers/usb/musb/musb_ark.c
            (compatible "arkmicro,ark-musb")
            attached device: WiFi (RTL8811CU, plugged into USB, not a SoC
            pin/pad at all) -- driver: vendor out-of-tree, full source
            present -- drivers/net/wireless/realtek/rtl8811cu/


KNOWN PIN CONFLICT
------------------
i2c-gpio-0 (GPIO2=SCL, GPIO3=SDA) sits on the SAME physical pins as LCD
RGB888's r0/r1 data lines (both are PBANK_0 offset 2/3). RGB888 mode is
what's actually selected, so the LCD is constantly driving those pads
with video data. Confirmed live via pinmux-pins debugfs during working
video output. Practical effect: DON'T put anything new on GPIO2/GPIO3 --
rn6752 already lives here and tolerates it via a reset workaround; the
project's touch (GT911, dormant anyway) and BD37033 investigations both
originally got misdiagnosed by assuming this bus was clean. Full writeup:
docs/DISPLAY_SUBSYSTEM.md


FOURTH PIN-MUX MECHANISM (why "not in the DTS" != "unconfigured")
-------------------------------------------------------------------
Stock 3.4 configures most pads via raw register pokes that are invisible
to BOTH the 4.9 DTS's pinctrl groups AND live pinmux-pins debugfs:
  1. ark_sys_pad_config(reg, mask, shift, val) -- multi-bit field RMW at
     fixed offsets in the shared syscon block (0x1e0, 0x1e4, 0x1f0 seen
     so far) -- i2c0/uart0-3/hsuart/audio/pwm1/mci all use this.
  2. Direct ARK_SYS_PINMUX0 (offset 0x140) / ARK_SYS_PINMUX1 (0x144)
     bit-clears -- used by touch (ark1680_ts) for its ADC pads.
  3. Plain gpio_request()/gpio_direction_*() -- visible in live
     /sys/kernel/debug/gpio but NOT in pinctrl debugfs (GPIO 95 etc).
  4. The 4.9 kernel's pinctrl-ark.c + DTS ark,pins groups -- the only
     mechanism visible in both the DTS and live debugfs, and only for
     pins some node actually selects via pinctrl-0.
When porting a peripheral to the 4.9 tree, check whether stock used (1)
or (2) for it -- if so, the 4.9 driver needs an equivalent raw register
poke; a pinctrl DT group alone won't reproduce it.


UNSAFE / UNVERIFIED PIN RANGES -- DO NOT LIVE-PROBE
-----------------------------------------------------
  30-38, 53-61, 85-96 (except 91, now confirmed BTEN), 100-116, 118,
  121-122, 125
A brute-force GPIO probing session on 2026-07-13 preceded an
`mmc0: card e126 removed` -> EXT4 journal abort -> full filesystem
corruption incident (recovered by power-cycle). Root cause was NOT
conclusively identified -- MMC/USB are believed to be on dedicated pads
outside this range, which would mean the incident's real cause is still
unknown (electrical disturbance / marginal SD socket contact are the
leading unproven theories). Treat "no claim found" for any pin in this
range as "unverified," not "safe." Any future live test on these pins
needs a backed-up SD card and one-pin-at-a-time discipline, not another
brute-force sweep. See PIN_MASTER_LIST.md's "Open items" for status.


SOURCES
-------
- PIN_MASTER_LIST.md          -- full sourcing/evidence for every line above
- I2C_GPIO0_LCD_PIN_CONFLICT.md
- AUDIO_SUBSYSTEM_INVESTIGATION.md
- ARK1680_TS_REVERSE_ENGINEERING.md
- MCU_ADAPTERS.md             -- McuType=6 (BoxP300), /dev/ttyHS0 protocol
- wireless_and_init_documentation.md -- Bluetooth (ttyHS1/GPIO91) + WiFi setup

## 11. Vendor reference block diagram (`docs/ARK1668 diagram.jpg`)

A genuine ArkMicro reference-design block diagram (not this specific
board's actual schematic -- several blocks below are confirmed, via
this project's own hardware work, to be populated differently on the
real Limcet P306 unit). Read 2026-07-27. Kept here as a cross-reference index
against everything else in this doc, not a replacement for it.

### Power / vehicle interface (left edge connector)

- `B+`/`GND` -> `DC-DC` converter -> `+5V`/`+3V3`/`+9V` rails
- `CAM PWR` -- power feed out to an external reversing camera
- `CAN H`/`CAN L` -> `CAN TRANSCEIVER` -> `Rx/Tx` into `MCU`
- `ACC`/`ILL`/`SWC` feed straight into `MCU` (ignition-accessory sense,
  illumination/dimming, steering-wheel-control resistor ladder)
- `ISO SOCKET` (radio antenna) -> `Tuner` (RF, own antenna) -> `I2C` to
  `MCU`
- Speaker wires `FR+/FR-`, `FL+/FL-`, `RR+/RR-`, `RL+/RL-`, and `CVBS`
  (analog composite video, presumably the reversing-camera feed) run
  to the `POWER AMP`/`DSP` area

### MCU

Hub for vehicle-side I/O: `CAN transceiver`, `ACC/ILL/SWC` (direct),
`Tuner` (I2C), `DSP` (I2C), `TFT`+`TOUCH PANEL`'s `BL_CTR` (backlight
control only -- **not** touch I2C), and a single **UART** to
`ARK1668`.

That UART matches `/dev/ttyHS0`, the real, already-confirmed MCU
serial link `MsnCoreApp` opens (see `MCU_ADAPTERS.md`). Notably, the
diagram shows touch's I2C going **straight to `ARK1668`**, not through
the MCU -- independent schematic-level confirmation of
[[project_touch_qws_env_gate]]'s finding (memory) that touch activation
is a Qt/QWS env-var gate reading the touch controller directly, not an
MCU-handshake-gated thing as an earlier theory in this project assumed.

### Display

`ARK1668` -> `LVDS` -> `TFT` panel (video); `ARK1668` -> `BL_PWR` ->
backlight power enable; `ARK1668` -> `I2C` -> `TOUCH PANEL` controller.

### Storage/memory

`NAND FLASH 256M` and `SDRAM 256M/512M`, direct bus interfaces to
`ARK1668`. This unit's real NAND is smaller than the reference design's
256M option (`nand: 128 MiB` in every real boot log).

### USB

`USB HOST` -> `USB2 SOCKET`; `USB OTG` -> shared between `USB CHARGE`
and `USB1 SOCKET`. Matches `usb0`/`usb1` in
[[project_usb_otg_host_mode_investigation]]/[[project_usb0_carplay_boot_mode_dtb]]
(memory).

### Apple Authentication

I2C to `ARK1668` -- the MFi/CarPlay licensing chip. Already tracked in
this doc as `GPIO 95` (`apple_encpy_ic_rst`, section 5/10 above),
presence on the actual unit still unconfirmed. The schematic confirms
it's I2C-attached, not just reset-line-attached -- worth an I2C bus
scan if wired CarPlay's MFi handshake is ever investigated.

**Presence and I2C address confirmed, 2026-07-31.** Three previously
separate pieces of evidence in this project connect into one clear
answer:

1. GPIO 95 (`apple_encpy_ic_rst`) is driven high (reset deasserted)
   unconditionally at boot by stock's `customer_gpio_init` (section 5
   above) -- the chip is expected to be present and ready, not
   optional/DNP hardware.
2. A live `i2c-scan` on hw `&i2c0` (the DesignWare controller, pins
   70-71) found **two unidentified devices, addr 0x10 and 0x11**
   (line ~552/973 above) -- never matched to anything in stock's
   static `i2c_board_info` table (which only covers `bd37033`,
   `Goodix-TS`, and `rn6752` on the bit-banged buses).
3. **Direct proof, from stock's own binary**: `firmware_source/
   mtd6_rootfs/usr/lib/MFITest` (a real, unstripped stock diagnostic
   tool, symbols intact) is a full MFi coprocessor test harness --
   `mfi_open`/`mfi_close`/`mfi_read`/`mfi_write`/`mfi_scan`/
   `mfi_get_device_id`/`mfi_get_certificate`/
   `mfi_get_cer_serial_number`/`mfi_process_challenge`/
   `mfi_process_challenge2`, referencing `MFI_REG_AUTH_CTRL_AND_STATUS`
   and opening `/dev/i2c-%d` directly (no kernel driver -- MFi
   coprocessors are driven from userspace via `i2c-dev`, standard for
   this class of chip). Disassembled `MFISerialportApp::
   initSerialPort()`'s call into `mfi_open()`
   (`objdump -d usr/lib/MFITest`, function at `0x120c4`): calls
   `mfi_open(bus=0, addr_param=0x22)`; inside `mfi_open` (`0x11a74`),
   the device path is built as `/dev/i2c-<bus>` and the `I2C_SLAVE`
   ioctl (`0x703`) is issued with `addr_param >> 1` as the 7-bit slave
   address -- `0x22 >> 1 = 0x11`. **This exactly matches one of the
   two live-scanned addresses (0x11) on exactly the bus the scan found
   them on (hw i2c0/`/dev/i2c-0`)**, independently confirming both the
   chip's presence on this real unit and its precise address.

Address `0x11` also matches the publicly documented I2C address for
one real Apple MFi Authentication Coprocessor generation (the 2.0C-era
`MFI337S3959`; the 3.0-era `MFI343S00177` instead defaults to `0x10`,
which is very plausibly what the scan's *other* unidentified address,
`0x10`, actually is -- either an alternate register bank on the same
chip or the coprocessor responding on both addresses depending on
boot/init state; not independently confirmed which).

**Net conclusion**: the Apple MFi/CarPlay auth chip IS populated on
this real unit, on hw `&i2c0` (`/dev/i2c-0` under Linux, DesignWare
controller, pins 70-71), 7-bit address `0x11` (with `0x10` a strong
secondary candidate for the same chip). `usr/lib/MFITest` is a ready,
stock, standalone diagnostic tool that can be run directly (no
`MsnCoreApp`/carplay daemon needed) to test the chip in isolation --
the natural next step if wired CarPlay's MFi handshake is ever
actually investigated on real hardware. Production code that also
references the `mfi_*` symbols: `usr/lib/libiap2link.so` (undefined
references to `mfi_close`/`mfi_get_cer_serial_number`/
`mfi_certificate`/`mfi_cert_len`, plus its own `mfi_get_certificate`
definition) -- this is the real, in-use CarPlay iAP2-link library, not
just the test tool.

### ARK7116 (reversing-camera decoder)

`BT656/601` digital video into `ARK1668`. On this actual board it's
populated as **RN6752** instead (confirmed via I2C probe,
`dvr_rn6752@2c`) -- see the chat/memory discussion the same day this
section was added. ArkMicro's own kernel commit history notes RN6752
alone has a known random power-on hang bug, and ARK7116(H) support was
added specifically to pair alongside it as a fix, so the two chips are
closely related alternates/companions, not unrelated options.

### Audio chain

`DSP` <-> `MCU` (I2C, control) and `DSP` -> `POWER AMP` -> speaker
wires (`RL/LL`, `RR/LR`, the amplified analog output). Separately,
`ARK1668` -> `Voice Processor` over **SPI + I2C + I2S**, and
`Voice Processor` <-> `MIC`/`BT` for hands-free audio (`MIC AUDIO`).
This "Voice Processor" is likely a distinct chip from the `DSP` block
-- probably an echo-cancellation/hands-free codec for Bluetooth calls,
separate from the main head-unit audio path (`Sound_BD37033`, the
I2S1/I2S2 codec pair `e4000000.i2s-dac`/`e8200000.i2s-adc` -- see
`AUDIO_SUBSYSTEM_INVESTIGATION.md`).

**Checked 2026-07-27: this Voice Processor chip is NOT in our device
tree.** `ark1668_limcet_p305.dts` has no SPI bus node at all (not even
a controller, let alone a device on one), and no real boot log shows
any SPI device probe -- the only `spi` string anywhere is the generic
PLL clock name (`spi-clk rate 162000000`), never an actual peripheral.
Our I2C buses are already fully accounted for by other things
(`i2c-gpio-0`: RN6752 `0x2c` + disabled/wrong-pinned GT911 touch
`0x5d`; `i2c-gpio-1`: BD37033 `0x40`) -- none is a plausible match for
a separate hands-free codec. Either this unit doesn't populate the
reference design's Voice Processor (relying on the BT chip's own
built-in echo cancellation instead), or it's populated but nobody's
gone looking for it on the SPI bus yet. Worth an SPI-controller
probe/pin audit if hands-free call audio quality ever becomes a target.

**Follow-up, same day: found the real stock AEC test mode, and it
doesn't need a separate chip.** `usr/lib/libSetting.so` has a genuine
`EchoCancellationWindow` class, launched via `FactoryWindow::
on_btnAEC_clicked()` -- an "AEC" button living in the same factory
test menu as `LCDTest`/boot-logo/calibration/reboot (the numeric-
keypad-gated menu behind this project's Limcet activation gate, see
`project_limcet_activation_gate` memory). Its methods: `startRecord()`/
`onPlayFinished()` (record mic audio, play it back), `micVolumeChange(int)`
(adjustable mic gain), and `AECDelayValues`/`AECDelayType` (a
configurable AEC delay parameter -- the acoustic delay between
reference/output and the mic's picked-up echo, a standard AEC tuning
knob). The actual cancellation algorithm is a **software/BT-stack
setting**, not hardware: `/etc/blueware.properties` has
`HFP_NREC=3` ("enable noise reduction and echo cancellation for voice
call"). Combined with the mic already routing through the SoC's own
internal ADC (`ahb:sdadc@0 <-> e8200000.i2s-adc`, confirmed in every
real boot log), this makes it very likely the schematic's separate
Voice Processor chip simply isn't populated on this unit -- AEC is
handled by `blueware`'s `HFP_NREC` algorithm over the SoC's own mic
path, not a dedicated SPI DSP. If this test mode or the underlying AEC
behavior is ever ported/reimplemented, `EchoCancellationWindow`/
`FactoryWindow::on_btnAEC_clicked` and `HFP_NREC` are the real entry
points -- not the SPI bus.

### Bluetooth

`BT` block -- UART to `ARK1668` (this is the already-documented
`ttyHS1`/GPIO91 link, see `wireless_and_init_documentation.md`), own
antenna, feeds `MIC AUDIO` to the Voice Processor.

### One inferred (not certain) connection

An `R/L` label sits on a line near the `ARK1668`/`MCU`/`DSP` area that
reads as a direct analog stereo audio pair from `ARK1668` down to `DSP`
(bypassing MCU) -- plausible (the head unit's own line-out feeding the
amp) but not confirmed from the image alone; flagged here rather than
stated as fact.

## 12. GPIO pins set/toggled directly by MsnCoreApp and its plugins (2026-07-28)

Prompted by "does MsnCoreApp/MsnFirstInit toggle any pins directly, e.g.
for touch activation?" Traced every real `GPIOOperater` (a genuine C++
class in `libMsnCommons.so`, with real `setValue`/`getValue`/`setDir`/
`setEdge` -- not just Qt/QWS environment-variable plumbing) constructor
call site across every binary that references it, via Ghidra decompile.
`objdump -T` first scoped which binaries even import the symbol:
`MsnCoreApp` itself, plus `libCanBus.so`, `libMcuCenter.so`,
`libFMRadio.so`, `libBTSender.so`, `libMsnSound.so`, `libSetting.so`.
**`MsnFirstInit` does not reference `GPIOOperater` at all** -- no direct
GPIO manipulation there; it only sets environment variables and
`insmod`s the touch kernel modules (see below).

Pin numbers below are the literal argument passed to
`GPIOOperater::GPIOOperater(int)` at each call site, decoded as the
same global `bank*32+offset` numbering as the rest of this doc, and
cross-checked against `ark1668-pinctrl.dtsi` for an existing
alternate-function claim (same method as the GPIO 95/Apple-auth pin
in section 5).

| Global GPIO | Bank/offset | Caller (class::function) | Binary | pinctrl claim |
|---|---|---|---|---|
| 30 | PBANK_0 30 | `CarSignalsWatch` (via `MsnCoreApp`) | MsnCoreApp | none -- spare |
| 31 | PBANK_0 31 | `CarSignalsWatch` (via `MsnCoreApp`) | MsnCoreApp | none -- spare |
| 30 | PBANK_0 30 | `CanBus_XinHang::CanBus_XinHang` | libCanBus.so | none -- spare |
| 33 | PBANK_1 1  | `CanBus_XinHang::CanBus_XinHang` | libCanBus.so | none -- spare |
| 34 | PBANK_1 2  | `CanBus_XinHang::onRecvMcuProtocol` | libCanBus.so | none -- spare |
| 34 | PBANK_1 2  | `Sound_BD37033`, `Sound_MCU`, `Sound_MCU_OnlyEQ`, `Sound_PT2312`, `MsnSoundPlugin` (all ctors) | libMsnSound.so | none -- spare |
| 33 | PBANK_1 1  | `SettingWindow::SettingWindow` | libSetting.so | none -- spare |
| 38 | PBANK_1 6  | `SettingWindow::SettingWindow` | libSetting.so | none -- spare |
| 36 | PBANK_1 4  | every `FMAdapter_*` ctor (ST7786/ST7703/Msn4730/ST7708/QN8035) | libFMRadio.so | none -- spare |
| 9  | PBANK_0 9  | `MCUAdapter_BoxP400::MCUAdapter_BoxP400` | libMcuCenter.so | **claimed: LCD r7** (see below) |
| 35 | PBANK_1 3  | `MCUAdapter_CarA300::msnAppStateChange` | libMcuCenter.so | none -- spare |
| 37 | PBANK_1 5  | `MCUAdapter_BoxP700`/`BoxP701`/`ZhongHang` ctors | libMcuCenter.so | none -- spare |
| 96 | PBANK_3 0  | `MCUAdapter_BoxP700`/`BoxP701` ctors | libMcuCenter.so | none -- spare |
| 98 | PBANK_3 2  | `MCUAdapter_BoxC270::MCUAdapter_BoxC270` | libMcuCenter.so | **claimed: uart rxd** (see below) |
| 102 | PBANK_3 6 | `MCUAdapter_Bagoo::MCUAdapter_Bagoo` | libMcuCenter.so | none -- spare |

**Critical finding: the MCU adapter this unit actually uses does not
touch GPIO at all.** `MsnProductInfo.ini` sets `McuType=6`, which is
`MCUAdapter_BoxP300` (already documented, `MCU_ADAPTERS.md`) -- its
full symbol table was checked directly (`objdump -T`) and it **never
calls `GPIOOperater`**, anywhere. It only talks over its UART port
(`getPortSettings`/`makeMCUProtocol`/`onRecvMcuProtocol`,
`/dev/ttyHS0`). Every GPIO-touching MCU adapter above
(`BoxP400`/`CarA300`/`BoxP700`/`BoxP701`/`BoxC270`/`ZhongHang`/`Bagoo`)
is a *different* board variant's adapter class, compiled into the same
shared `libMcuCenter.so` but never instantiated on this unit --
several of them (`BoxP400`'s GPIO 9, `BoxC270`'s GPIO 98) would
actually **collide with real, claimed pins** (LCD r7 and UART rxd
respectively) if they were ever mistakenly activated, which is exactly
the kind of cross-board-variant pin conflict this project has run into
before (see `I2C_GPIO0_LCD_PIN_CONFLICT.md`).

**No evidence of a touch-specific GPIO anywhere.** All six binaries
that reference `GPIOOperater` were checked; none has a touch-related
call site, and no `libSetting.so`/`libLauncher-Box.so`/
`libCarReversing.so`/`libMsnCommons.so` string suggests one either.
This reinforces (doesn't just fail to contradict) the existing
[[project_touch_qws_env_gate]] conclusion: touch activation on this
unit is genuinely just the Qt/QWS environment-variable gate
(`QWS_MOUSE_PROTO`/`QWS_ARK_MT_DEVICE`, set by `MsnFirstInit`) plus the
kernel driver's own always-on registration -- there is no pin
`MsnCoreApp` or `MsnFirstInit` toggles to "turn on" the touchscreen.

**What `MsnFirstInit` actually does around touch** (from its own
decompiled logic, no GPIO involved): `insmod`s *both*
`ark1680_ts.ko` (the real resistive touch driver, confirmed active)
*and* `gt9xx.ko` (Goodix capacitive -- matches the disabled/wrong-pinned
`gt911` DTS node, see section 11's Bluetooth/Apple-auth notes and
`ARK1680_TS_REVERSE_ENGINEERING.md`), then symlinks `/tmp/touch_export`
to whichever one's `/msnprofile/touch_*_export` config exists --
board-driver selection via a symlink+file-existence check, not a pin.

**Active-variant pin numbers still need confirming.** `CanType=0` and
`RadioType=0` in `MsnProductInfo.ini` select one of
`CanBus_OdieBenz`/`CanBus_XinHang`/`CanBus_XinRi` and one of the five
`FMAdapter_*` classes respectively -- the 0-indexed mapping wasn't
resolved this session (the plugin factory function found,
`libCanBus.so`'s `create()`, is just the Qt plugin entry point, not the
type switch). `SoundType=3` is already confirmed as `Sound_BD37033` via
real boot logs (`Sound_BD37033::muteSpeakerAtts`, matches
`AUDIO_SUBSYSTEM_INVESTIGATION.md`) -- and since every `Sound_*`
variant shares the same GPIO 34 regardless of which is active, that one
pin's relevance doesn't depend on resolving the exact class.

**Not yet decoded**: `libBTSender.so`'s `GPIOOperater` calls pass a
*runtime variable*, not a literal constant -- its actual pin number(s)
depend on tracing back to wherever that variable is populated (likely
a config read), not yet done.

### The protocol daemons have essentially zero GPIO interaction

Follow-up check, same session: does the *daemon* layer (the separate
binaries `MsnCoreApp`'s plugins launch over D-Bus/sockets -- see
`MSN_APP_ARCHITECTURE.html` band 03) touch any GPIO? `objdump -T` +
`strings` checked on every one directly:

| Daemon | GPIO interaction |
|---|---|
| `sink` (Android Auto) | none |
| `carplay` | none |
| `dbus-daemon` | none |
| `msncarlife` / `carlife` | none |
| `mirrlink` | none |
| `airplay` | none |
| `ECLink` | none |
| `blueware` | **yes** -- see below |

`blueware` is the sole exception, and it closes a loop rather than
opening a new one: `/etc/blueware.properties` sets
`BTEN_INTERFACE=gpio91` (a config key, not hardcoded), and the binary
writes it via plain sysfs (`/sys/class/gpio/export`,
`/sys/class/gpio/gpio91/direction`, `/sys/class/gpio/gpio91/value`) --
confirmed by the binary's own `BTEN_INTERFACE`/`EventBTChipsetPowerOn`/
`EventBTChipsetPowerOff` strings. This is the actual mechanism behind
the already-documented GPIO 91/`ttyHS1` Bluetooth power pin
(`wireless_and_init_documentation.md`) -- previously known to exist,
not previously traced to *how* it gets toggled. Notably `blueware`
uses plain sysfs, not the `GPIOOperater` C++ class every other GPIO
user above goes through -- consistent with it being a standalone C
daemon, not a Qt/C++ MsnCoreApp plugin.

Net result: none of the actual protocol/video daemons touch hardware
directly -- they're pure IPC/protocol engines, matching the
architecture diagram. All real GPIO interaction in this whole stack
happens either inside `MsnCoreApp`'s plugins (via `GPIOOperater`) or in
`blueware` (via sysfs), never in the daemons they launch.
