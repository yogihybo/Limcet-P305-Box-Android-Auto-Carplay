# Kernel Analysis — `mtd5_kernel/zImage`

Source: `Prado firmware reconstructed/mtd5_kernel/zImage`  
Analysed by: binwalk + manual LZO decompression of vmlinux payload.

---

## File Identification

| Field | Value |
|---|---|
| Path | `Prado firmware reconstructed/mtd5_kernel/zImage` |
| Type | Linux kernel ARM boot executable zImage (little-endian) |
| Size (on disk) | 3,255,536 bytes (3.10 MB) |
| MD5 | `78782daea22d5e22ad90c6e660da75e1` |
| NAND partition | mtd5 — `kernel`, flash offset `0x1A0000`, slot size 4 MB |
| `kernel_size` (uboot-env) | `0x305130` = 3,166,512 bytes — ~89 KB less than repo file |

---

## Kernel Build Information

| Field | Value |
|---|---|
| **Kernel version** | Linux 3.4.0 |
| **Build number** | `#383` |
| **Build timestamp** | Tue Dec 5 10:50:38 CST 2023 |
| **LZO stream mtime** | 2023-12-04 21:50:40 UTC |
| **Compiler** | GCC 4.9.4 (Buildroot 2018.08-rc1-00026-gaeef2a9) |
| **Build host** | `root@build-server` |
| **Architecture** | ARMv7 (little-endian) |
| **Target SoC** | **ARK1680** (`mtdids=nand0=ark1680-nand`) |

Full version string extracted from vmlinux:
```
Linux version 3.4.0 (root@build-server) (gcc version 4.9.4 (Buildroot 2018.08-rc1-00026-gaeef2a9) ) #383 Tue Dec 5 10:50:38 CST 2023
```

---

## zImage Structure (binwalk annotated)

| Offset | Hex | Binwalk hit | Reality |
|---|---|---|---|
| `0x000000` | `e1a00000`… | ARM zImage header | **Real** — 6.3 KB ARM decompressor stub. Prints `"Uncompressing Linux... done, booting the kernel."`. Contains `ARMv7 Processor` string at `0xA93D`. |
| `0x001914` | `89 4c 5a...` | LZO compressed data (v0.000) | **Artifact** — malformed 340-byte LZO fragment with invalid version 0.000. Padding/stub artefact. |
| `0x001A68` | `89 4c 5a 4f...` | LZO compressed data (v1.030, LZO1X-999) | **Real — main kernel payload.** Valid LZO1X-999 stream. |
| `0x04D21A` | `30 82...` | X.509 DER certificate (x509 v3) | **False positive** — offset falls inside the LZO compressed data stream. Compressed binary data matching the DER pattern. |
| `0x20DC85` | — | mcrypt 2.5 encrypted data | **False positive** — also inside the LZO stream. No real encryption. |
| `0x31ACB2` | `00 00 00 00` | (end sentinel) | LZO stream terminator + 62-byte ARM load-address metadata tail. |

### LZO Payload Detail

| Property | Value |
|---|---|
| Compression | LZO1X-999 (maximum compression, level 9) |
| Integrity | Per-block Adler32 checksums |
| Block count | 25 blocks × 256 KB uncompressed each |
| Total uncompressed size | **6,384,420 bytes (6.09 MB vmlinux)** |
| Compressed size | ~3,172 KB |
| Compression ratio | ~1.96:1 |

---

## Compiled-in Drivers — Complete ARK1680 BSP Inventory

Everything is **monolithic** (no loadable modules). `kallsyms` is present; no module signing enforced.

### Serial / UART

| Driver | Notes |
|---|---|
| `ark1680-uart` | 4 standard UARTs (`uart0`–`uart3`); UART0 = boot console (`ttyS0,115200n8`) |
| `ark1680-hsuart` | 2 high-speed UARTs (`hsuart0`, `hsuart1`); used for MCU arktool protocol |
| `serial8250` | Also compiled in as fallback |

Boot console enabled with: `<6>enable uart0 rx to open console.`

### Display / Video Pipeline

The kernel has a multi-stage hardware video pipeline all built in:

| Driver | Function |
|---|---|
| `ark_display` | Top-level display controller, multi-layer compositor |
| `ark_fb` | Linux framebuffer (`/dev/fb0`) — GUI layer |
| `ark_prescal` | Pre-scaler — hardware resize engine for video layers |
| `ark_deinterlace` | Hardware de-interlacer for interlaced camera input |
| `ark_itu656` | ITU-R BT.656 parallel video input decoder |
| `ark_carback` | Reverse camera + parking track overlay driver |
| `ark_pwm` | Backlight PWM controller (PWM0 and PWM1 pads) |

**LCD panel types supported:**

| Constant | Description |
|---|---|
| `ARKDISP_LCD_PANEL_PARALLEL_16BIT` | Direct 16-bit parallel RGB |
| `ARKDISP_LCD_PANEL_PARALLEL_18BIT` | Direct 18-bit parallel RGB |
| `ARKDISP_LCD_PANEL_PARALLEL_24BIT` | Direct 24-bit parallel RGB |
| `ARKDISP_LCD_PANEL_CPU_SCREEN` | CPU-interface (MCU-style) LCD |
| `ARKDISP_LCD_PANEL_SRGB` | Serial-RGB |

**TV encoder / video output modes:**

| Mode | Resolutions |
|---|---|
| CVBS | NTSC, PAL |
| ITU-656 | NTSC, PAL |
| YPbPr component | 480i/60, 576i/50, 480p/60, 576p/50, 720p/50, 720p/60, 1080i/50, 1080i/60, 1080p/50, 1080p/60 |
| VGA | 640×480@60, 800×600@60, 1024×768@60, 1280×720@60, 1280×960@85, 1280×1024@60, 1280×1024@75, 1900×1200@60 |

### Reverse Camera / AHD Video Decoder

- **RN6752** (Richwave Technology) — AHD (Analog HD) video decoder IC
- Detected variants at runtime: `RN6752`, `RN6752M`, `RN6752V1`
- Connected via **ITU-656** bus; kernel probes for signal via `rn6752_detect_signal()`
- DVR restart/mode switching via `dvr_restart` callback
- Signal routed through: ITU-656 input → prescaler → de-interlacer → display compositor

**Carback kernel driver ioctls (`/dev/ark_carback`):**

| IOCTL | Function |
|---|---|
| `CARBACK_IOCTL_GET_STATUS` | Is reverse signal active? |
| `CARBACK_IOCTL_DETECT_SIGNAL` | Poll for signal presence |
| `CARBACK_IOCTL_GET_HASTRACK` | Does device have a reversing track overlay? |
| `CARBACK_IOCTL_SET_STRACKID` | Set active steering track ID |
| `CARBACK_IOCTL_STRACK_GET_PARAM` | Read track geometry parameters |
| `CARBACK_IOCTL_STRACK_SET_PARAM` | Write track geometry parameters |
| `CARBACK_IOCTL_STRACK_GET_FILETYPE` | Get track overlay file type |
| `CARBACK_IOCTL_STRACK_SETTING` | Apply track settings |
| `CARBACK_IOCTL_MRADAR_SET_ID` | Assign multi-radar (PDC sensor) channel ID |

Multi-radar (PDC/parking sensor) support is compiled into the carback driver — sensor IDs assigned per-channel via `CARBACK_IOCTL_MRADAR_SET_ID` / `set_disp_mradar_id`.

### Audio

| Driver | IC / Function |
|---|---|
| `ark_i2s` | I²S audio bus interface (2 instances: `.0` and `.1`) |
| `ark_sddac` | Internal Sigma-Delta DAC; params: `input_channel`, `input_gain` |
| `ark_cs4334_dev` | CS4334 (Cirrus Logic) external I²S audio DAC |
| `bd37033_drv` | **BD37033** (ROHM) — audio processor / tone controller with LPF and mixer |
| `ark_audio_mute` | Software audio mute function |
| ALSA ASoC | Version 1.0.25 |

### MCU UART Protocol (`arktool`)

The kernel has a custom binary serial framing protocol for communicating with a companion MCU over HS-UART:

- Function: `DealCommData()` — parses `CUSTOMER_ARK_FLAG`-prefixed packets with checksum validation
- Commands carried: animation start ACK, backcar enable/disable, MCU init status, display register init/deinit
- Errors logged: `CUSTOMER_ARK_FLAG Error!`, `CheckSum Error! iCommandId=%0x length=%d`
- MCU init failure: `uart%d init mcu failed.`
- Enable/disable via `/proc`: `0:disable arktool, 1:enable arktool`

### USB

| Driver | Notes |
|---|---|
| `musb-ark1680` | Mentor Graphics USB OTG controller (ARK1680 custom variant) |
| `ipheth` | Apple iPhone USB Ethernet tethering driver (compiled in) |
| USB Mass Storage | Host-side mass storage |
| File-backed Storage Gadget | Device-side gadget |
| Prolific Storage Gadget | Device-side gadget |
| `nop_usb_xceiv` | USB PHY stub (no external HSIC/UTMI PHY chip) |

### JPEG Hardware Decoder

- `ark_jpeg` — hardware JPEG decode engine, character device `/dev/ark_jpeg`
- Used for: boot logo decode, boot animation frame decode, bootanimation playback
- Error conditions: `jpeg decode error!`, `JpegFileSeek timeout`, `Wait jpeg int timeout`

### Flash / Filesystems

| Filesystem | Use |
|---|---|
| **UBIFS** | Root filesystem on mtd6 (`ubi0:rootfs`) — primary |
| **JFFS2** | Compiled in (legacy / other partitions) |
| **YAFFS2** | Compiled in |
| **ext4** | USB drives / SD card |
| **FAT/VFAT** | USB drives / SD card |
| **NTFS** | USB drives (read-only) |
| **FUSE** | Compiled in for userspace filesystems |

UBI volume table corruption is handled gracefully: `UBI warning: volume table copy #1/#2 is corrupted`.

### Other Peripherals

| Driver | Notes |
|---|---|
| `ark_gpio` | GPIO controller; `/proc/ark_gpio` debug interface |
| `ark_dw_dmac` | DesignWare DMA controller (Arkmicro variant); N channels logged at boot |
| `ark1680-spi` + `ark_ecspi` | SPI bus controller + eCSPI clock divider |
| `ark1680-rtc` | Real-time clock |
| `ark_wdt` (ArkMicro Watchdog) | Hardware watchdog; heartbeat via `arkwdt.heartbeat` param |
| `ark1680-ts` | ARK1680 touchscreen controller |
| ILI210x | ILI210x capacitive touchscreen controller (also compiled in) |
| L2x0 cache | ARM L2 cache controller (PL310 / L2x0 series) — base address logged at boot |
| `ark_sys_pad_config_gpio_mode` | Pad multiplexer / pinmux configuration |
| `ark1680-nand` | NAND flash controller (MTD layer) |

### Interrupt Controller

- ARK1680 VIC (Vectored Interrupt Controller) — dual bank: `VICL` + `VICH`
- Boot message: `ARK interrupt controller virtual address VICL %x VICH %x`

### Media Tuners (optional hardware variants)

These drivers are compiled in to support TV-tuner accessory variants:

| Driver | IC | Standard |
|---|---|---|
| `xc5000` | Xceive XC5000 | Multi-standard TV/FM |
| `xc4000` | Xceive XC4000 | Multi-standard |
| `tda18271` | NXP TDA18271 | RF tuner with tracking filter |

### Networking / Wireless

- IPv4 + IPv6 full stack
- `mac80211` + `cfg80211` + `nl80211` + `rfkill` — compiled in for optional WiFi module (SDIO or USB)
- No wired Ethernet driver (automotive head unit, no LAN port)
- `ipheth` for iPhone USB tethering

---

## Build Workspace Structure

```
/workspace/ark0618system/kernels/linux-3.4/
├── arch/arm/mach-ark1680/
│   └── clock.c                  ← PLL / clock tree
├── drivers/ark/
│   ├── dma/ark_dw_dmac.c        ← DMA controller
│   ├── serial/ark_uart.c        ← standard UART
│   ├── serial/ark_hsuart.c      ← high-speed UART
│   └── spi/spi-ark.c            ← SPI controller
├── drivers/media/               ← xc5000, tda18271, gspca, uvcvideo
├── drivers/mmc/                 ← MMC/SDIO stack
├── drivers/mtd/                 ← NAND, UBI, MTD
├── drivers/usb/                 ← musb, ipheth, gadget
├── fs/{ubifs,jffs2,yaffs2,ext4,fat,ntfs,fuse}
├── net/{mac80211,cfg80211,rfkill,ipv4,ipv6}
└── sound/soc/                   ← ASoC ark_i2s, ark_sddac
```

---

## Key Findings

1. **ARK1680 SoC** — Chinese automotive-grade ARMv7 processor. No public datasheet available. All BSP drivers are vendor-proprietary, built into the kernel monolithically.

2. **Linux 3.4.0 build #383** — kernel version dates from 2012 but this build is from December 2023, indicating 11+ years of accumulated ARK1680 vendor patches against the upstream 3.4 LTS base.

3. **No kernel module signing** — no `module.sig`, `verify_module`, or PKCS#7 signature verification references found. Kernel accepts unsigned modules (though none are loaded — everything is built-in).

4. **No loadable modules** — the entire driver set is compiled monolithically. `kallsyms` is present for symbol resolution.

5. **RN6752 AHD decoder** is the reverse camera interface IC — probed via I²C, streams ITU-656 video to the hardware prescaler → de-interlacer → display compositor chain.

6. **BD37033** (ROHM) is the audio DSP/tone processor; **CS4334** (Cirrus Logic) is an external I²S DAC. The internal `ark_sddac` sigma-delta DAC is also available.

7. **MCU bridge via HS-UART** — `arktool` binary protocol handles boot animation synchronisation, backcar signal enable/disable, and MCU health monitoring. Protocol uses `CUSTOMER_ARK_FLAG` framing with checksum.

8. **Multi-radar (PDC) compiled in** — parking distance sensor IDs are managed kernel-side through the carback ioctl interface.

9. **iPhone USB tethering (`ipheth`)** compiled in — likely for optional CarPlay or internet-sharing scenarios.

10. **TV tuner drivers compiled in** (XC5000, TDA18271) — these are optional hardware add-ons; the drivers are present in all builds regardless of whether the hardware is fitted.

11. **Size discrepancy** — `uboot-env` records `kernel_size=0x305130` (3,166,512 bytes) but the repo file is 3,255,536 bytes (~89 KB larger). The env variable was not updated after a kernel rebuild, or the repo carries a newer build than what was originally flashed.
