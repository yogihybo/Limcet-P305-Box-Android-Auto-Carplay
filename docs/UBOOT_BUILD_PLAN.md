# U-Boot Build Plan — Compiling a Fresh U-Boot from `linux-arkmicro` Source

**Status: planned, not started.** This documents what it would take to build a real,
source-compiled U-Boot for this board, using the vendor source now available in
[`linux-arkmicro Reference/`](../linux-arkmicro%20Reference/README.md). No build has been attempted
yet — this is the plan to follow when one is.

## Why bother

Every U-Boot workaround so far (`patch_uboot.py`, the `sdscript` self-contained SD-boot patch in
`docs/UBOOT_SDBOOT_INVESTIGATION.md` §8) exists solely because the Prado's actual, deployed `uboot.bin`
is a raw NAND dump with no reserved `CONFIG_ENV_SIZE` buffer — only ~52 bytes of genuinely safe space
for the compiled-in env, which is why the `sdscript` patch had to get so clever (load a boot *script*
instead of trying to fit a full `bootcmd` in 52 bytes). A real source build sidesteps that class of
problem entirely: it has a real, intentional env partition (see deltas below), so `bootcmd` can just be
a normal, readable command line, no script indirection needed.

## What's confirmed and what isn't

**Confirmed:** `linux-arkmicro` (`RD_Software/linux-arkmicro`, see
[`linux-arkmicro Reference/README.md`](../linux-arkmicro%20Reference/README.md) for the live URL,
commit, and toolchain) really does contain an `ark1668` U-Boot board target — `configs/ark1668_defconfig`,
`board/arkmicro/ark1668/`, `arch/arm/mach-arkmicro/` — for the same SoC family this project already
established the Prado's ARK1680 tracks (`docs/SOC_ARK1668_CROSSREF.md` §2).

**Not confirmed / real risk:** this is U-Boot **2018.07** with SPL+FDT boot, ~6 years newer than the
Prado's actual **2012.10** stock bootloader, built with a materially different toolchain (Linaro
gcc 7.x / Buildroot 2021.02.2 vs. the Prado kernel's gcc 4.9.4 / Buildroot 2018.08). Building the
`ark1668_defconfig` as-is targets ArkMicro's own reference eval board, not the Prado — every value in
the deltas table below needs to be checked, and several things (DDR3 timing, NAND ECC layout, whether
the SPL this produces is even compatible with the Stepldr already burned into this board) are
**genuinely unverified**, not just unconfigured. Treat this as "buildable and worth testing via SD
card," not "known to work."

---

## 1. Prerequisites

- **Linux or WSL build environment.** This project already requires WSL for `u-boot-tools`
  (`mkimage`) elsewhere (`docs/UBOOT_SDBOOT_INVESTIGATION.md` §8) — same requirement applies here, more so
  (a full kernel-style Kbuild needs a real Linux userland, not Git Bash on Windows).
- **Toolchain:** Linaro `gcc-linaro-7.3.1-2018.05` or `7.4.1-2019.02`, `arm-linux-gnueabihf-` target —
  see [`linux-arkmicro Reference/env.source`](../linux-arkmicro%20Reference/env.source). Any reasonably
  modern `arm-linux-gnueabihf-gcc` (Linaro or distro) should work; U-Boot is far less toolchain-sensitive
  than a kernel build. Sourcing the vendor's exact version first is the safer starting point.
- **Full source clone** (the copy in `linux-arkmicro Reference/` is a ~1MB citation slice, not
  buildable on its own — it's missing all of U-Boot's generic `common/`, `lib/`, `cmd/`, `fs/`, `scripts/`,
  Kbuild machinery):
  ```bash
  git clone http://121.15.164.102:3000/RD_Software/linux-arkmicro.git
  cd linux-arkmicro/u-boot
  ```
  (Plain HTTP — that's the server's own setup. ~75MB / 13,808 files for the full `u-boot/` tree alone;
  expect the initial clone to be slow, as it doesn't appear to honor partial-clone filters cleanly.)

## 2. Starting defconfig

```bash
source ../env.source   # sets PATH, CROSS_COMPILE=arm-linux-gnueabihf-, ARCH=arm
make ark1668_defconfig
```

This targets `board/arkmicro/ark1668/` — the same board directory referenced throughout, not
`ark1668e_*` (already ruled out as the wrong generation in `docs/SOC_ARK1668_CROSSREF.md` §2) and not
one of the other customer variants (`ark1668_aofan`, `ark1668_ft`, `ark1668_tyw_zksw`,
`ark1668_dongle_sim` — kept in `linux-arkmicro Reference/` for comparison only).

## 3. Config deltas needed — defconfig default vs. Prado's real values

All real values below are from `docs/PARTITION_LAYOUT.md` and `env/uboot-env.txt` (the live device's
actual, captured environment) — not guesses.

| Setting | `ark1668_defconfig` default | Prado's real value | Why it matters |
|---|---|---|---|
| `CONFIG_ENV_OFFSET` | `0x160000` | `0x120000` | Must point at the Prado's actual `U-boot-Env` partition start (`docs/PARTITION_LAYOUT.md`), not the reference board's offset. Same fix `docs/SD_BOOT_PLAN.md` already identified before this source was actually located. |
| `CONFIG_ENV_SIZE` | `4096` | ≤ `0x40000` (256K partition) | Defconfig's 4096 bytes already gives ~80× the ~52 bytes the raw-dump patching has been squeezed into — no need to enlarge unless you want extra headroom; must not exceed the real 256K partition. |
| `CONFIG_MTDIDS_DEFAULT` | `nand0=ark-nand` | `nand0=ark1680-nand` | Cosmetic (env var default, overridable post-boot) but should match the Prado's real env (`env/uboot-env.txt`) for consistency with existing tooling/scripts. |
| `CONFIG_MTDPARTS_DEFAULT` | reference board's own layout (`bootstrap`/`bootloader`/`fdt`/... — see `linux-arkmicro Reference/README.md`) | `docs/PARTITION_LAYOUT.md`'s full 12-partition table | Needed if anything reads the compiled-in default rather than the env-stored `mtdparts` (the live device already carries its own `mtdparts` string in its env either way, so this is a fallback/rescue-mode safety net, not a hard boot blocker). |
| `CONFIG_DEFAULT_FDT_FILE` | `"ark169.dtb"` | *(none — remove)* | The Prado kernel has **no devicetree support at all** — legacy ATAG boot, confirmed in `docs/SOC_ARK1668_CROSSREF.md` §2. Leaving `CONFIG_OF_LIBFDT=y` compiled in is harmless; the boot command must not actually pass an fdt argument to `bootz`/`bootm`. |
| `CONFIG_BOOTCOMMAND` | loads and passes `ark169.dtb` | Custom — see below | See §4. |
| Kernel bootargs (via `CONFIG_EXTRA_ENV_SETTINGS` or just set at runtime) | reference board's own | `console=ttyS0,115200n8 mem=180M earlyprintk=serial ubi.mtd=6 root=ubi0:rootfs rootfstype=ubifs rootwait ro` (NAND boot) or the SD-specific variant (`root=/dev/mmcblk0pN rootfstype=ext4 rootwait rw`) — both already in `env/uboot-env.txt` / `env/sdboot_script.txt` | Baking in the *correct* bootargs (not the reference board's) is what actually gets a kernel to boot cleanly — wrong `root=`/`rootfstype`/`ubi.mtd` here is a guaranteed kernel panic even with perfect DRAM/NAND init. |

### `CONFIG_BOOTCOMMAND` — no-FDT, UBI-aware boot

The defconfig's default boot flow assumes FDT. Replace it with something matching the Prado's actual
`nandboot`/`mmcboot` env logic (`env/uboot-env.txt`), 2-arg `bootz` (no fdt address):

```
setenv bootargs console=ttyS0,115200n8 mem=180M earlyprintk=serial ubi.mtd=6 root=ubi0:rootfs rootfstype=ubifs rootwait ro
nand read ${loadaddr} <kernel offset from PARTITION_LAYOUT.md> <kernel size>
bootz ${loadaddr}
```

For the initial SD-only test phase (§5), the simpler SD path is lower-risk and matches what
`env/sdboot_script.txt` already does — no NAND read at all:

```
setenv bootargs console=ttyS0,115200n8 mem=180M root=/dev/mmcblk0p2 rootfstype=ext4 rootwait rw
fatload mmc 0:1 ${loadaddr} zImage
bootz ${loadaddr}
```

## 4. Open risk items — verify, don't assume

These are things the source *could* answer but haven't been checked yet — do this before trusting a
build on real hardware:

1. **DDR3 timing/training** — `arch/arm/mach-arkmicro/ddr_ark1668.c` (`ddr3_sdramc_init()`,
   `ddr3_data_training()`) has real, board-specific DRAM controller parameters. `dram_init()` itself
   auto-probes *size* via `get_ram_size()` (reassuring — it's not hardcoded to the reference board's
   RAM size), but the DDR3 *timing/training* values are a separate, unverified concern: they're tuned
   for whatever RAM chip ArkMicro's reference eval board actually has, which may or may not match the
   Prado's board. No way to confirm this without either a datasheet/chip-marking cross-check
   (`Limcet Hardware/BOARD_ANALYSIS.md` territory) or testing on hardware.
2. **NAND ECC/BBT layout** — `CONFIG_SYS_NAND_ONFI_DETECTION` is set (auto-detects geometry from the
   chip's ONFI parameter page rather than hardcoding it), which is a good sign given the Prado's real
   NAND geometry (`nand_erasesize=0x20000`, `nand_oobsize=0x40`, `nand_writesize=0x800` — all in
   `env/uboot-env.txt`) isn't hardcoded anywhere in this defconfig to conflict with. Still worth a sanity
   check (`nand info`) at the first serial-console checkpoint below before trusting any NAND read/write.
3. **SPL size budget** — this defconfig produces an SPL (`CONFIG_SPL=y`) that must fit in the 128K
   `S-Loader` partition budget (`docs/PARTITION_LAYOUT.md`) if it's ever flashed there. Not a concern
   for the SD-only test phase (SPL fit only matters for actual NAND placement), but worth measuring
   before this project ever considers a NAND-side promotion.
4. **Stepldr/SPL compatibility** — the Prado's existing first-stage loader (`Nboot.bin`/Stepldr,
   `Limcet Hardware/BOARD_ANALYSIS.md`) already knows how to hand off to a raw U-Boot binary from SD
   (that's the entire existing SD-boot mechanism this project relies on). Whether it can also hand off
   correctly to *this* build's SPL entry point/header format is unverified — but doesn't need to be
   answered for the SD-boot test path below, since that path replaces `UBOOT.BIN` wholesale the same
   way the existing `sdscript` patch already does, and Stepldr's SD-boot handoff convention is already
   proven to work with a raw (non-SPL) U-Boot binary today. If this build's SPL output can't be made to
   match that same raw-binary handoff shape, that's a real blocker worth learning early via the SD test,
   not discovering after a NAND flash.

## 5. Build steps

```bash
git clone http://121.15.164.102:3000/RD_Software/linux-arkmicro.git
cd linux-arkmicro/u-boot
source ../env.source
make ark1668_defconfig
make menuconfig   # apply the CONFIG_ENV_OFFSET / CONFIG_MTDIDS_DEFAULT / CONFIG_MTDPARTS_DEFAULT
                   # deltas from §3, or edit configs/ark1668_defconfig + include/configs/ark1668.h
                   # directly and re-run `make ark1668_defconfig`
# edit CONFIG_BOOTCOMMAND per §3 (SD-boot variant first)
make -j$(nproc)
# Output: u-boot.bin (or u-boot-with-spl.bin if SPL is combined) — this is what gets tested as UBOOT.BIN
```

## 6. Test plan — SD card only, no NAND writes, until proven

Following this project's existing safety pattern (`docs/UBOOT_SDBOOT_INVESTIGATION.md` §4, §7 — NAND
mistakes are JTAG-only to recover from, SD mistakes are "pull the card out"):

1. **Serial console first.** Flash nothing — boot with the new binary as `UBOOT.BIN` on an SD card's
   p1 partition (same mechanism already proven for the existing raw-dump SD-boot path,
   README §5.0), monitor via the existing serial console setup (README §2.0). **Go/no-go:** does a
   U-Boot banner appear at all, with the expected version string? If it hangs or resets before any
   banner, DDR3 init (§4.1) is the prime suspect.
2. **Peripheral sanity checks at the U-Boot prompt** (interrupt autoboot): `nand info` (does it detect
   the same geometry as `env/uboot-env.txt` records — erasesize/oobsize/writesize?), `mmc list` / `mmc info`
   (does it see the SD card?), `fatload mmc 0:1 <addr> zImage` (can it actually read a file off p1?).
   Don't proceed past this step until all three work.
3. **Boot the actual kernel** using the SD-variant `CONFIG_BOOTCOMMAND` from §3 — same `zImage` and
   `root=/dev/mmcblk0pN rootfstype=ext4` SD-rootfs setup already used by `build_bootable_sdcard.sh`
   and `env/sdboot_script.txt`. **Go/no-go:** does the kernel banner appear and does it get as far as
   mounting rootfs, matching the same checkpoint this project already uses for the existing raw-dump
   SD-boot path?
4. **Only after step 3 succeeds repeatedly and reliably** does it become worth thinking about a NAND
   boot variant (§3's `nand read`/`ubi.mtd` `CONFIG_BOOTCOMMAND`) — still SD-tested first (the kernel
   and rootfs stay on SD; only the *bootcmd logic* being tested changes to the NAND-read form), never
   flashed to the live device's NAND directly.

**Explicitly out of scope for this plan:** flashing this build to the Prado's actual `S-Loader`/
`U-Boot`/`U-Boot_back` NAND partitions. That's a separate, later decision to make only after every step
above is confirmed working on real hardware over SD — consistent with this project's existing stance
(`docs/UBOOT_SDBOOT_INVESTIGATION.md` §7) that NAND-side U-Boot changes need real testing headroom
before being trusted at all.
