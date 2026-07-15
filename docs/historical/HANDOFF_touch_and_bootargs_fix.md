# Handoff: Fix touchscreen + SD bootargs on the Limcet P305/P306 (ARK1668) build

**Audience:** the agent/model that has the live `linux-arkmicro` build tree and
flashes U-Boot + kernel to the device. This is self-contained — you do not need
any prior conversation. Two independent fixes, both derived from analysis of the
`docs/new kernel bootlog*.txt` serial captures (Linux 4.19.192, `#7`→`#15`).

**Build tree (per `docs/KERNEL_REFERENCE.md`):**
`/home/osboxes/Downloads/linux-arkmicro/` with `linux/`, `u-boot/`, `env.source`.
Kernel DTS: `linux/arch/arm/boot/dts/ark1668_limcet_p305.dts`.
Boot image: `zImage` + appended DTB → `zImage.w_dtb`.

Apply Fix A and Fix B independently; each has its own verification.

---

## Fix A — Touchscreen (Goodix GT911) never probes

### Symptom (every boot log)
```
Goodix-TS 1-005d: i2c test failed attempt 1: -6      # new-U-Boot builds (-ENXIO)
Goodix-TS 1-005d: I2C communication failure: -6
Goodix-TS 0-005d: I2C communication failure: -121    # earlier builds (-EREMOTEIO)
```
No touch input. And **no hardware I2C controller ever appears** in the log
(no `i2c_designware …` / `…e4300000.i2c …` line).

### Root cause (confirmed in the DTS)
In `ark1668_limcet_p305.dts` the `gt911` node is a **child of the bit-banged
`i2c-gpio-0` bus (GPIO 3 = SDA, GPIO 2 = SCL)** — which is the **ARK7116 camera
decoder's** I2C bus. The GT911 is physically wired to the **hardware I2C
controller `&i2c0` (`i2c@e4300000`)** (see `docs/KERNEL_REFERENCE.md` §4:
"Goodix GT911 — Hardware I2C `&i2c0`"; §8: GPIO 2/3 = ARK7116 bus). So the
kernel drives touch transactions out the camera's pins and the panel never
answers.

> **Correction (2026-07-11):** every mention of "ARK7116" in this document is the
> **wrong camera chip** — the board actually has a **Richwave RN6752** at 7-bit
> `0x2c` (proven from the stock 3.4.0 kernel). This does **not** change the touch
> fix below (touch belongs on hardware `&i2c0` regardless of which decoder sits on
> the bit-bang bus), but the camera node itself needs retargeting. See
> `docs/historical/HANDOFF_kernel_build_camera_and_touch.md` and the "Camera decoder chip"
> callout in `docs/KERNEL_REFERENCE.md`.

Compounding: the board `#include`s `ark1668.dtsi`, whose `i2c0` is
`compatible = "snps,designware-i2c"`, but the config enabled `CONFIG_I2C_ARK`
(that matches `ark1668e.dtsi`'s `arkmicro,ark-i2c`, a different SoC file). So
the hardware controller has no driver and doesn't register.

### Step A1 — kernel config
```bash
source env.source && cd linux
scripts/config --enable CONFIG_I2C_DESIGNWARE_PLATFORM   # driver for i2c@e4300000
scripts/config --enable CONFIG_TOUCHSCREEN_GOODIX        # confirm GT911 driver on
make olddefconfig
# sanity:
grep -E "CONFIG_I2C_DESIGNWARE_PLATFORM|CONFIG_TOUCHSCREEN_GOODIX" .config
```
`CONFIG_I2C_ARK` may stay set or be removed — it does not match this board's
`i2c0` and is not what makes touch work.

### Step A2 — DTS edit (`linux/arch/arm/boot/dts/ark1668_limcet_p305.dts`)

**(a) DELETE the `gt911` node from inside `i2c-gpio-0`.** Remove exactly this
block (leave the `dvr_ark7116` camera node and the `i2c-gpio-0` bus itself in
place):
```dts
		// Goodix Touchscreen Controller (Reset pin GPIO 80, IRQ GPIO 4)
		gt911: touchscreen@5d {
			compatible = "goodix,gt911";
			reg = <0x5d>;
			interrupt-parent = <&gpio0>;
			interrupts = <4 IRQ_TYPE_EDGE_FALLING>;
			irq-gpios = <&gpio0 4 GPIO_ACTIVE_HIGH>;
			reset-gpios = <&gpio2 16 GPIO_ACTIVE_HIGH>;
			touchscreen-inverted-x;
		};
```

**(b) ADD an `&i2c0` override at the top level** (after the root `/ { … };`
block closes, alongside other `&label { … }` overrides):
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
Notes:
- `i2c0` in `ark1668.dtsi` already has `pinctrl-0 = <&pinctrl_i2c0>` and defaults
  to `status = "okay"`; the explicit `status`/`clock-frequency` here are for
  clarity. Confirm `pinctrl_i2c0` is defined in `ark1668-pinctrl.dtsi` and its
  pins are not claimed by another enabled node.
- Keep `reset-gpios`/`irq-gpios` exactly as above — those (GPIO4 IRQ, GPIO2[16]
  reset) are already correct and unchanged; only the **bus** moves.

### Step A3 — rebuild + reassemble boot image
```bash
source env.source && cd linux
make -j$(nproc) zImage dtbs
cat arch/arm/boot/zImage arch/arm/boot/dts/ark1668_limcet_p305.dtb > ../zImage.w_dtb
```
Flash `../zImage.w_dtb` as usual.

### Step A4 — verify on next boot log (PASS criteria)
- A hardware I2C controller registers, e.g.:
  `i2c_designware e4300000.i2c: ...` (or similar for `snps,designware-i2c`).
- Touch now probes on **bus 0**, addr 0x5d, and **succeeds** — the
  `I2C communication failure` line is GONE. Expect instead a Goodix ID/config
  line and an input device, e.g. `Goodix-TS 0-005d: ID 911, version: ....` and
  an `input:` device registered.
- Touching the panel produces input events (`evtest`, or the UI responds).

**FAIL fallback:** if `i2c_designware e4300000.i2c` still does not register (the
`ark1668.dtsi` designware node may be wrong for this SoC and it is actually an
`arkmicro,ark-i2c`), do NOT return touch to GPIO 2/3 (camera bus). Instead
identify the GT911's real SDA/SCL SoC pins from the schematic and either point
`&i2c0` at the correct controller compatible or give touch its **own** dedicated
`i2c-gpio` bus on those pins. The invariant: touch must never share GPIO 2/3
with the ARK7116 camera.

---

## Fix B — SD bootargs passes literal `${mtdparts}` / `${screen}`

### Symptom (latest log `new kernel bootlog new uboot v3.txt`)
```
Kernel command line: console=ttyS0,115200n8 mem=180M earlyprintk=serial \
  root=/dev/mmcblk0p2 rootfstype=ext4 rootwait rw ${mtdparts} screen=${screen}
```
`${mtdparts}` and `${screen}` reached the kernel **unexpanded** (a regression —
the previous build `new kernel bootlog new uboot v2.txt` still passed the real
`mtdparts=ark1680-nand:…` string). U-Boot only expands `${var}` when the string
is evaluated by the hush shell (`run`); `bootz` passes `bootargs` to the kernel
verbatim, so a literal `${…}` in `bootargs` is handed to the kernel as-is.

### Fix
Find where the SD/mmc-path `bootargs` is set (one of: the board header
`u-boot/include/configs/ark1668_tyw_zksw.h`; the defconfig
`u-boot/configs/ark1668_tyw_zksw_defconfig`; a `boot.scr`/`uEnv.txt` on SD p1;
or a manual `setenv` at the prompt). Then either:

**Option 1 (simplest, recommended) — drop the two tokens.** The appended DTB
already describes the panel and (if present) the NAND partitions, so neither is
needed for SD boot:
```
setenv bootargs 'console=ttyS0,115200n8 mem=180M earlyprintk=serial root=/dev/mmcblk0p2 rootfstype=ext4 rootwait rw'
```

**Option 2 — keep NAND partition names.** Only if something on the running
kernel must address NAND partitions by name. Use the resolved literal (NOT
`${mtdparts}`):
```
setenv bootargs 'console=ttyS0,115200n8 mem=180M earlyprintk=serial root=/dev/mmcblk0p2 rootfstype=ext4 rootwait rw mtdparts=ark1680-nand:128k(S-Loader),512k(U-boot),512k(U-boot_back),256K(U-boot-Env),256K(arkdata),4m(kernel),106m(rootfs),6m(userdata),512K(bootlogo),3m(bootanimation),3m(reversingtrack),256K(Unicode) screen=0'
```
(Caveat: this kernel's NAND ECC is mismatched — `ECC … too weak`, ~417 false bad
blocks — so NAND access is unreliable regardless; see Fix C.)

**Option 3 — proper expansion.** If you want to keep `${mtdparts}`/`${screen}`
templated, define both env vars and build `bootargs` through hush so they expand
at set time:
```
setenv mtdparts 'mtdparts=ark1680-nand:128k(S-Loader),512k(U-boot),512k(U-boot_back),256K(U-boot-Env),256K(arkdata),4m(kernel),106m(rootfs),6m(userdata),512K(bootlogo),3m(bootanimation),3m(reversingtrack),256K(Unicode)'
setenv screen 0
setenv sdargs 'setenv bootargs console=ttyS0,115200n8 mem=180M earlyprintk=serial root=/dev/mmcblk0p2 rootfstype=ext4 rootwait rw ${mtdparts} screen=${screen}'
# boot path must `run sdargs` before bootz (run => hush expansion)
```

### Verify (PASS criteria)
Next boot log `Kernel command line:` shows the resolved string with **no literal
`${…}`** remaining.

---

## Fix C — NAND "too weak ECC" + ~417 false bad blocks

**Not required for SD boot** — the SD path never mounts NAND. Read this only if
something must read the stock NAND partitions from the new stack. There are
**two distinct issues** here; do not conflate them.

### Chip + stock ECC scheme (the target to match)
NAND is a Toshiba `Manufacturer ID 0x98, Chip ID 0xf1` (SLC 128 MiB, 2048-byte
page, 64-byte OOB). The stock S-Loader header (in
`docs/bootlog_prado_holden_firmware.txt`) states the exact scheme:
```
EccCodeSize: 0x0000000d   -> 13 ECC bytes per segment
SegSize:     0x00000200   -> 512-byte segment
 ECC 7 bit !!             -> BCH 7-bit correction
```
=> **hardware BCH 7-bit, 512-byte step, 13 ECC bytes/segment** (4 segments per
2 KB page = 52 ECC bytes, ~12 OOB bytes left incl. the bad-block marker),
on-flash BBT. Any reader OR writer of this NAND must use exactly this.

### Issue A — the `too weak ECC` WARNING is BENIGN
```
nand: WARNING: ark-nand: the ECC used on your system is too weak compared to the
one required by the NAND chip
```
This appears in *every* boot, including ones that read NAND perfectly. It is
MTD's `nand_scan_tail()` noting the chip's datasheet wants stronger ECC than the
ARK hardware's 7-bit. Stock ran 7-bit for years. **Do not "fix" it** — raising
strength would stop matching stock-written data. Ignore it.

### Issue B — the ~417 false bad blocks are caused by the NEW U-BOOT, not the kernel
Evidence (same kernel build #13 in both):

| Boot path | Bad blocks reported |
|---|---|
| SD / patched-stock U-Boot (`v2`–`v4`) | **1** (factory BBT read correctly) |
| Freshly-compiled U-Boot (`nu-v2`/`nu-v3`) | **417** |

The *same kernel* reads 1 vs 417 depending only on which U-Boot ran first ->
the kernel's NAND ECC is fine; the **new U-Boot** reads/writes NAND with an
ECC/BBT scheme that does NOT match stock 7-bit/512/13. With `nand-on-flash-bbt`,
when it can't parse the factory bad-block markers (wrong OOB layout) it scans
and writes a fresh, polluted BBT marking ~400 good blocks bad, which the kernel
then inherits.

### Fix — pick one

**Option 1 (recommended): keep the new stack OFF NAND.** For SD boot NAND is
dead weight (root is on SD; NAND partitions are copied to `/nanddata` on SD via
`redirect_mtd_data`). So:
- Issue no `nand write/scrub/erase` in the new U-Boot, and keep its environment
  **off NAND** (on SD) so it never rewrites the on-flash BBT.
- Then Issue B stops happening and Issue A is harmless (kernel never mounts NAND).

**Option 2 (only if NAND access is truly needed): match 7-bit/512/13 in BOTH
U-Boot and kernel.**
1. **New U-Boot** (`ark1668_tyw_zksw`): set the ARK NAND ECC to **7-bit / 512-byte
   step / 13 ECC bytes** in the board NAND config, so it reads the existing BBT
   instead of creating a polluted one. **This is the actual source of the 417.**
2. **Kernel**: the `arkmicro,ark-nand` driver already read correctly under the
   SD path, so it is likely 7-bit already; confirm `ecc.strength=7, ecc.size=512`.
   Neither `.dtsi` sets `nand-ecc-strength`, so it comes from the driver/hardware
   default. If it needs forcing, add to the `nfc: nand@ec000000` node (as an
   `&nfc { … };` override in the P305 DTS) — **only if the driver honors DT**:
   ```dts
   &nfc {
       nand-ecc-strength = <7>;
       nand-ecc-step-size = <512>;
   };
   ```
3. After both match, **erase the already-polluted on-flash BBT** so it rebuilds
   from the real factory markers. Verify read-only first.

### ⚠️ Safety (applies to Option 2)
Do **not** let the new U-Boot or kernel `write`/`scrub`/`erase` NAND until the
ECC matches stock 7-bit. A wrong-ECC write to the BBT — or to the
S-Loader/U-Boot/kernel partitions — can brick the unit (JTAG-only recovery).
Keep NAND strictly **read-only** while verifying. (The stock updater does
`nand scrub`/`nand write` on the bootloader region — that is exactly the
operation that is dangerous with a mismatched ECC.)

---

## Priority
1. **Fix A (touch)** — blocker; the head-unit UI is unusable without it.
2. **Fix B (bootargs)** — quick U-Boot env change; restores what build `nu-v2`
   already had.
3. **Fix C (NAND)** — usually just **Option 1 (avoid NAND)**; only pursue the
   ECC match if stock NAND access is required, and note the fix that matters is
   in the **new U-Boot**, not the kernel.
