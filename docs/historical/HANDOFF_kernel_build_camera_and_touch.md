# Handoff — 4.19 Kernel Build: Camera (RN6752) & Touch (I²C bus) Fixes

**Audience:** whoever compiles the reconstructed **Linux 4.19.192** kernel
(`linux-arkmicro` BSP; config = `Limcet Hardware/kernel_dot_config`).
**Date:** 2026-07-11
**Status:** two hardware-config bugs, both **verified against the stock 3.4.0
firmware** by disassembling the dumped kernel. Fixes are **config + DTS only —
no driver code needs to be written.**

These findings come from analysing the *working* stock unit (kernel 3.4.0,
`Prado firmware dump/mtd5_kernel/extracted/vmlinux` + reconstructed
`System.map`/`vmlinux.elf`). The stock kernel is the ground truth for what the
hardware actually is.

---

## TL;DR — what to change

| # | Area | Change | Type |
|---|------|--------|------|
| 1 | **Camera** | Enable `CONFIG_VIDEO_RN6752`, disable `CONFIG_VIDEO_ARK7116`; retarget the DTS camera node to **RN6752 @ 7-bit `0x2c`** | config + DTS |
| 2 | **Touch** | Move `gt911@5d` off the bit-bang `i2c-gpio-0` onto the **hardware `&i2c0`**; enable the DesignWare I²C controller driver | config + DTS |

Neither requires new driver source — the RN6752 driver already exists in the BSP
Kconfig, and the DesignWare I²C driver ships with mainline.

---

## Verified hardware facts (from the stock 3.4.0 kernel)

| Peripheral | Chip / value | Bus | Address | Source |
|---|---|---|---|---|
| **Touchscreen** | Goodix **GT911** | **hardware `i2c0`** (bus 0) | 7-bit **0x5d** | `ark1680_machine_init` registers `Goodix-TS`/`0x5d` via `ark1680_add_device_i2c` (bus 0 = HW controller) |
| Touch INT | **GPIO 4**, falling-edge | — | — | `gt9xx.ko` disasm (`gpio_to_irq(4)`, IRQ flag 2) |
| Touch RST | **GPIO 80** (`gpio2[16]`) | — | — | `gt9xx.ko` disasm |
| **Camera decoder** | Richwave **RN6752** (AHD/CVBS) | bit-bang | 7-bit **0x2c** (8-bit 0x58) | `dvr_rn6752_probe`/`rn6752_init`/`rn6752_reset`; userspace `libSetting` → `.../i2c-gpio.1/i2c-1/1-002c/dvr` |
| Audio amp | ROHM BD37033 | bit-bang | 0x40/0x41 | `ark1680_add_device_audio` → `drv_bd37033` |

**Key point:** on the stock unit only the **audio amp and camera** are on
bit-banged buses; **touch is on the hardware controller.** The 4.19 DTS gets both
the camera chip *and* the touch bus wrong.

---

## Fix 1 — Camera: RN6752, not ARK7116

### Why
The reconstruction assumes an **ARK7116** decoder; the board actually has a
**Richwave RN6752**. Full evidence + origin analysis:
`docs/KERNEL_REFERENCE.md` → "⚠️ Camera decoder chip" callout. Also note
`docs/KERNEL_REFERENCE.md` already identified RN6752 correctly.

The current 4.19 config (`Limcet Hardware/kernel_dot_config`) has it backwards:
```
CONFIG_VIDEO_ARK7116=y              # wrong chip, ENABLED
# CONFIG_VIDEO_RN6752 is not set    # right driver, AVAILABLE but disabled
CONFIG_VIDEO_ARK1668_VIN=y          # VIN capture — keep
CONFIG_ARK_CARBACK=y                # reverse-gear detect — keep
```
`# CONFIG_VIDEO_RN6752 is not set` means the BSP Kconfig **defines** that driver
(`drivers/media/i2c/rn6752.c` exists in the full ArkMicro tree). It is simply not
enabled — so this is a **config swap, not a port**. (The `linux-arkmicro Reference/`
copy in this repo is trimmed to `arch/` + `include/`, which is why the `.c` isn't
visible here.)

### Config change
```sh
scripts/config --enable  CONFIG_VIDEO_RN6752       # correct AHD decoder
scripts/config --disable CONFIG_VIDEO_ARK7116      # wrong chip
scripts/config --enable  CONFIG_VIDEO_ARK1668_VIN  # keep (VIN capture)
scripts/config --enable  CONFIG_ARK_CARBACK        # keep (reverse detect)
```

### DTS change
The current node (`Limcet Hardware/ark1668-limcet-prado.dts`) is:
```dts
dvr_ark7116: dvr_ark7116@B2 {
    compatible = "arkmicro,ark1668_ark7116";
    reset-gpio = <&gpio0 0 0>;   // UNVERIFIED — ARK7116 guess
    reg = <0x59>;                // WRONG address
    carback-config = <1>;
};
```
Retarget to the RN6752 at 7-bit `0x2c` (use the compatible string the BSP's
`rn6752.c` matches — check its `of_device_id`/`i2c_device_id` table):
```dts
dvr_rn6752: dvr_rn6752@2c {
    compatible = "arkmicro,rn6752";   // confirm exact string in rn6752.c
    reg = <0x2c>;
    reset-gpio  = <&gpio? ? 0>;       // see GPIO caveat below
    // reverse-detect / carback GPIO as the driver's binding expects
    carback-config = <1>;
};
```

### ⚠️ GPIO caveat (do not copy the old numbers)
The RN6752 driver reads its **reset** and **reverse-detect** GPIOs from **board
platform data**, not hardcoded constants (disasm: reset = `ctx+0x8`, detect =
`ctx+0x4`, detect line uses **20 µs debounce + falling-edge threaded IRQ**). Those
pin numbers live in a runtime `.bss` struct and are **not** in the static kernel
image, so they could not be extracted. The old **"reset GPIO 0 / detect GPIO 5"**
values in the docs/DTS were **ARK7116 guesses — do not trust them.** Get the real
reset + reverse-detect pins from the board schematic or by probing the working
stock unit (e.g. `cat /sys/kernel/debug/gpio` while toggling reverse gear).

---

## Fix 2 — Touch: hardware `&i2c0`, not the camera's bit-bang bus

### Why
The DTS makes `gt911@5d` a child of the bit-banged `i2c-gpio-0` (SDA `gpio0 3`,
SCL `gpio0 2`) — the **camera's** bus. The stock kernel puts GT911 on the
**hardware controller (bus 0)**. On the bit-bang bus the panel NAKs (`-EREMOTEIO`
/ `-ENXIO`), and no hardware I²C controller registers at all. Full analysis:
`docs/historical/boot_experiment_log.md` → "Touch root cause" and the verified-bus note.

### Config change
`ark1668.dtsi` declares `i2c0` as `compatible = "snps,designware-i2c"`, so enable
the DesignWare platform driver (the currently-enabled `CONFIG_I2C_ARK` matches a
different `ark1668e.dtsi`, not this board):
```sh
scripts/config --enable CONFIG_I2C_DESIGNWARE_PLATFORM
```

### DTS change
Move `gt911` onto `&i2c0` and leave the camera alone on `i2c-gpio-0`:
```dts
&i2c0 {
    status = "okay";
    clock-frequency = <400000>;
    gt911: touchscreen@5d {
        compatible = "goodix,gt911";
        reg = <0x5d>;
        interrupt-parent = <&gpio0>;
        interrupts = <4 IRQ_TYPE_EDGE_FALLING>;   // INT = GPIO 4, falling
        irq-gpios   = <&gpio0 4 GPIO_ACTIVE_HIGH>;
        reset-gpios = <&gpio2 16 GPIO_ACTIVE_HIGH>; // RST = GPIO 80 = gpio2[16]
    };
};
```
Apply to the **build-tree DTS** actually compiled
(`arch/arm/boot/dts/ark1668_limcet_p305.dts`), not only the repo copy.
*Fallback:* if the DesignWare controller won't probe on this SoC, give touch its
**own** dedicated `i2c-gpio` bus on the GT911's real SDA/SCL pins — do **not**
leave it sharing `gpio0 2/3` with the camera.

---

## Verification (next boot log)

- **Touch:** a hardware I²C controller registers (look for
  `…e4300000.i2c` / DesignWare probe), and `Goodix-TS 0-005d` completes its i2c
  probe instead of `-6`/`-121`. Touch events on `/dev/input/event0`.
- **Camera:** `rn6752` (not `ark7116`) probes on the bit-bang bus at `…-002c`;
  `libCarReversing.so` / `arkapi_open_dvr()` succeeds; reverse gear shows video.

---

## Evidence artifacts (in-repo)

- `Prado firmware dump/mtd5_kernel/extracted/vmlinux` — decompressed stock 3.4.0 kernel
- `Prado firmware dump/mtd5_kernel/extracted/System.map` — 34,116-symbol map (recovered)
- `Prado firmware dump/mtd5_kernel/extracted/vmlinux.elf` — loadable ELF for re-disassembly
- `Limcet Hardware/kernel_dot_config` — the 4.19.192 build config (shows the wrong camera symbol)
- `docs/KERNEL_REFERENCE.md` — camera callout + full build recipe
- `docs/historical/boot_experiment_log.md` — touch bus proof, initramfs/driver inventory
- `docs/KERNEL_REFERENCE.md` — already-correct RN6752 identification

## Open / unverified
- Exact RN6752 **reset** and **reverse-detect** GPIO pin numbers (see caveat).
- Exact RN6752 `compatible` string — confirm against the BSP's `rn6752.c` match table.
