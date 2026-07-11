# Handoff — Boot-Log Review: `new kernel bootlog new uboot v6.txt`

**Reviewed:** 2026-07-11
**Log:** `docs/new kernel bootlog new uboot v6.txt` (649 lines)
**Build:** Linux **4.19.192** `#18` (gcc 12.2.0), machine `Limcet P305/P306`, built `Sat Jul 11 2026`
**cmdline:** `console=ttyS0,115200n8 mem=180M earlyprintk=serial root=/dev/mmcblk0p2 rootfstype=ext4 rootwait rw screen=0`

**Headline:** Major progress — this build boots an **ext4 root off the SD card** to a
shell, the **RN6752 camera driver now loads** (the ARK7116→RN6752 config/DTS swap took
effect), and LCD/GPU/WiFi-AP all come up. **Two blockers remain:** touch (DTS fix not yet
applied) and a **new** `MsnCoreApp` segfault (userspace).

---

## ✅ Working in v6

| Subsystem | Evidence (line) |
|---|---|
| **SD ext4 root boot** | `EXT4-fs (mmcblk0p2): mounted` (623) → `VFS: Mounted root (ext4)` (624) → `Run /sbin/init` (627). Boots directly off SD — **no initramfs required** (4.19 kernel has `dw_mmc` SD host + ext4 built-in). |
| **RN6752 camera driver** | `### rn6752_eq_work reset` (130) — driver loads/runs; **no ARK7116**. Config swap `CONFIG_VIDEO_RN6752=y` + DTS `dvr_rn6752@2c` worked. |
| **LCD framebuffer** | `ark1668_lcdfb e0500000.lcd: fb0` (83); `clk lcdclk rate 32400000` (~32.4 MHz, matches DTS 33 MHz). |
| **GPU** | `Galcore version 6.2.4.150331`. |
| **WiFi AP** | `rtl8821cu` + `wlan0: AP-ENABLED` (SSID `carplay_wifi`). |
| **SD/MMC host** | `dw_mmc ec400000.mmc` = mmc0 (SD); `ec800000.mmc` = mmc1 (SDIO WiFi, non-removable). |
| Reaches userspace shell | `/ #` prompt. |

---

## ❌ Blocker 1 — Touchscreen still broken (DTS fix not applied)

```
Goodix-TS 0-005d: i2c test failed attempt 1: -6
Goodix-TS 0-005d: I2C communication failure: -6      (-ENXIO)
```
- GT911 is still a child of the **bit-bang `i2c-gpio-0`** (bus `0`, SDA gpio0 3 / SCL gpio0 2)
  — the **camera's** bus — and `&i2c0 { status = "disabled" }` in the DTS.
- The log shows **no hardware I²C controller** registers; both bit-bang buses are flagged
  `Not I2C compliant: can't read SCL / Bus may be unreliable` (89–97) due to
  `i2c-gpio,scl-output-only`.
- **This is the still-outstanding fix from `HANDOFF_kernel_build_camera_and_touch.md` (Fix 2):**
  move `gt911@5d` onto hardware **`&i2c0`** (`status = "okay"`) and enable the controller
  driver (`CONFIG_I2C_DESIGNWARE_PLATFORM`, since `ark1668.dtsi` declares `i2c0` as
  `snps,designware-i2c`). Stock 3.4.0 runs GT911 on the hardware controller (bus 0) — verified.
  - *Open caveat:* confirm whether this SoC's `i2c0` is really DesignWare vs `arkmicro,ark-i2c`;
    if DesignWare won't probe, give touch its **own** dedicated `i2c-gpio` bus on the GT911's
    real SDA/SCL pins rather than sharing the camera's.

Touch pins are already correct in the DTS and match stock: **INT = GPIO 4** (falling),
**RST = GPIO 80 = gpio2[16]**, **addr 0x5d**. Only the **bus** is wrong.

---

## ❌ Blocker 2 — `MsnCoreApp` segfaults on launch (NEW, userspace)

```
/ # start_msn
Setting up environment for MsnCoreApp...
Starting MsnCoreApp (Software Framebuffer)...
Segmentation fault
[1]+  Segmentation fault         MsnCoreApp -qws
```
- Crashes **immediately**, with **no** preceding `error while loading shared libraries` /
  `undefined symbol` / `cannot open` — so it's not an obvious missing-lib case.
- This is a **rootfs/userspace** problem, not kernel: the kernel, `fb0`, GPU, and WiFi all
  initialised fine before this.
- **Likely lead — wrong screen profile.** The app computed a **6.94″ / 154×86 mm** panel:
  `set display inch: QSize(154, 86) 6.94433` (640), whereas the real Prado is ~**5.5″**
  (stock logged `QSize(120, 72) 5.5″`). A mismatched screen/resolution profile feeding QWS
  could null-deref on init.

### Debug plan
1. Get a backtrace: run `MsnCoreApp -qws` under `gdb` (`gdb --args MsnCoreApp -qws`, then
   `bt`) or `strace -f MsnCoreApp -qws 2>&1 | tail -40` to see the last syscall before SIGSEGV.
2. Check the SD rootfs's `msnprofile/MsnProductInfo.ini` — `ScreenType` / `ResolutionType` /
   `ResourceName` — against what this unit expects (see `docs/SCREEN.md` and
   `docs/ARKDATA_VARIANTS.md`). The 6.94″ figure suggests a wrong `ResolutionType`.
3. Verify the SD ext4 rootfs libraries/resources match build #18 (mismatched `libLauncher-*`
   / missing `.rcc` for the active resolution can crash the launcher).
4. Confirm `QWS_ARK_TOUCH_DEVICE` / framebuffer env is set (touch being down shouldn't crash
   the app, but check the QWS init path).

---

## ⚠️ Watch items (not yet blockers)
- **Camera detection unconfirmed:** RN6752 driver *loads* (`rn6752_eq_work`) but there is **no
  explicit "chip detected @ 0x2c"** line, and it sits on the unreliable bit-bang bus. Verify
  `/dev/video0` exists and the reverse camera actually streams at runtime.
- **NAND ECC:** `ECC too weak` / `uncorrectable ECC error` persist — irrelevant to SD boot
  (NAND now only backs `/nanddata` mtds).
- **i2c-gpio reliability:** `scl-output-only` + no clock stretching makes **both** the touch
  and camera buses "unreliable" per the kernel — another reason to move touch to hardware i2c.

---

## Priority order
1. **Fix `MsnCoreApp` segfault** — it's the UI blocker; start with the screen-profile lead.
2. **Apply the touch `&i2c0` DTS fix** — outstanding from the kernel-build handoff.
3. **Confirm RN6752 camera** streams (`/dev/video0`, reverse-gear test).

## References
- Log: `docs/new kernel bootlog new uboot v6.txt`
- Kernel-build fixes: `docs/HANDOFF_kernel_build_camera_and_touch.md`
- Touch root cause + stock-bus proof: `docs/boot_experiment_log.md`
- Screen/model selection: `docs/SCREEN.md`, `docs/ARKDATA_VARIANTS.md`
- Camera chip resolution: `docs/KERNEL_BUILD_REFERENCE.md` ("Camera decoder chip" callout)
