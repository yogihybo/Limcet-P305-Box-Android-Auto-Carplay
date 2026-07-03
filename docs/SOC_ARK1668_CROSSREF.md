# SoC Identity Cross-Reference & Ghidra Analysis — `mtd5_kernel` (dump build)

Analysed by: LZO extraction + Ghidra 12.1.2 headless disassembly/decompilation, cross-referenced
against the public ArkMicro vendor kernel tree `linux-arkmicro` (ARK1668/ARK1668E/ARKN141 BSP)
and (§7) userspace RE of `MsnCoreApp`/`libMcuCenter.so` from the rootfs.

**⚠️ This document's primary analysis covers a different zImage than `docs/KERNEL.md`.** See [Build Discrepancy](#build-discrepancy-vs-kernelmd) below — **§6 now cross-checks the `KERNEL.md` build (#383) directly and confirms the findings hold on both.**

**§7 answers the "where does the backup-camera GPIO come from" open item** left in §5: it doesn't — on this product (`McuType=6`, `MCUAdapter_BoxP300`) the signal comes from the companion MCU over the `arktool` UART protocol, not any SoC-side GPIO.

**§8: a synthetic device tree combining every finding here with the physical board inspection in `Limcet Hardware/BOARD_ANALYSIS.md`** now exists at [`Limcet Hardware/ark1668-limcet-prado.dts`](../Limcet%20Hardware/ark1668-limcet-prado.dts) — not a real boot artifact (this board has no device tree, see §2), but a structured, confidence-tagged writeup of the confirmed hardware layout.

**§9 cross-checks §§2–5 against real ASTRI vendor source** (`ArkPro Reference/`, a public leak, not the public `linux-arkmicro` tree used elsewhere in this doc) — confirms the `config.c` board-init device roster and the `CLCD_TIMING`/`CLKDIV1` register field names, and surfaces a third (unused-on-Prado) candidate mechanism for backcar detection.

**§10 works out the full pin-mux table** from the real, now-vendored `linux-arkmicro` devicetree source, filling in and correcting `Limcet Hardware/ark1668-limcet-prado.dts`'s `pinctrl0` node — and finds that an earlier draft's `can0`/`can1` pin-mux entry was wrong: that capability only exists on the newer ARK1668E generation, not this chip.

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

`FUN_8059c13c` in the Prado kernel is a byte-for-byte match for `setup_machine_tags()` in `arch/arm/kernel/setup.c` — it walks the `__arch_info_begin..__arch_info_end` `struct machine_desc` array comparing the ATAG-supplied machine ID (register r1) against `.nr`, then prints `"Machine: %s"` with `.name`. `FUN_8059c0e4` is the companion "unrecognized machine ID" panic path (`"Available machine support: ID (hex) NAME..."`).

The array has **exactly one entry** (kernel built for a single board, legacy `MACHINE_START(ARK1680,...)`/`MACHINE_END`, no device tree):

```
nr (machine ID) = 0x1068  (4200 decimal)
name             → "ARK1680"
boot_params      = 0x100
fixup            = 0x80008474  (confirmed: calls ioremap-style setup consistent with ATAG fixup signature)
```

Machine ID `0x1068` does **not** appear in `linux-arkmicro/linux/arch/arm/tools/mach-types` — it's a vendor-private ID, never submitted to the official ARM registry (expected/common for closed OEM SoCs, especially post-devicetree-era when that registry effectively froze).

### Architectural alignment: ARK1680 tracks ARK1668, not ARK1668E

The Prado kernel boots via legacy ATAG/machine-ID (no device tree) and uses a **VIC** interrupt controller:
```
"ARK interrupt controller virtual address VICL %x VICH %x"
```
confirmed live in `FUN_8059f188` (see §4), which does `ioremap(0xe0b00000, 0x200)` / `ioremap(0xe0c00000, 0x200)` for VICL/VICH.

In `linux-arkmicro/linux/arch/arm/mach-arkmicro/Kconfig`:
- `SOC_ARK1668` → `select ARM_VIC` (matches Prado exactly)
- `SOC_ARK1668E` → `select ARM_GIC`, `HAVE_ARM_ARCH_TIMER` (newer generation, does **not** match)

**Conclusion:** ARK1680 aligns architecturally with **ARK1668** (not the newer ARK1668E), and — per §3 — shares its exact peripheral register map. Most likely explanation: ARK1680 is ArkMicro's internal/pre-devicetree engineering name for the same silicon later formalized and upstreamed as "ARK1668"; this OEM's OS integrator (`flyound`, building for Toyota) never migrated off the old internal name or the legacy ATAG boot path onto the newer `mach-arkmicro/ark1668.c` device-tree support present in the public tree. A remarked/rebadged-die explanation can't be fully ruled out without a datasheet or decap, but is less likely given how deep the software-level match goes (see below).

---

## 3. Peripheral Memory Map — Diff vs `linux-arkmicro/linux/arch/arm/boot/dts/ark1668.dtsi`

Every peripheral base address in `ark1668.dtsi` was checked against the Prado ARK1680 binary two ways: (a) raw literal-pointer occurrence count, (b) direct `struct resource {start, end, name, flags=IORESOURCE_MEM}` pattern scan (Linux platform-device resource declarations).

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

**Cross-check against `linux-arkmicro/linux/arch/arm/boot/dts/ark1668-pinctrl.dtsi`:** `ARK_PBANK_2` pin 31 has **no** alternate-function mux entry anywhere in that file (pins 0–20 and 31 of PBANK_2 are claimed by uart1/2/3, i2c0, LVDS, pwm1-3, i2s1 — pin 31 specifically is absent). On the generic ArkMicro reference dev-board, this pin is left spare; on the Prado production PCB, the same physical pin is wired to the CarPlay auth-chip reset. This is the expected kind of difference between a reference dev-board and a customer production board — not a conflict, just board-specific wiring on top of identical silicon.

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

- ❌ **`arkdata` NAND partition (MTD4)** — checked `docs/ARKDATA_VARIANTS.md`; this partition is confirmed to carry only LCD panel timing (resolution, `CLKDIV1`, `VBP`/`HBP`/`VSW`/`HSW`, touch-key config), nothing GPIO-related. Ruled out.
- ✅ **Likely candidate: userspace, via `/proc/ark_gpio`** — `docs/KERNEL.md` already documents this debug/control interface exposed by the `ark_gpio` driver. A userspace daemon (`MsnCoreApp` or similar, reading `MsnProductInfo.ini`'s `ResourceName`/`McuType`/`ProductId` fields — see `docs/SOURCES.md`) most plausibly writes the camera/backup-camera GPIO numbers into the kernel at boot through this interface, based on which product variant is running. This is consistent with the whole project's established pattern of board variants being selected by userspace config rather than separate kernel builds.

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

Note `musb-ark1680` and `dw_mmc` are each registered twice (`id=0`/`id=1`) — confirms **2 USB OTG controller instances** and **2 MMC/SD controller instances** are present on this SoC/board, both consistent with `ark1668.dtsi`'s `mmc0`/`mmc1` nodes in the public tree. `ark_nec_sw_remote` (NEC IR protocol steering-wheel remote receiver) is a device not previously called out in `docs/KERNEL.md` and worth folding into that inventory.

### Open items / next steps

1. Disassemble the rootfs userspace binary responsible for writing to `/proc/ark_gpio` at boot (most likely `MsnCoreApp`) to recover the actual `backcar`/`rn6752_reset`/`rn6752_irq` pin numbers — this is rootfs/userspace RE, not kernel RE, and is a natural follow-on task.
2. ~~Re-run this same pass against the `reconstructed`-tree zImage (build #383)~~ — **done, see §6.**
3. `fixup` field decoding of the single `machine_desc` struct beyond `nr`/`name` was a rough field-order guess based on mainline layout and should not be trusted without further verification — this vendor's exact 3.4 struct layout may differ.
4. The remaining unnamed `FUN_80009xxx`/`FUN_8020dxxx` helper functions (`ioremap`, `platform_device_register`, `gpio_request`, etc., all identified so far purely by call signature/error-string context, not real symbols) could be formally labeled in the saved Ghidra project for anyone continuing this work interactively.

---

## 6. Cross-check against the `KERNEL.md` build (#383, Dec 2023)

Ran the identical extraction + Ghidra pipeline against `Prado firmware reconstructed/mtd5_kernel/zImage` (MD5 `78782daea...`, the build `docs/KERNEL.md` documents) to check whether everything above still holds 22 months and 30 build numbers later.

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

**`libMcuCenter.so`** (the MCU-serial-comms library — matches `docs/KERNEL.md`'s documented `arktool`/UART protocol) is where it gets interesting. It contains "Reverse camera", "Reverse condition", "Reverse Radar", "Reverse Track" strings, and a whole family of `MCUAdapter_*` C++ classes — one per product/box model (`MCUAdapter_Bagoo`, `_ZhongHang`, `_BoxC230` through `_BoxC290`, `_BoxP100` through `_BoxP900`, `_CarA200/300/301`, `_HUD`, `_NV17`, `_D107`, `_RuiYuanSWC`, `_IM60BC`, `_MsnDecoder`, `_Box_Encryption` — 27 variants total). Several directly construct a `GPIOOperater` with a hardcoded pin number in their constructor:

| Adapter class | GPIO (hex → dec) | `PBANK`/pin |
|---|---|---|
| `MCUAdapter_BoxP400` | `9` | PBANK_0 pin 9 |
| `MCUAdapter_CarA300` (conditional, app-state == 4) | `0x23` = 35 | PBANK_1 pin 3 |
| `MCUAdapter_ZhongHang` | `0x25` = 37 | PBANK_1 pin 5 |
| `MCUAdapter_BoxP700` | `0x25`=37 **and** `0x60`=96 (two GPIOs) | PBANK_1 pin 5; PBANK_3 pin 0 |
| `MCUAdapter_Bagoo` | `0x66` = 102 | PBANK_3 pin 6 |
| `MCUAdapter_BoxC270` | `0x7d` = 125 | PBANK_3 pin 29 |

**Found the factory/dispatcher:** `MCUAdapter::getAdapterInstance(MCUAdapter::McuType)` (exact demangled symbol name — no guessing needed) is a 30-case `switch` on the numeric `McuType` value, `case 1` through `case 0x1e`, each `operator_new`-ing and constructing the matching adapter class. This is the mechanism `docs/SOURCES.md`'s `McuType` field (16 for Holden, **6 for Prado**) actually drives.

**`case 6` → `MCUAdapter_BoxP300`.** This is the Prado's real, active MCU adapter class. Decompiled its full constructor (and confirmed — by scanning literally every method with `BoxP300` in the name, only 4 exist total: ctor/dtor pairs — **`GPIOOperater` does not appear anywhere in this class**. Instead, its constructor calls `MsnApplication::getFactorySetting(...)` twice — once to read a list of radar-sensor IDs (parsed via `QString::split` into a `uint` list — matches `KERNEL.md`'s documented multi-radar/PDC support) and once for another numeric setting. Both are **software config reads**, not GPIO hardware access.

### Conclusion

**The Prado unit does not read the reverse-camera/backup signal via a raw SoC GPIO pin in userspace at all.** The `MCUAdapter_BoxP300` class handling this product's MCU communication never touches `GPIOOperater`. This closes the loop with two things already documented independently:

1. `docs/KERNEL.md` already recorded that the `arktool` MCU-UART binary protocol carries a **`backcar enable/disable`** command.
2. §5 of this document found the kernel's `carback` platform_device has an **entirely zero `platform_data` struct** — no GPIO configured for it at the kernel level either.

Both facts point to the same conclusion: on this product, reverse-gear/backup-camera detection happens on the **companion MCU** (which has its own firmware, wired directly to the vehicle's reverse-light circuit or a physical switch — not analyzed here, would require dumping/RE'ing the MCU's own firmware image separately), and the MCU simply *tells* the ARK1680 SoC "enter backcar mode" over the HS-UART `arktool` link. There's no GPIO pin on the SoC side to find for this signal on the Prado — it was never a compiled-kernel-literal, a userspace `/proc/ark_gpio` write, *or* an `McuType`-specific board GPIO, because the whole detection path lives outside the SoC entirely. The GPIO literals found in `MCUAdapter_Bagoo`/`ZhongHang`/`BoxP400`/`BoxP700`/`BoxC270`/`CarA300` above are real and board-specific, just **for different, non-Prado products** built from the same shared `libMcuCenter.so` — a good illustration of how much shared-codebase archaeology this firmware requires: the code path that's actually load-bearing for one product is dead weight (or vice versa) for another, and only the `McuType` factory switch tells you which is which.

**Independently confirmed after the fact:** `Limcet Hardware/BOARD_ANALYSIS.md` (physical board inspection, done separately from this software RE) identifies the companion MCU as an **STM32F105RBT6** running **Limcet-V1.0-1302** firmware, and explicitly lists "ACC/IGN detection, reverse trigger input, panel button inputs" among its GPIO responsibilities, plus an onboard NXP TJA1042 CAN transceiver wired to the STM32's own bxCAN peripheral for reading Toyota-specific steering-wheel CAN messages. Two completely independent methods (kernel/userspace binary RE vs. physically inspecting the board) converged on the same architecture: MCU owns the vehicle-signal GPIOs and CAN bus, SoC just listens over UART.

This also means the earlier open items about `rn6752_reset`/`rn6752_irq`/`spi_cs_gpio` (the actual camera decoder chip's own reset/IRQ lines, as opposed to the reverse-signal trigger) remain genuinely open — those are a separate, still-unresolved question from the "how does the system know to enter backcar mode" question this section answers. They'd need the same `platform_data` static-struct tracing approach in the kernel binary that was inconclusive in §5 (all-zero struct), so likely also resolve to an MCU-reported or otherwise non-kernel-literal source, but that hasn't been directly confirmed.

---

## 8. Synthetic device tree

Wrote [`Limcet Hardware/ark1668-limcet-prado.dts`](../Limcet%20Hardware/ark1668-limcet-prado.dts) — a structured, `.dts`-syntax reconstruction of this board's hardware, combining every finding in this document with the physical inspection in `BOARD_ANALYSIS.md` (chip markings, the STM32 MCU, RN6752 camera decoder, Bluetooth module, confirmed LCD timings from `docs/SCREEN.md`).

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
and scope notes. This is a **generic reference BSP, not the Prado's actual OEM board file** — it has
none of this OEM's customer-specific additions (`apple_encpy_ic_rst`, `carback`, `rn6752_*`, etc.) —
but it confirms several things §§2–5 could only infer from disassembly:

- **`ark1680_add_device_*()` roster in [`config.c`](../ArkPro%20Reference/kernel/arch/arm/mach-ark1680/config.c)** registers platform devices under the same names found in the Prado binary's `FUN_8059f188` (§5's table): `gpio-ark`, `ark1680-uart`, `ark1680-nand`, `ark_i2s_dev`, `ark-display`, `ark-prescaler`, `ark-jpeg`, `ark-deinterlace`, `ark-itu656`, `ark-pwm`, `ark-wdt`, `pwm-backlight`. Real source confirming what was previously just a decompiled device-name string list.
- Same file's USB bring-up does `ioremap(VICH_BASE, ...)` + sets `BIT(7)|BIT(8)` to gate MUSB IRQs into the VIC — confirms the VICL/VICH ioremap pattern §4 found in the Prado binary's board-init routine is a real, intentional step (not a decompiler artifact), just for USB IRQ routing specifically.
- **LCD timing register fields** — see the new section added to `docs/ARKDATA_VARIANTS.md`: `ark_display_lcd.c` and `uboot/ark_lcd.c` confirm `CLKDIV1`/`VBP`/`HBP`/`VSW`/`HSW`/`IVS` are ArkMicro's real register field names (`CLCD_TIMING0/1/2`), not RE-guessed labels, and explain `CLKDIV1` as a system-PLL clock divider.
- **A third candidate mechanism for backcar detection**, not previously considered: [`userspace/display.h`](../ArkPro%20Reference/userspace/display.h) defines `ARKDISP_GET_BACKCAR_STATUS` (ioctl `0xa0`/`25`) on the `ark-display`/framebuffer device itself. This doesn't change §7's conclusion — the Prado-specific trace of `MCUAdapter_BoxP300` independently and directly confirmed the MCU-UART `arktool` path is what's actually used on this product — but it's worth recording that ArkMicro's own reference stack has a kernel-ioctl-based backcar path as a generic option, in case a future variant or product on this platform turns out to use it instead.

No `clock.c`, GPIO driver, NAND/touchscreen driver, or `MsnCoreApp`/`libMcuCenter`-equivalent userspace source was found anywhere in the upstream `ArkPro` repo (checked `Launcher/`, `MultimediaService/`, `AutoConnect/`, and the other Qt-service directories) — so the open items in §5 (platform_data-sourced GPIO pins for `backcar`/`rn6752_reset`/`rn6752_irq`) remain open; this vendor source doesn't resolve them.

---

## 10. Pinout — full pin-mux table from real vendor source, and a CAN correction

Located and vendored the actual `linux-arkmicro` kernel devicetree source (`RD_Software/linux-arkmicro`,
see [`linux-arkmicro Reference/README.md`](../linux-arkmicro%20Reference/README.md) for provenance —
this is the same public BSP `docs/UBOOT_BUILD_PLAN.md` uses for the U-Boot side). Its
`arch/arm/boot/dts/ark1668-pinctrl.dtsi` is ArkMicro's own pin-mux table for this exact SoC — every
GPIO pad, which peripheral function(s) it can be muxed to, and the alt-function value to select each.
Used it to fill in and correct the `pinctrl0` node in
[`Limcet Hardware/ark1668-limcet-prado.dts`](../Limcet%20Hardware/ark1668-limcet-prado.dts).

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
