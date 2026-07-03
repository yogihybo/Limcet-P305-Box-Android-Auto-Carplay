# linux-arkmicro Reference

Third-party ArkMicro vendor U-Boot source, copied here for citation and as a starting point for
building a fresh U-Boot for this board — see [`docs/UBOOT_BUILD_PLAN.md`](../docs/UBOOT_BUILD_PLAN.md)
for the actual compile plan. Not built by anything in this repo as-is.

## Source

- Repo: `RD_Software/linux-arkmicro`, hosted on ArkMicro's own public Gitea instance:
  `http://121.15.164.102:3000/RD_Software/linux-arkmicro` (plain HTTP, no TLS — that's the server's
  setup, not a mistake here)
- Branch: `master` @ commit `676deb203cfcd197b099232a54fd250d56aa1454`. Checked all 5 branches
  (`master`, `luyuan`, `tianyouwei`, `weilai`, `zhonghong`) and all are the same U-Boot generation
  (see below); no tags exist.
- This is the real repo `docs/SD_BOOT_PLAN.md` and `docs/UBOOT_SDBOOT_INVESTIGATION.md` referenced as
  `~/Downloads/linux-arkmicro` — that path was on a previous session's machine and was never actually
  in this repo. This copy replaces that unverified reference with the real, verified thing.

## Critical caveat: this is a later BSP generation, not a source match for the Prado's stock U-Boot

| | Prado's actual U-Boot (live banner string) | This repo |
|---|---|---|
| U-Boot version | `2012.10` | `2018.07-linux4ark_1.0` |
| Boot method | Legacy ATAG (Prado kernel has no devicetree support at all — `docs/SOC_ARK1668_CROSSREF.md` §2) | SPL + FDT (`CONFIG_DEFAULT_FDT_FILE="ark169.dtb"`) |
| Build toolchain | gcc 4.9.4 / Buildroot 2018.08 (from the kernel's own banner) | Linaro gcc 7.3.1/7.4.1, Buildroot 2021.02.2 |
| NAND partition layout | `128k(S-Loader),512k(U-boot),512k(U-boot_back),256K(U-boot-Env),256K(arkdata),...` (`docs/PARTITION_LAYOUT.md`) | `128k(bootstrap),640k(bootloader),640k(bootloader_back),128k(bootloaderenv),128k(fdt),...` (`configs/ark1668_defconfig`) |

No branch or tag in this repo is closer to 2012.10 — this is simply how far ArkMicro's own BSP moved
in the ~6 years between the Prado's board design and now. Building this source produces a **new,
compatible-family U-Boot**, not a recompile of the Prado's exact stock bootloader. See
`docs/UBOOT_BUILD_PLAN.md` for what has to change to target the Prado specifically, and what remains
unverified (DDR3 timing, NAND ECC/BBT layout, SPL-vs-Stepldr compatibility).

## What's copied here vs. the full upstream repo

The full `u-boot/` tree in the upstream repo is the entire mainline U-Boot fork (13,808 files, ~75MB —
every board ArkMicro ever touched, plus unrelated upstream boards like Xilinx Zynq and Renesas). Only
the ARK1668-specific slice is copied here (~1MB, 89 files) — everything needed to *read and understand*
the SoC/board bring-up, not a buildable tree on its own:

```
u-boot/
  arch/arm/mach-arkmicro/        SoC arch code shared by all ark1668/ark1668e boards — clock.c,
                                   ddr_ark1668.c (DDR3 controller init/training — the file that
                                   actually answers "is DRAM init board-specific"), spl_ark1668.c,
                                   gpio.c, reset.c, timer.c, u-boot-spl.lds
  board/arkmicro/
    ark1668/                      The reference eval-board target — ark1668.c, ark1668_lcd.c,
                                   ark1668_hardware.h, ark1668_display_cfg.c
    ark1668-ft/, ark1668_aofan/, ark1668_dongle_sim/, ark1668_tyw_zksw/
                                   Other customer variants built on the same ark1668 target, kept
                                   for comparison (each is a different OEM's board file, same
                                   pattern as this project's own Prado-vs-Holden-vs-Alfa comparisons)
  configs/ark1668*_defconfig      Kconfig defconfigs for the above board targets
  include/configs/ark1668*.h      Board header configs (CONFIG_ENV_OFFSET, CONFIG_BOOTCOMMAND,
                                   CONFIG_MTDPARTS_DEFAULT, etc. — see UBOOT_BUILD_PLAN.md for the
                                   deltas needed against the Prado's real values)
  drivers/mtd/nand/ark_nand*.c    ArkMicro NAND controller driver (+ SPL variant)
  drivers/pwm/ark_pwm.c           Backlight PWM
  drivers/spi/ark_spi.c
  drivers/usb/musb/, musb-new/    ark_musb.c — USB MUSB OTG driver (2 variants, old/new musb stack)

linux/                            Kernel-side devicetree source (the "public linux-arkmicro tree"
                                   already cited throughout docs/SOC_ARK1668_CROSSREF.md — this is
                                   the first time it's actually vendored into the repo rather than
                                   referenced from memory)
  arch/arm/boot/dts/
    ark1668.dtsi / ark1668-pinctrl.dtsi     The SoC this board actually matches — full pin-mux
                                              table, used to build the pinctrl0 section of
                                              Limcet Hardware/ark1668-limcet-prado.dts
    ark1668e.dtsi / ark1668e-pinctrl.dtsi   Newer generation, kept only for contrast — this is
                                              where can0/can1 pin-mux actually lives (NOT in the
                                              ark1668 files — see the CAN correction in the
                                              synthetic dts's top comment block)
  include/dt-bindings/pinctrl/ark-pinfunc.h  ARK_PBANK_n / ARK_PVAL_n / ARK_PDRV_n macros used
                                              throughout the pinctrl dtsi files

env.source                        Toolchain setup script (Linaro gcc path + CROSS_COMPILE/ARCH export)
```

Not copied: ark1668**e** (the newer, architecturally-different generation — already established in
`docs/SOC_ARK1668_CROSSREF.md` §2 as NOT matching this SoC), the rest of upstream U-Boot's generic
kbuild/common/lib/fs infrastructure (needed to actually build — clone the full repo for that, per
`docs/UBOOT_BUILD_PLAN.md`), and `buildroot`/`buildroot-external` (a separate, much larger companion
tree in the same repo, unrelated to U-Boot itself).
