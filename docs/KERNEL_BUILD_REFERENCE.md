# Limcet P305/P306 Kernel Compilation Reference

**Device:** Limcet P305 / P306 Toyota Prado Dashboard Head Unit  
**SoC:** ARKMicro ARK1668 (ARM Cortex-A7, 32-bit)  
**Kernel:** Linux 4.19.192  
**Last successful build:** 2026-07-08

---

## Table of Contents

1. [Build Environment](#1-build-environment)
2. [Quick Build Commands](#2-quick-build-commands)
3. [Output Artifacts](#3-output-artifacts)
4. [Device Tree — Limcet P305](#4-device-tree--limcet-p305)
5. [Key Kernel Config Settings](#5-key-kernel-config-settings)
6. [Source Code Fixes Required](#6-source-code-fixes-required)
7. [Known Gotchas & Driver Conflicts](#7-known-gotchas--driver-conflicts)
8. [Hardware Pin Reference](#8-hardware-pin-reference)
9. [Boot Log Analysis — Missing Config Findings](#9-boot-log-analysis--missing-config-findings)

---

## 1. Build Environment

### Source Tree Location
```
/home/osboxes/Downloads/linux-arkmicro/
├── linux/              ← Kernel source
├── u-boot/             ← Bootloader source
├── buildroot/          ← Root filesystem
├── buildroot-2021.02.2/
├── buildroot-external/
│   └── toolchain/      ← Cross-compiler lives here
├── compiled_modules/   ← Module install output (INSTALL_MOD_PATH)
├── zImage.w_dtb        ← Final boot image (zImage + DTB appended)
├── env.source          ← Build environment setup script
└── tools/
```

### Toolchain
```
Linaro GCC arm-linux-gnueabihf 7.3.1 / 7.4.1
Located: buildroot-external/toolchain/gcc-linaro-*/bin/
```

### Environment Setup
Always source this before any build command:
```bash
source env.source
```

This sets:
```bash
export CROSS_COMPILE=arm-linux-gnueabihf-
export ARCH=arm
export PATH=.../toolchain/gcc-linaro-*/bin:$PATH
```

### Prerequisites (host packages)
```bash
sudo apt install gcc make bc bison flex libssl-dev libelf-dev \
  python3 rsync u-boot-tools device-tree-compiler lzop
```

> **Note:** The kernel uses `scripts/extract-cert` which requires `libssl-dev`.
> If OpenSSL 3.x is installed, you will see deprecation warnings about
> `ENGINE_ctrl_cmd` — these are **warnings only** and do not break the build.

---

## 2. Quick Build Commands

### Full clean build
```bash
source env.source
cd linux

# Apply default config for ARK1668
make ark1668_defconfig

# Apply all required config options (see Section 5), then:
make olddefconfig

# Build kernel image
make -j$(nproc) zImage

# Build and install kernel modules
make -j$(nproc) modules
make modules_install INSTALL_MOD_PATH=../compiled_modules
```

### Build device tree blobs only
```bash
source env.source && cd linux
make dtbs
```

### Incremental rebuild (after config or source changes)
```bash
source env.source && cd linux
make -j$(nproc) zImage modules && make modules_install INSTALL_MOD_PATH=../compiled_modules
```

### Create the appended boot image (zImage + DTB)
```bash
cat arch/arm/boot/zImage arch/arm/boot/dts/ark1668_limcet_p305.dtb > ../zImage.w_dtb
```

> **Important:** The ARK1668 bootloader expects a kernel image with the DTB appended
> directly to the end of the compressed zImage. The combined file `zImage.w_dtb` is
> the correct image to flash to the boot partition.

---

## 3. Output Artifacts

| File | Path | Description |
|---|---|---|
| `zImage` | `linux/arch/arm/boot/zImage` | Compressed kernel (~4.7 MB) |
| `ark1668_limcet_p305.dtb` | `linux/arch/arm/boot/dts/ark1668_limcet_p305.dtb` | Compiled device tree (~18 KB) |
| `zImage.w_dtb` | `linux-arkmicro/zImage.w_dtb` | **Boot image** — kernel + DTB appended |
| `compiled_modules/` | `linux-arkmicro/compiled_modules/` | 152 kernel modules for rootfs |
| `System.map` | `linux/System.map` | Kernel symbol table (for debugging) |

### Key loadable modules (`.ko` files)

| Module | Path in compiled_modules | Purpose |
|---|---|---|
| `rtl8821cs.ko` | `kernel/drivers/net/wireless/realtek/rtl8821cs/` | WiFi SDIO — internal AP chip |
| `rtl8811cu.ko` | `kernel/drivers/net/wireless/realtek/rtl8811cu/` | WiFi USB dongle support |
| `rtl8192cu.ko` | `kernel/drivers/net/wireless/realtek/rtlwifi/rtl8192cu/` | WiFi older USB chipsets |
| `musb_hdrc.ko` | `kernel/drivers/usb/musb/` | USB OTG core |
| `musb_ark.ko` | `kernel/drivers/usb/musb/` | ARK1668 USB OTG glue |

---

## 4. Device Tree — Limcet P305

**DTS Source:** `linux/arch/arm/boot/dts/ark1668_limcet_p305.dts`

This file was authored based on reverse engineering of the stock Prado firmware
binaries. The reference config used was `ark1668_tyw_zksw.dts`, with all pin
assignments verified against the dumped kernel and drivers.

### Hardware Map

| Peripheral | Interface | GPIO / Detail |
|---|---|---|
| **Goodix GT911 Touchscreen** | Hardware I2C (`&i2c0`) | IRQ: GPIO 4, Reset: GPIO 80 (GPIO2\[16\]) |
| **ROHM BD37033 Audio Proc** | Bit-bang I2C bus 1 | SDA: GPIO 9, SCL: GPIO 121 (GPIO3\[25\]) |
| **RN6752 AHD Camera Decoder** ⚠️ (not ARK7116 — see note below) | Bit-bang I2C, 7-bit addr **0x2c** | Reset + reverse-detect GPIOs via `dvr_rn6752_probe`/`rn6752_reset` |
| **LCD Panel** | TTL RGB888 | 800x480, 33 MHz pixel clock |
| **LCD Power Enable** | GPIO | GPIO 81 (GPIO2\[17\]), active-high |
| **LCD Backlight PWM** | PWM | `pwm0` channel 1, 50 kHz, 20% default |
| **Reverse Camera Detect** | GPIO | GPIO 5, edge-both interrupt |
| **MCU UART** | `&uart3` | `/dev/ttyS2` — STM32 companion MCU |
| **Debug/Tool UART** | `&uart0` | `/dev/ttyS0` — console |
| **USB OTG** | `&usb0` / `&usb1` | Both enabled, NOP PHY |

> #### ⚠️ Camera decoder chip — RESOLVED: it is **RN6752**, not ARK7116 (2026-07-11)
>
> This doc, the reconstruction DTS (`dvr_ark7116@B2`, `compatible = "arkmicro,ark1668_ark7116"`,
> `reg = <0x59>`), and `HANDOFF_touch_and_bootargs_fix.md` all assumed an **ARK7116**
> camera decoder. Disassembly of the **stock 3.4.0 kernel** proves the real board uses a
> **Richwave RN6752** AHD/CVBS decoder instead — this **contradicts the ARK7116 config
> here and matches [`KERNEL.md`](KERNEL.md#reverse-camera--ahd-video-decoder)**, which
> already had it right. Evidence:
> - Active driver is **`dvr_rn6752_probe`** (+ `rn6752_init`, `rn6752_reset`,
>   `rn6752_detect_signal`, `rn6752_irq`); there is **no** `dvr_ark7116_probe`. The lone
>   `ARK7116H` string is one entry in the DVR driver's supported-decoder enum
>   (`RN6752 / RN6752M / RN6752V1 / ARK7116H`) — this board populates the RN6752 path.
> - Userspace confirms the address/bus: `libSetting` →
>   `/sys/.../i2c-gpio.1/i2c-1/**1-002c**/dvr` = 7-bit **0x2c** (RN6752's 0x58 8-bit),
>   **not** the DTS's `0x59`. Wrong chip **and** wrong address.
> - The 4.19 `linux-arkmicro` tree in this repo ships **no** camera decoder driver at all
>   (no `ark7116.c`, no `rn6752.c`), so `CONFIG_VIDEO_ARK7116` binds to nothing here.
>
> **Impact:** the reversing camera will not work on the 4.19 reconstruction as configured.
> **Fix:** forward-port the stock RN6752 driver (`dvr_rn6752_*` from the 3.4.0 tree) to
> 4.19, and change the DTS node to the RN6752 chip at **0x2c** (drop the ARK7116 node /
> `CONFIG_VIDEO_ARK7116`). The likely origin of the error: the DTS inherited the
> `&ark7116` node from the generic `ark1668.dtsi` rather than matching the real chip.
> *Not yet byte-verified:* the exact reset / reverse-detect GPIO numbers (this doc's
> earlier "reset GPIO 0 / detect GPIO 5" came from the ARK7116 assumption).

### Display Timings
```
Resolution:    800 x 480
Pixel Clock:   33 MHz
HFP / HBP:    210 / 16
HSW:           30
VFP / VBP:    22 / 12
VSW:           13
HSYNC active:  high
VSYNC active:  high
LCD wiring:    BGR
```

### Audio Configuration
```
simple-audio-card with two DAI links:
  Link 0 (ADC):  i2s_adc → sdadc
  Link 1 (DAC):  i2s_dac (master) → sddac
BD37033 I2C address: 0x40
Gain: FL/FR/RL/RR all = 65
```

### Adding the DTS to the Build System
The DTS is registered in `arch/arm/boot/dts/Makefile`:
```makefile
dtb-$(CONFIG_ARCH_ARKMICRO) += \
    ark1668_tyw_zksw.dtb \
    ark1668_limcet_p305.dtb \   # added here
    ...
```

---

## 5. Key Kernel Config Settings

These settings must be **enabled** beyond the default ARK1668 defconfig.
Use `scripts/config` to set them, then run `make olddefconfig`.

### Complete set of required config commands

```bash
cd linux

# --- Android Auto & CarPlay USB gadget stack ---
scripts/config --enable CONFIG_USB_CONFIGFS_F_EAP          # Android Auto bridge /dev/eap
scripts/config --enable CONFIG_USB_CONFIGFS_F_IAP2         # Apple CarPlay iAP2 protocol
scripts/config --enable CONFIG_USB_CONFIGFS_F_ACC           # Android Open Accessory (AOA)
scripts/config --enable CONFIG_USB_CONFIGFS_F_ADB           # Android Debug Bridge
scripts/config --enable CONFIG_USB_CONFIGFS_F_APPLE_MUX_SIM # Apple Mux simulator
scripts/config --enable CONFIG_USB_CONFIGFS_F_APPLE_PTP_SIM # Apple PTP simulator
scripts/config --enable CONFIG_USB_CONFIGFS_F_APPLE_VSC_SIM # Apple VSC simulator
scripts/config --enable CONFIG_USB_CONFIGFS_F_APPLE_HID_SIM # Apple HID simulator
scripts/config --enable CONFIG_USB_CONFIGFS_RNDIS           # Wired AA/CP (Windows)
scripts/config --enable CONFIG_USB_CONFIGFS_NCM             # Wired AA/CP (Mac/iOS)
scripts/config --enable CONFIG_USB_CONFIGFS_MASS_STORAGE    # SD card over USB

# --- Networking ---
scripts/config --enable CONFIG_INET            # TCP/IP stack (needed by WiFi drivers)
scripts/config --enable CONFIG_IP_MULTICAST    # mDNS for wireless CarPlay discovery
scripts/config --enable CONFIG_CFG80211_WEXT   # Wireless Extensions for hostapd + RTL

# --- Camera & Video (V4L2 — disable legacy equivalents!) ---
scripts/config --enable  CONFIG_VIDEO_ARK7116      # V4L2 ARK7116 AHD decoder
scripts/config --enable  CONFIG_VIDEO_ARK1668_VIN  # V4L2 Video Input Node
scripts/config --disable CONFIG_ARK7116            # DISABLE — conflicts with VIDEO_ARK7116
scripts/config --disable CONFIG_ARK1668_ITU656     # DISABLE — conflicts with VIDEO_ARK1668_VIN

# --- Audio ---
scripts/config --enable CONFIG_SND_USB_AUDIO   # USB audio for Android Auto sessions

# --- Bluetooth ---
scripts/config --enable CONFIG_BT_HCIUART_BCM  # Feasycom FSC-BT8251 (Broadcom chipset)

# --- Display ---
scripts/config --enable CONFIG_BACKLIGHT_PWM   # PWM backlight (DTS uses pwm0)

# --- I2C ---
# The P305 DTS #includes ark1668.dtsi, whose i2c0 (i2c@e4300000) is a
# "snps,designware-i2c" controller — so the Goodix touchscreen's hardware bus
# needs the DesignWare driver, NOT CONFIG_I2C_ARK (that matches ark1668e.dtsi's
# "arkmicro,ark-i2c", a different SoC file). See §7 "Touchscreen I2C bus" and
# docs/HANDOFF_touch_and_bootargs_fix.md.
scripts/config --enable CONFIG_I2C_DESIGNWARE_PLATFORM  # Hardware I2C (i2c0) for Goodix GT911
scripts/config --enable CONFIG_TOUCHSCREEN_GOODIX       # GT911 touch driver

# --- WiFi: must be modules (=m) not built-in (=y) ---
scripts/config --module CONFIG_RTL8821CS
scripts/config --module CONFIG_RTL8821CU
scripts/config --module CONFIG_RTL8192CU
scripts/config --module CONFIG_RTLWIFI
scripts/config --module CONFIG_RTLWIFI_USB
scripts/config --module CONFIG_RTL8XXXU

# Apply changes (non-interactive)
make olddefconfig
```

---

## 6. Source Code Fixes Required

These patches must be applied to the ARK1668 kernel source before it will compile
successfully with a modern GCC (12.x) cross-compiler.

### Fix 1 — `sound/soc/arkmicro/BD37033.c`
**Problem:** `bd37033_remove` is a `void` function but has `return 0;`

```diff
 static void bd37033_remove(struct snd_soc_component *component)
 {
-    return 0;
 }
```

**Reason:** The upstream kernel changed `snd_soc_component_driver.remove` from
`int (*remove)()` to `void (*remove)()`. The ARK driver was not updated.

---

### Fix 2 — `drivers/soc/arkmicro/itu656/ark1668_vin.c` (line ~1990)
**Problem:** `vin_driver_init` is `void` but returns `-ENOMEM`

```diff
 static void vin_driver_init(struct device *dev)
 {
     if (g_ark168_vin->dvr_dev == NULL) {
-        return -ENOMEM;
+        return;
     }
```

---

### Fix 3 — `drivers/soc/arkmicro/itu656/ark1668_vin.c` (line ~2069)
**Problem:** Incorrect nested struct access for `fwnode`

```diff
-    subdev_entity->asd->match.fwnode.fwnode = of_fwnode_handle(rem);
+    subdev_entity->asd->match.fwnode = of_fwnode_handle(rem);
```

**Reason:** In kernel 4.19, `v4l2_async_subdev.match.fwnode` is directly a
`struct fwnode_handle *`. An older kernel version had it nested differently.

---

### Fix 4 — `drivers/soc/arkmicro/itu656/ark1668_vin.c` (line ~2120)
**Problem:** Old `v4l2_async_notifier` API — callbacks were direct struct members

```diff
+    static const struct v4l2_async_notifier_operations vin_async_ops = {
+        .bound    = vin_async_bound,
+        .unbind   = vin_async_unbind,
+        .complete = vin_async_complete,
+    };
     list_for_each_entry(subdev_entity, ...) {
         subdev_entity->notifier.subdevs = &subdev_entity->asd;
         subdev_entity->notifier.num_subdevs = 1;
-        subdev_entity->notifier.bound    = vin_async_bound;
-        subdev_entity->notifier.unbind   = vin_async_unbind;
-        subdev_entity->notifier.complete = vin_async_complete;
+        subdev_entity->notifier.ops = &vin_async_ops;
```

**Reason:** Kernel 4.19 moved async notifier callbacks from direct struct members
into a separate `v4l2_async_notifier_operations` struct pointed to by `.ops`.

---

### Fix 5 — `drivers/soc/arkmicro/hx170dec/hx170dec.c` (line ~625)
**Problem:** `vdec_probe` crashes with a NULL pointer dereference if the animation memory resource (index 1) is missing from the device tree.

```diff
 //MFC jpeg decode
 	animres = platform_get_resource(pdev, IORESOURCE_MEM, 1);
-	if (IS_ERR(animres)) {
-		return PTR_ERR(animres);
-	}
-
 	p->context.dev = p->dev;
 	p->context.anmation_stats = 0;
-	p->context.animation_data_phyaddr = animres->start;
-	p->context.animation_data_size = resource_size(animres);
-	p->context.animation_data_virtaddr =
-	    (unsigned int)ioremap(p->context.animation_data_phyaddr, resource_size(animres));
-	if (p->context.animation_data_virtaddr) {
...
-	} else
-		p->context.animation_end = true;
+	if (animres) {
+		p->context.animation_data_phyaddr = animres->start;
+		p->context.animation_data_size = resource_size(animres);
+		p->context.animation_data_virtaddr =
+		    (unsigned int)ioremap(p->context.animation_data_phyaddr, resource_size(animres));
+		if (p->context.animation_data_virtaddr) {
...
+		} else {
+			p->context.animation_end = true;
+		}
+	} else {
+		p->context.animation_data_phyaddr = 0;
+		p->context.animation_data_size = 0;
+		p->context.animation_data_virtaddr = 0;
+		p->context.animation_end = true;
+	}
```

**Reason:** `platform_get_resource` returns `NULL` (not an `IS_ERR` pointer) if the requested resource index does not exist. Checking it with `IS_ERR()` fails to catch the missing resource, causing a kernel crash when `animres` is dereferenced.

---

## 7. Known Gotchas & Driver Conflicts

### RTL WiFi drivers must be modules, not built-in
`rtl8821cs` and `rtl8811cu` share dozens of global function names such as
`SetTxPower`, `rtw_mp_mode_check`, `HwRateToMPTRate`. If both are compiled as
built-in (`=y`), the linker fails with "multiple definition" errors.
**Always use `=m`.**

### Legacy ITU656 and new V4L2 camera drivers are mutually exclusive

| Old (Legacy) | New (V4L2) | Action |
|---|---|---|
| `CONFIG_ARK7116` → `ark7116_ark169.o` | `CONFIG_VIDEO_ARK7116` → `drivers/media/i2c/ark7116.c` | Disable old |
| `CONFIG_ARK1668_ITU656` → `ark1668_itu656.o` | `CONFIG_VIDEO_ARK1668_VIN` → `ark1668_vin.c` | Disable old |

Enabling both simultaneously causes linker symbol conflicts (e.g.
`AMT_PadMuxStaticPara`, `ark_itu656_stop`, `dvr_set_sys_clk`).

### `CONFIG_INET` must be enabled before WiFi modules will load
`rtl8821cs` and `rtl8811cu` reference `register_inetaddr_notifier` which lives
in the TCP/IP stack. Without `CONFIG_INET=y`, modpost fails at build time.

### Always use `make olddefconfig` not `make oldconfig`
After using `scripts/config` to change settings, `make oldconfig` is interactive
and will block waiting for user input on newly exposed options. Use
`make olddefconfig` which automatically accepts defaults for all new symbols.

### Touchscreen I2C bus — must be hardware `&i2c0`, not the camera's bit-bang bus
**Symptom (all `docs/new kernel bootlog*.txt`):** `Goodix-TS …-005d: I2C
communication failure: -6` (or `-121`), no touch input, and no hardware I2C
controller in the log. **Cause:** the P305 DTS put the `gt911` node on the
bit-banged `i2c-gpio-0` bus (GPIO 3 SDA / GPIO 2 SCL) — which is the **ARK7116
camera** bus (see §8) — while the GT911 is physically wired to the hardware I2C
controller `&i2c0` (`i2c@e4300000`). The kernel drove touch out the camera's
pins, so the panel never answered. **Fix:** enable
`CONFIG_I2C_DESIGNWARE_PLATFORM` (§5) and move the `gt911` node onto `&i2c0`,
leaving the ARK7116 alone on `i2c-gpio-0`. Full before/after DTS diff, build
commands, and boot-log PASS/FAIL criteria are in
[`HANDOFF_touch_and_bootargs_fix.md`](HANDOFF_touch_and_bootargs_fix.md) (Fix A);
cross-log analysis is in [`boot_experiment_log.md`](boot_experiment_log.md).

---

## 8. Hardware Pin Reference

### GPIO Reference

| GPIO # | GPIO Bank/Bit | Function |
|---|---|---|
| GPIO 0 | GPIO0\[0\] | ARK7116 AHD decoder reset (active-low) |
| GPIO 2 | GPIO0\[2\] | I2C bus 0 SCL (bit-bang, for ARK7116) |
| GPIO 3 | GPIO0\[3\] | I2C bus 0 SDA (bit-bang, for ARK7116) |
| GPIO 4 | GPIO0\[4\] | Goodix GT911 touch IRQ |
| GPIO 5 | GPIO0\[5\] | Reverse camera detection (edge interrupt) |
| GPIO 9 | GPIO0\[9\] | I2C bus 1 SDA (bit-bang, for BD37033) |
| GPIO 80 | GPIO2\[16\] | Goodix GT911 touch reset |
| GPIO 81 | GPIO2\[17\] | LCD panel power enable |
| GPIO 91 | GPIO2\[27\] | Bluetooth module enable (FSC-BT8251) |
| GPIO 95 | GPIO2\[31\] | Apple MFi chip reset (`apple_encpy_ic_rst`) |
| GPIO 121 | GPIO3\[25\] | I2C bus 1 SCL (bit-bang, for BD37033) |

### UART Reference

| UART | Device Node | Purpose |
|---|---|---|
| UART0 | `/dev/ttyS0` | Console / debug / firmware update tool |
| UART1 | Disabled | — |
| UART3 | `/dev/ttyS2` | STM32 companion MCU (vehicle CAN bus bridge) |

### Bluetooth Module
- **Module:** Feasycom FSC-BT8251 (Broadcom BCM chipset, firmware V5.5.0)
- **Interface:** UART AT command protocol via `/usr/bin/blueware`
- **Enable GPIO:** GPIO 91
- **MAC address:** `DC:0D:30:14:FC:9F` (observed in boot log)
- **Protocol:** BlueWare AT commands (not raw HCI over UART)

### Apple MFi Authentication Chip
- **Reset GPIO:** GPIO 95
- **Interface:** I2C (address TBD from firmware RE)

---

## 9. Boot Log Analysis — Missing Config Findings

Cross-referencing `boot log.txt` (stock firmware boot) against the kernel `.config`
revealed settings needed for full functionality. All items below were added to
the config and verified to build successfully.

| Setting | Evidence from Boot Log |
|---|---|
| `CONFIG_IP_MULTICAST=y` | `mDNSResponder` starts for wireless CarPlay discovery |
| `CONFIG_CFG80211_WEXT=y` | `hostapd` uses WEXT ioctls for WiFi AP mode |
| `CONFIG_VIDEO_ARK7116=y` | `libCarReversing.so` loaded + `arkapi_open_dvr()` called |
| `CONFIG_VIDEO_ARK1668_VIN=y` | `VideoDecoder::video_init` pipeline in Android Auto |
| `CONFIG_USB_CONFIGFS_RNDIS=y` | Wired Android Auto (Windows RNDIS ethernet) |
| `CONFIG_USB_CONFIGFS_NCM=y` | Wired CarPlay (Mac/iOS CDC-NCM ethernet) |
| `CONFIG_BACKLIGHT_PWM=y` | *"Open backlight pwm: true"* at 9.9s |
| `CONFIG_I2C_DESIGNWARE_PLATFORM=y` | Hardware I2C controller (`i2c@e4300000`, `snps,designware-i2c`) for Goodix GT911 on `&i2c0` — **not** `CONFIG_I2C_ARK`, see §7 |
| `CONFIG_USB_CONFIGFS_MASS_STORAGE=y` | SD card partitions mounted, exposed over USB |
| `CONFIG_SND_USB_AUDIO=y` | Three ALSA PCM streams opened: 48kHz stereo + 2x 16kHz mono |
| `CONFIG_BT_HCIUART_BCM=y` | Feasycom FSC-BT8251 is a Broadcom BCM-family chipset |

### Observed Android Auto Connect Sequence (from boot log)
```
~23s  BT phone detected (Pixel 9 Pro, 04:00:6E:AF:29:C4)
      libMsnCarAuto.so loaded → CarAutoWindow shown
      com.arkmicro.auto DBus service registered
      BtRfcommController RFCOMM link established
      Device serial: 56171FDAP001TE
      WiFi AP: ssid=carplay_fc9f, pass=88888888, wlan0=192.168.43.1
      Phone DHCP: 192.168.43.20
~30s  Wireless transport connected (status=9)
      SSL: TLSv1.2 / ECDHE-RSA-AES128-GCM-SHA256
      Android Auto session active (status=4)
      Video: 800x480, H.264 decode pipeline open
      Audio: plug:softvol1 (16kHz mono), softvol2 (48kHz stereo)
~45s  Session disconnected (LinuxController ping timeout 5s)
```

---

## 10. U-Boot Compilation Reference

U-Boot can be built directly from the `u-boot/` subdirectory inside `linux-arkmicro`. The build targets the `ark1668_tyw_zksw` board, matching the Prado's SoC family and LCD screen timings.

### Quick Build Commands
```bash
source env.source
cd u-boot

# Clean previous build
make mrproper

# Apply Prado defconfig
make ark1668_tyw_zksw_defconfig

# Compile
make -j$(nproc)
```

### Output Binaries
- **Main U-Boot Image:** `u-boot.bin` (~567 KB)
- **First Stage Bootloader (SPL):** `spl/u-boot-spl.bin` (~17 KB)

### Required Compilation Fixes

#### Fix 1: Compiler `-march` Mismatch (early cc-option checks)
**File:** `u-boot/arch/arm/Makefile` (lines 17-18)
- **Problem:** Debian's `arm-linux-gnueabihf-gcc` defaults to HardFP, which causes early `cc-option` compiler checks to fail without an FPU, falling back to `-march=armv5` (which is unsupported by the compiler).
- **Fix:** Set the V7A architecture compiler flag directly without the cc-option check:
```diff
-arch-$(CONFIG_CPU_V7A)        =$(call cc-option, -march=armv7-a, \
-                                $(call cc-option, -march=armv7, -march=armv5))
+arch-$(CONFIG_CPU_V7A)        =-march=armv7-a
```

#### Fix 2: Linker Error on `burn_data_to_partition`
- **Problem:** Enabling the custom flashing command compiles `cmd/update_cmd.c`, which makes calls to `burn_data_to_partition()`. This function is only defined in reference boards (`ark1668e_devb`), causing a linker error on our custom board.
- **Fix:** Disable `CONFIG_CMD_FLASH_UPDATE` in the defconfig:
```diff
# configs/ark1668_tyw_zksw_defconfig
+# CONFIG_CMD_FLASH_UPDATE is not set
```

### Key Configuration Settings (Prado NAND-Aligned)

These parameters are configured in the board definition files to match the stock Holden firmware environment:

#### 1. NAND Environment Offsets (`u-boot/include/configs/ark1668_tyw_zksw.h`)
- **CONFIG_ENV_OFFSET:** `0x120000` (1152 KB) — Points directly to the start of the `U-boot-Env` partition (aligned to the end of `bootstrap` + `U-boot` + `U-boot_back`).
- **CONFIG_ENV_SIZE:** `0x40000` (256 KB) — Matches the size of the Prado environment partition (2 erase blocks).

#### 2. Default MTD Partitions (`u-boot/configs/ark1668_tyw_zksw_defconfig`)
- **CONFIG_MTDIDS_DEFAULT:** `"nand0=ark1680-nand"`
- **CONFIG_MTDPARTS_DEFAULT:**
  `"mtdparts=ark1680-nand:128k(S-Loader),512k(U-boot),512k(U-boot_back),256K(U-boot-Env),256K(arkdata),4m(kernel),106m(rootfs),6m(userdata),512K(bootlogo),3m(bootanimation),3m(reversingtrack),256K(Unicode)"`

#### 3. Custom Commands & Delay (`u-boot/configs/ark1668_tyw_zksw_defconfig`)
- **CONFIG_CMD_SOURCE=y** — Enabled to allow executing SD card boot scripts (`s`) directly.
- **CONFIG_BOOTDELAY=9** — Sets a 9-second delay for console interruption during boot.

#### 4. ATAG Boot Command (`u-boot/include/configs/ark1668_tyw_zksw.h`)
The Prado kernel uses legacy ATAGs (no device tree). The bootcommand is configured to read the kernel and boot without an FDT parameter:
```c
"nandargs=setenv bootargs console=ttyS0,115200n8 mem=180M earlyprintk=serial ubi.mtd=6 root=ubi0:rootfs rootfstype=ubifs rootwait ro ${mtdparts} screen=${screen}\0"
"nandboot=echo Booting from nand ...; run nandargs; nand read ${kerneladdr} kernel; bootz ${kerneladdr}\0"
```

---

*Document generated: 2026-07-08*  
*Kernel: linux-arkmicro/linux (4.19.192)*  
*U-Boot: linux-arkmicro/u-boot (2018.07)*  
*Device tree: ark1668_limcet_p305.dts*  
*Project: prado-firmware-reconstruction*
