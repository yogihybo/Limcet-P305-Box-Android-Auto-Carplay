# Handoff — Boot-Log Review: `new kernel bootlog new uboot v6.txt`

**Reviewed:** 2026-07-11
**Log:** `docs/new kernel bootlog new uboot v6.txt` (649 lines)
**Build:** Linux **4.19.192** `#18` (gcc 12.2.0), machine `Limcet P305/P306`, built `Sat Jul 11 2026`
**cmdline:** `console=ttyS0,115200n8 mem=180M earlyprintk=serial root=/dev/mmcblk0p2 rootfstype=ext4 rootwait rw screen=0`

**Headline:** Major progress — this build boots an **ext4 root off the SD card** to a
shell, the **RN6752 camera driver now loads** (the ARK7116→RN6752 config/DTS swap took
effect), and LCD/GPU/WiFi-AP all come up. **Two blockers remain:** touch (bus choice has been
flip-flopped twice by inference alone — commit `7c7ce4c` then `0be21c7` — and now needs a
live-hardware I²C scan to settle, see `tools/i2c-scan/`) and a **new** `MsnCoreApp` segfault
(userspace).

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

## ❌ Blocker 1 — Touchscreen still broken

> **⚠️ SUPERSEDED (2026-07-11):** everything below chased the wrong device.
> Live I²C scanning (bus placement, reset toggling, pin polarity — all
> ruled correct/ruled out) never found a GT911 anywhere because **this
> unit's stock firmware doesn't load GT911 at all** — it loads
> `ark1680_ts.ko`, the SoC's built-in resistive ADC touch controller,
> selected via a marker file (`/msnprofile/ark1680_ts`, present in the
> live NAND dump). See `docs/historical/boot_experiment_log.md` → "ROOT CAUSE FOUND"
> for the full trail. The 4.19 port needs a resistive ADC/TSC driver, not
> a GT911 I²C fix — the section below is kept for historical context only.

```
Goodix-TS 0-005d: i2c test failed attempt 1: -6
Goodix-TS 0-005d: I2C communication failure: -6      (-ENXIO)
```
- GT911 is still a child of the **bit-bang `i2c-gpio-0`** (bus `0`, SDA gpio0 3 / SCL gpio0 2)
  — the **camera's** bus — and `&i2c0 { status = "disabled" }` was in the DTS at the time
  this log was captured.
- The log shows **no hardware I²C controller** registers; both bit-bang buses are flagged
  `Not I2C compliant: can't read SCL / Bus may be unreliable` (89–97) due to
  `i2c-gpio,scl-output-only`.
- **⚠️ Correction from initial review:** this doc originally said the fix is to move
  `gt911@5d` onto hardware `&i2c0`, per `HANDOFF_kernel_build_camera_and_touch.md` (Fix 2) and
  the stock 3.4.0 disassembly proof in `boot_experiment_log.md`. But that exact move was
  **already tried and reverted same-day**: commit `7c7ce4c` moved touch to `&i2c0`; commit
  `0be21c7` (made 25 min before this review doc, and immediately before it in the log) moved
  it back to `i2c-gpio-0` "to match actual hardware wiring" — with **no evidence recorded**
  anywhere for that reversal. So the disassembly-backed fix has already been tried once
  without confirmation of the outcome, and blindly redoing it risks a third silent flip-flop.
  - **Do not edit this DTS node again without hardware confirmation.** The DTS now enables
    both `&i2c0` and `i2c-gpio-0` simultaneously (disjoint pins, no conflict) specifically so
    this can be settled empirically — see `docs/historical/boot_experiment_log.md` → "Systematic I²C bus
    verification" and `tools/i2c-scan/` for a live-shell probe tool and procedure. Run that
    scan on hardware, record the result, **then** attach `gt911@5d` to whichever bus actually
    ACKs `0x5d`.

Touch pins are already correct in the DTS and match stock: **INT = GPIO 4** (falling),
**RST = GPIO 80 = gpio2[16]**, **addr 0x5d**. Only the **bus** is in question — and it should
be resolved by probing the live board, not by re-inferring from disassembly a third time.

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
   `ResourceName` — against what this unit expects (see `docs/DISPLAY_SUBSYSTEM.md` and
   `docs/DISPLAY_SUBSYSTEM.md`). The 6.94″ figure suggests a wrong `ResolutionType`.
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
  (NAND now only backs `/nanddata` mtds). **Root-caused (2026-07-11), see
  `docs/historical/boot_experiment_log.md` "NAND ECC too weak"** — a genuine, unfixable hardware
  mismatch (this Toshiba SLC chip's ID reports it needs 8-bit ECC; the controller's
  discrete BCH modes jump straight from 7-bit to 13-bit, and 13-bit doesn't fit this
  chip's 64-byte OOB) — not a misconfiguration. The verbose per-block bad-block log
  spam (hundreds of lines) *was* fixed — patched to a one-line summary count.
- **i2c-gpio reliability:** `scl-output-only` + no clock stretching makes **both** the touch
  and camera buses "unreliable" per the kernel — another reason to move touch to hardware i2c.

---

## Priority order
1. **Fix `MsnCoreApp` segfault** — it's the UI blocker; start with the screen-profile lead.
2. **Run the live I²C bus scan** (`tools/i2c-scan/`) to settle the touch bus question with
   hardware evidence, then attach `gt911@5d` to the confirmed bus — do not guess again.
3. **Confirm RN6752 camera** streams (`/dev/video0`, reverse-gear test).

## References
- Log: `docs/new kernel bootlog new uboot v6.txt`
- Kernel-build fixes: `docs/historical/HANDOFF_kernel_build_camera_and_touch.md`
- Touch root cause + stock-bus proof + pending live verification: `docs/historical/boot_experiment_log.md`
- Live I²C bus scan tool: `tools/i2c-scan/`
- Screen/model selection: `docs/DISPLAY_SUBSYSTEM.md`, `docs/DISPLAY_SUBSYSTEM.md`
- Camera chip resolution: `docs/KERNEL_REFERENCE.md` ("Camera decoder chip" callout)
