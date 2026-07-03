# ArkPro Reference

Third-party vendor source, copied here for citation — **not written for this project, not built by
any script here.** Referenced from [`docs/SOC_ARK1668_CROSSREF.md`](../docs/SOC_ARK1668_CROSSREF.md)
and [`docs/ARKDATA_VARIANTS.md`](../docs/ARKDATA_VARIANTS.md) to back specific claims with real vendor
source instead of Ghidra-decompilation guesses.

## Source

- Repo: https://github.com/cphatt/ArkPro
- Commit: `e7437446cacc79e242d9b7a90e3724af52c33bba` ("last one")
- Original path prefix (dropped here for readability): `AVService/背光和vp调节/` ("backlight and VP
  adjustment") — a Qt-based AVService SDK (`ArkSdk.pro`) built by **ASTRI** (Hong Kong Applied Science
  and Technology Research Institute) that happens to bundle a slice of ARK1680 kernel/U-Boot source
  needed to build its display/backlight kernel modules.
- Files carry ASTRI "Proprietary and Confidential Information" copyright headers (2012, author "Jack
  Tang" for `config.c`, "Chan Man Chi" for the GPIO regs header). Kept here only as a research
  reference for identifying this project's own (Toyota/Limcet OEM) firmware — not redistributed or
  used as build input anywhere in this repo.

## What's here vs. what isn't

This is a narrow slice ASTRI shipped for one feature branch (backlight/VP adjustment), **not** a full
BSP. Missing: `clock.c` (referenced by a build-path string found in the actual Prado kernel binary —
not verifiable from this drop), the GPIO/NAND/touchscreen drivers, and critically **any OEM-customized
board file** — nothing here has `apple_encpy_ic_rst`, `customer_gpio_init`, `carback`,
`rn6752_reset`/`rn6752_irq`, `ark1680-spi`, or `ark_nec_sw_remote` (all found only in the actual Prado
kernel binary via Ghidra — see `SOC_ARK1668_CROSSREF.md` §5). Those are this OEM's (`ark0618system`)
proprietary additions layered on top of this generic ASTRI reference file.

```
kernel/
  arch/arm/mach-ark1680/config.c       Reference board-init file — ancestor of the Prado's
                                        FUN_8059f188 board bring-up routine (device names, VIC
                                        gating, platform_device roster all match)
  drivers/ark/display/                 LCD/framebuffer/OSD/TV-encoder kernel driver
    ark_display_lcd.c                    CLCD_TIMING0/1/2 register layout — confirms HSW/HBP/VSW/
                                          VBP/IVS field names & bit positions used by arkdata
  drivers/ark/audio/                   I2S/SDDAC/CS4334/FM1288 audio drivers
  drivers/ark/pwm/                     PWM driver (backlight PWM)

uboot/ark_lcd.c                        U-Boot LCD driver (2671 lines) — confirms CLKDIV1 is the
                                        "srgb_clock div factor" (SYS_LCD_CLK_CFG bits 23:19)
                                        dividing the ~393MHz system PLL to derive LCD pixel clock

userspace/                             Qt/DBus AVService reference implementation (com.arkmicro.av)
  AVService.cpp / .h                     DVR/camera capture service, generic ArkMicro DBus pattern
  ark_api.h                              arkapi_* userspace wrappers over /dev/dvr, /dev/fb0 ioctls
  display.h                              ioctl definitions incl. ARKDISP_GET_BACKCAR_STATUS —
                                          a 3rd candidate backcar-detection mechanism (kernel
                                          display driver ioctl), separate from /proc/ark_gpio
                                          (ruled out for Prado) and the MCU-UART arktool path
                                          (confirmed as what Prado actually uses)
```

Not included from the upstream repo (checked, found irrelevant to this project): `Launcher/`,
`MultimediaService/`, `Package/`, `AutoConnect/`, and the other Qt-service scaffolding — all generic
media-player/DBus-framework plumbing with no Bluetooth/CAN/MCU-adapter logic, and no
`MsnCoreApp`/`libMcuCenter`/`MCUAdapter`/`GPIOOperater`-equivalent anywhere in the upstream repo.
