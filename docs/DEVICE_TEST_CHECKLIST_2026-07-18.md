# Device Test Checklist — 2026-07-18 session

**Major update, same day:** rather than keep chasing individual
mismatched files, the entire base rootfs
(`firmware_source/prado_reconstructed/mtd6_rootfs/rootfs`) has been
replaced with Holden's confirmed-working 2024-02-21 build (`1925b6a`).
Prado's own device identity (`config.ini`, `MsnProductInfo.ini` —
`Limcet-P306`/`McuType=6`, `FactoryConfig.ini`, boot branding) was kept
as-is, and everything Holden's build doesn't ship at all but Prado's
stock dump did (`sshd`+SSH host keys, `usbmuxd`/`adb`/`carlife`,
`libMsnAirPlay.so`, `wifi_ap.sh`, the `Launcher-Box-P301-*.rcc`
resource pack our own `ResourceName=Box-P301` actually loads) was
restored rather than silently dropped. See the commit message and
`firmware_source/.../holden_identity_reference/README.md` for the full
rationale. **This supersedes the individual blueware-file fix below —
that fix is now just one part of the wholesale rebase.** Flash and
retest everything in this checklist fresh against the new image, not
just Bluetooth.

Everything below is already built and committed (main repo `96eef2e`,
`linux-arkmicro` `3bc02e43e`). This is just the on-device verification
pass — nothing here needs a rebuild unless noted.

Record results inline (pass/fail + notes) so this doubles as the
session's evidence log.

---

## 1. LCD red-tint fix

**Change:** reverted `pinctrl_lcd_prado` back to full `pinctrl_lcd_rgb888`
(`linux-arkmicro` `a8e3ecc77`).

- [ ] Flash/boot the latest kernel + DTB.
- [ ] Boot to `MsnCoreApp`'s normal UI.
- [ ] Confirm: no flicker (already confirmed fixed, re-check it hasn't
      regressed).
- [ ] Confirm: red tint on UI elements (icons, buttons) — gone or still
      present?

**Result:** _____________________

---

## 1b. LCD alpha-blend channel-order fix (2026-07-19)

**Confirmed by user testing:** position offset fixed (arkdata.ini/DTB
patching) and red tint is gone, but colors on alpha-blended UI
elements (icons, anti-aliased widgets) are visibly skewed while the
flat opaque background looks correct — a different, more specific
symptom than the original red-tint bug.

**Root cause found via direct Ghidra comparison** against stock's real
LCDC driver (not another config/register guess): `ark1668_lcdfb.c`'s
panel-init path did a flat literal write to `ARK1668_LCDC_OSD1_CTL`
that unconditionally zeroed the RGB channel-order field (bits 18-22)
on every `fb_set_par()` call, clobbering whatever the ioctl path's own
correct read-modify-write helper had set. That field controls channel
routing in the blend unit's actual compositor math — which only
applies to partial-alpha pixels, exactly matching the symptom (opaque
background fine, blended elements wrong). Fixed in `linux-arkmicro`
`ad7b2647c` — proper read-modify-write now preserves that field.

**Update: `ad7b2647c` alone wasn't enough — hardware-tested 2026-07-19,
colors still skewed.** Turned out to be a near-total no-op: nothing in
our boot path or in Qt/MsnCoreApp (`QWS_DISPLAY=linuxfb:...`, confirmed
via `start_msn` to use the plain LinuxFB QScreen driver with no `:fb=`
suboption, i.e. `/dev/fb0` = OSD1) ever calls the vendor ioctl that
`rgb_order` gets set through — so the RMW-preserve fix was preserving a
value that was never anything but its hardware reset default (0)
anyway, both before and after the fix.

Re-ran `fb-alpha-test` (already deployed) and got a much sharper,
decisive result: only **4 visible bars instead of 6**, with the widths
matching exactly what two specific band-merges would produce. Bands 1
(opaque red, `0xffff0000`) and 2 (half-alpha red, `0x80ff0000`) — RGB
bytes identical, alpha byte the only difference — rendered as one
uniform "wide strong red" bar. Bands 4/5 (raw-byte tests) merged
similarly. **This means the alpha byte had zero effect on the
composited output** — confirmed as data, not photo-guesswork.

Checked whether the two relevant enable bits (`MODE_LCD_REG1`'s
`alpha_blend_en`/`per_pix_alpha_blend_en` for OSD1, bits 13/12) were
actually live via a direct `devmem 0xe0500064 32` read on hardware:
**`0x00003001`** — both bits genuinely set. So the enable bits were
never the problem either.

**Real root cause, found via further Ghidra comparison:** a completely
separate register field, `MODE_LCD_REG0` bits `[15:12]` — OSD1's 4-bit
"blend mode" (confirmed via stock's `ark_disp_set_osd_blend_mode_lcd()`,
`vmlinux.elf @ 0x802deb08`, which RMWs exactly this nibble). Our
`ark1668_lcdc_dev_init()` never touched it in either code path (the
DTS `lcd-priority` formula only ORs into bits `[3:0]`/`[11:8]`/
`[19:16]`/`[27:24]`; the fallback literal `0x03000204` also leaves it
`0`) — so OSD1 was stuck at blend mode 0 regardless of the enable bits
being on. Confirmed via stock's own `ark_disp_dev_init()`
(`vmlinux.elf @ 0x802ddde4-802ddde8`) that every layer's *default*
`blend_mode` struct field is hardcoded to `1`, not `0`, before any
DTS/ioctl override — mode 0 evidently means "ignore per-pixel alpha
entirely," matching the symptom exactly. Fixed in `linux-arkmicro`
`063c5be8c` — OSD1's blend mode nibble now explicitly set to `1` in
both `ark1668_lcdc_dev_init()` code paths.

- [x] Flash this kernel. `devmem 0xe0500060 32` confirmed `0x03001204`
      on real hardware — bits `[15:12]=1`, the fix genuinely landed.
- [x] Re-run `fb-alpha-test`. **Still visibly wrong** — see below.

**Update: fix landed but didn't resolve it — live register sweep
findings (2026-07-19).** With `blend_mode=1` live, `fb-alpha-test`
still showed the old merged-band symptom. Rather than more blind
kernel rebuilds, switched to sweeping registers live via `devmem` on
`LCDC` (`0xe0500000`), since `MODE_LCD_REG0`/`OSD1_CTL` are just MMIO —
no reboot needed per attempt. Findings, most useful first:

- **Opaque pixels (alpha=255) always render correctly** on every
  combination tried, before and after every change below. Only
  *partial*-alpha pixels are ever wrong.
- Swept `blend_mode` (`MODE_LCD_REG0[15:12]`) through all 16 values on
  hardware: **modes 9, 10, and 14 genuinely enable per-pixel alpha
  blending** (`fb-alpha-test` shows 6 distinct bands, not 4 merged
  ones) — mode `1` (stock's struct-init default, what the kernel fix
  above sets) does *not* by itself turn on real blending on our
  reconstruction. Unclear why; no vendor register documentation found.
- With `blend_mode=9` locked in, swept what was believed to be
  `rgb_order` (`OSD1_CTL[20:18]`) through all 8 values: band 2
  (half-alpha red) came out dark-green (0,1), light-green (2,3), brown
  (4,5), dark-green (6,7) — never a correct dim red.

**Correction found via further Ghidra tracing (still 2026-07-19):**
this project had `rgb_order`/`yuv_order` **swapped**. Traced the real
parameter-passing convention in stock's `ark_disp_set_osd_format(id,
format, yuv_order, rgb_order)` (`vmlinux.elf @ 0x802ddf98`):
```
orr r2, r1, r7, lsl #18   -- r7 = yuv_order (3rd arg) -> bits[20:18]
orr r3, r2, r6, lsl #21   -- r6 = rgb_order (4th arg) -> bits[22:21]
```
`yuv_order` is the 3-bit field at bits `[20:18]`; `rgb_order` is only
a **2-bit** field at bits `[22:21]` — never tested. What the sweep
above actually tested was `yuv_order`, not `rgb_order`. Confirmed our
own `ark1668_lcdc_set_osd_format()` (`ark1668_lcdc_funcs.c`) had this
exact swap baked in (`(yuv_order&3)<<21 | (rgb_order&7)<<18`, backwards
from stock) — fixed in `linux-arkmicro` `730c5cf1c`. That function
isn't on the live boot path so the fix alone doesn't resolve the
hue-corruption bug, but confirms the Ghidra trace is correct.

Also checked whether stock's real `MsnCoreApp`/`libarkadapt.so`/
`libarkcmn.so` binaries hardcode a correct value when calling the
vendor `VIN_SET_WINDOW_FORMAT` ioctl (`0x4f3b`) — the two vendor
libraries are stripped with no ioctl symbols found by name;
`MsnCoreApp` itself (unstripped) had no vendor-ioctl symbols either,
so the actual call (if any) is buried in a stripped library. Not
pursued further (binary-level RE, more effort than the register sweep
for uncertain payoff) — noted as a future option.

Also **ruled out** two other candidates via Ghidra (no more hardware
testing needed for these):
- `Y2R_COEF321`/`654`/`7` — our driver's literals (computed from
  `(425<<20)|(91<<10)|(298<<0)` etc.) match stock's hardcoded values
  in `ark_disp_set_lcd_panel_type()` (`@ 0x802e0a78`) byte-for-byte
  (`0x1a916d2a`, `0x1d12e060`, ORs in `0x1029`). Not the cause.
- `ALPHA1_0_VIDEO_OSD1` (offset `0x24`) — traced every access to the
  LCDC MMIO base across the entire display-driver code range;
  `ark_disp_set_layer_cfg()` (the "apply full layer config" function)
  calls a comprehensive list of per-layer setters and none target this
  offset. Very likely unused for this board.

**Consolidated next-session test plan:** `tools/fb-alpha-test/
lcd-blend-sweep.sh` (deployed to the overlay, on-device as
`lcd-blend-sweep.sh`), rewritten to prioritize the corrected fields:
1. **Phase 1 (most promising, untested):** the real `rgb_order` field
   (`OSD1_CTL[22:21]`, only 4 values), `yuv_order`/`format` held
   fixed.
2. **Phase 1b:** if phase 1 alone doesn't work, the combined
   `rgb_order` × `yuv_order` sweep (16 combos).
3. **Phase 2 (low priority):** `format` (`OSD1_CTL[15:12]`) swept
   0–15 — confirmed via Ghidra to be a direct passthrough (no
   remapping), so `6` (RGBA888) is very likely already correct.

See `tools/fb-alpha-test/README.md` for the full writeup.

---

## 2. `mcu-handshake` — touch-switch trigger (highest priority)

**Change:** wire protocol corrected (real checksum, real frame format,
no dead-end 0xFA/0xAF replies), now sends 3 confirmed proactive frames
on startup instead of one.

**Important update (2026-07-18):** a kernel-level fix landed after this
was first tested (`linux-arkmicro` `215c2b36f` — `ark_hsuart.c`'s
`uartclk` was resolving wrong via `clk_get_rate()` for both `ttyHS0`
and `ttyHS1`, now hardcoded to 24MHz matching stock). If earlier
`mcu-handshake` attempts got zero response, **retest with the new
kernel before concluding anything about the frame content/protocol** —
the wrong clock rate would make every baud rate on this port wrong too,
which alone could fully explain "never returned any data" independent
of anything protocol-level.

```sh
killall MsnCoreApp
mcu-handshake --verbose
```
- [ ] Watch the `[TX]` lines — confirm all 3 frames go out: `cmd=81`,
      `cmd=82`, `cmd=84`.
- [ ] **Try the touchscreen immediately after the 3 frames send** —
      does it respond now?
- [ ] Watch for any `[RX]` frames from the MCU (informational — logged,
      no reply sent back, that's expected/correct now).
- [ ] If nothing: toggle a vehicle input (reverse, ACC, a button) to
      prompt more MCU traffic, try touch again.
- [ ] If still nothing: try `mcu-handshake --scan 10 -v` to sweep other
      baud rates, just to rule that out completely.

**Result:** _____________________
**If touch worked, which frame do you think triggered it (if you can
tell from timing)?** _____________________

**Update (2026-07-18, `new uboot new kernel baseline v4.txt`,
normal `MsnCoreApp` boot — not a dedicated `mcu-handshake` test):**
**the physical scroll knob on the head unit works, confirmed by the
user directly on hardware.** The log shows exactly what that looks
like at the `MsnMainWindow` level:
```
[184.267]  MsnMainWindow::setRealVisible LauncherWindow(...) false
[184.327]  MsnMainWindow::setRealVisible SettingWindow(...) true
[184.339]  Waite Event Ticks 10
[208.156]  MsnMainWindow::setRealVisible SettingWindow(...) false
[208.156]  MsnMainWindow::setRealVisible LauncherWindow(...) true
```
Launcher → Settings → (held ~24s) → back to Launcher, in real time —
real UI navigation, not a static log line. This is the strong,
physical-behavior kind of evidence (see project memory on weak
boot-log evidence) — stronger than anything `mcu-handshake` alone
could show. `libMcuCenter.so` (line 852, `Load App Plugin 401`) logs
nothing at the individual knob-event level (closed-source, silent),
so this window-visibility transition is the only observable trace,
but it's conclusive: **MCU input is reaching `MsnCoreApp` over
`ttyHS0`.** Same root cause and same fix as Bluetooth (§3's
`clk_prepare_enable()` fix in `ark_hsuart.c`, `linux-arkmicro`
`33fd16e31`) — both ports share this one driver file. The
`"Open MCU Serial Port ... Ret: true \"No such file or directory\""`
line still prints (already known to be an unreliable/buggy log string
from `MsnCoreApp` itself, not real evidence either way) — the physical
knob test is what actually settles this.

**Still open:** touchscreen itself not yet confirmed working this same
way — worth testing directly now that MCU input is confirmed alive.

**Root cause of touch never responding, found (2026-07-19):** every
touch test since v7 has actually been explained by
`CONFIG_TOUCHSCREEN_ARK1680` being missing from the kernel entirely —
a seventh instance of the `--defconfig`-regeneration pattern. `new
uboot new kernel baseline v10.txt` has no
`ark1680_ts e4500000.tsc: probe:`/`ARK1680 resistive touchscreen
registered` line anywhere at all (present in every prior successful
boot, very early, since it's a built-in driver independent of any
insmod/modprobe timing) — the driver was simply never in the kernel.
This was never a downstream consequence of `ark_display` or anything
else investigated in the meantime; that theory is ruled out. Fixed in
`linux-arkmicro` `8a0da9c96`.

- [ ] Flash this kernel. Confirm `ark1680_ts e4500000.tsc: ... resistive
      touchscreen registered` appears early in dmesg.
- [ ] Retest touch directly — a real finger test, now that the driver
      is actually present for the first time since v4.

---

## 3. Bluetooth — first real test on our own image

**Change:** 5 missing files added (`rtl8761bt_fw`, `librtkvnd.so`,
`rtkbt.conf`, 2× `RingTone.wav`).

**Result (first test, 2026-07-18): FAIL, but diagnostic.** Files
confirmed present and MD5-correct on device, but `blueware`'s log
skipped the whole `realtek selected` / `openning librtkvnd.so` /
`set:fwdir=/etc/` block entirely, jumping straight to
`bt_chipset_vnd_init` → `firmware_config_cb:1` → the same
`*pgd=00000000` crash (`0xfcee0112`) seen since the start of this
investigation. Also found (from a separate full boot log,
`new uboot new kernel baseline v2.txt`): `hsuart1` never reaches the
`1500000` baud stock's own log shows it reaching in ~160ms — ours
loops at `115200` forever. Traced this to a **real kernel bug**: a
missing interrupt-clear bit in `ark_hsuart.c`, now fixed
(`linux-arkmicro` `21d254af0`) — not yet tested. **Retest with the new
kernel before troubleshooting `blueware`/rootfs further**, this may
be the actual root cause of the crash, not a rootfs/config issue.

```sh
killall blueware
/usr/bin/blueware /etc/blueware-bw121.properties
```
- [ ] Watch for `openning librtkvnd.so` (confirms the vendor lib loads
      and the full init sequence runs, not just present-on-disk).
- [ ] Watch for `hsuart1` reaching `baud:1500000` (not stuck at 115200)
      in `dmesg` around the same time.
- [ ] Watch for `firmware_config_cb:0` (success) not `:1` (the old
      failure).
- [ ] Watch for full HCI bring-up ending in `[BLUEWARE:ON][NAME:...]`.
- [ ] Confirm a phone can actually see/pair with the Bluetooth radio.

**Result (retest with ark_hsuart fix, v3 log 2026-07-18): FAIL,
identical crash.** Bit-for-bit identical register state to every prior
crash (`r0=fcee0104`, `r1=000b8cbc`, `r2=000dd678`, all exact) across
4 separate runs spanning different kernel/config states this session —
confirms this is NOT data-dependent (garbled bytes would vary) and NOT
fixed by the RXD-clear change. `hsuart1` still stuck at `115200`,
never reaches `1500000`. New finding: stock's log has an
`nvm_init`/`nvm_write_database /data/feasycom/bw_conf*.db` block right
after config parsing, before the GPIO91/AT block -- **completely
absent** in ours. `/data/feasycom/` itself confirmed to already exist
on-device (not a missing-directory issue) -- so this points at
something failing *within* that nvm/database step itself, not the
directory being absent.

- [ ] Confirm `/data/` is actually mounted read-write at the point
      `blueware` runs (not just present -- check it's not still
      read-only or unmounted from an earlier boot stage):
      ```sh
      mount | grep data
      touch /data/feasycom/test_write && echo "write OK" && rm /data/feasycom/test_write
      ```
- [ ] Compare full `blueware -v`-style verbose output (if available)
      or a raw `/dev/ttyHS1` byte sniff (stop blueware first, per
      `docs/MCU_ADAPTERS.md` Method B) against stock's captured
      sequence line-by-line, specifically checking whether `nvm_init`
      ever gets attempted at all vs. silently skipped.

**Correction (2026-07-18):** a full Ghidra decompile of stock's real
`ark1680_hsuart_clear_interrupt` found stock does **not** clear
`HSUART_INT_RXD`/bit9 either — it's a live/self-clearing FIFO-status
flag on this silicon, not a W1C interrupt-pending bit. The RXD-clear
change (`21d254af0`) is harmless but was **not** a real fix — don't
treat it as resolved, it just happened to be tested alongside the real
one below.

**Strongest lead found (2026-07-18), now fixed and built, not yet
tested:** the same Ghidra pass found stock's driver hardcodes
`uartclk = 24000000` directly and never calls any clock-framework API
— ours instead called `clk_get_rate(uap->clk)`. Our own boot log shows
every *other* clock printing its resolved rate at boot except
`hsuart0clk`/`hsuart1clk` specifically (the two-parent
`arkmicro,ark-clk-sys` clocks) — strong evidence that call was
returning a wrong value for exactly the two ports showing this bug.
A wrong `uartclk` would make every baud divisor wrong on **both**
`ttyHS0` (MCU) and `ttyHS1` (Bluetooth) — one shared root cause for
both "no MCU handshake response" and "Bluetooth crash", through the
one driver file both ports share. Fixed in `linux-arkmicro` `215c2b36f`
(hardcoded to 24MHz, matching stock exactly).

**Result (retest with uartclk fix, 2026-07-18): FAIL, same crash at
the same point.** Ruled out too. Also observed: `MsnCoreApp` loops on
`AT+BTEN=1` with no response — but note this is likely a *downstream
consequence* of `blueware` already being crashed (`MsnCoreApp`'s
`libBlueTooth.so` writes to `/dev/bw_serial`, a virtual port `blueware`
itself provides, not `/dev/ttyHS1` directly), not necessarily an
independent second data point pointing at something new.

**Status:** three well-evidenced kernel-side theories tried and ruled
out (RXD-clear, uartclk, missing rootfs files). The crash is 100%
deterministic (identical registers every time) regardless of kernel
changes, which increasingly suggests either (a) the chip is receiving
*zero* bytes back at all (not garbled data — genuine silence — and
blueware's crash is what happens when its parser processes a
stale/uninitialized buffer after a timeout with nothing real ever
received), or (b) something earlier and more fundamental than UART
timing. **GPIO91 polarity ruled out without needing a live test**:
`AT+DEVSTAT=0`/`AT+PWRSTAT=1` come back as real, valid responses in
*every* log, both stock and ours — if the enable line's polarity were
wrong the chip wouldn't be powered at all, and those wouldn't succeed
either. The failure is confirmed downstream of chip power-on.

**Root cause mechanism found (2026-07-18, full disassembly of
`blueware` itself):** the crash is a consumer thread (spawned right
after `dlopen("libbt-vendor.so")`) reading a global "pending response"
list head (`0xb8cbc`) that is **never written to anywhere in
`blueware`'s own code** — the only possible producer is a callback the
vendor library registers during its own init. If that callback never
fires, the list stays stale and the first read returns garbage
(exactly matching the deterministic `r0=0xfcee0104` crash).

**Major methodology correction:** the "successful" stock log this
whole investigation treated as ground truth (`realtek selected` /
`openning librtkvnd.so` / `TAG:...` / `set:fwdir=...`) — those two
key strings **do not exist anywhere** in `blueware` or in our
currently-loaded `/lib/libbt-vendor.so` (confirmed byte-identical to
the pristine Prado dump). That log could not have come from a run of
this exact library. The literal name "librtkvnd.so" in that log's own
text is a strong hint the real working device loaded a Realtek-named
library under this same `dlopen()` target — which we already have
sitting at `/usr/lib/librtkvnd.so` (pulled from Holden earlier because
the log named it).

**Fix applied and tested (2026-07-18): FAIL, no progress, same error.**
Replaced `/lib/libbt-vendor.so`'s *content* with `librtkvnd.so`'s
content (committed `c05ef4e`). User reported no change and asked the
right corrective question: is our rootfs actually *consistent* with
Holden's known-working one, file-for-file? Checking Holden's own
`/lib/libbt-vendor.so` directly found it **byte-identical**
(`c3699b4686fa2cb009e902523f77f5c0`) to what we had all along — Holden
ships `libbt-vendor.so` and `librtkvnd.so` as two separate, coexisting
files, not one replacing the other. The swap was based on a flawed
inference. **Reverted**, confirmed matching, committed `bd320f3`.

**Real root cause found (2026-07-18): full rootfs consistency audit.**
Ran a systematic MD5 comparison of every file present in *both* our
rootfs and Holden's rootfs (926 common files, 528 content mismatches —
most are cosmetic: bootlogos, language `.qm` files, launcher variant
`.so`s, openssl man pages). Cross-referencing `etc/version` explains
the pattern: our reconstructed rootfs is built from **Prado's own
2020-12-03 firmware dump**, while Holden's confirmed-working rootfs is
a **2024-02-21 build** — a materially newer firmware release, not just
a different device's rootfs.

This directly explains the Bluetooth crash: our `usr/bin/blueware`
(627376 bytes, MD5 `01615438...`) was the *old* 2020 binary, which only
`dlopen()`s `libbt-vendor.so`. Holden's `blueware` (887648 bytes, MD5
`248cf013...`) is the *new* 2024 binary, which dlopens a full
vendor-plugin chain: `libaicvnd.so`, `libbw151.so`, `librtkvnd.so`,
`libbwvnd.so`, `libbt-vendor.so`. This is exactly why the earlier
"missing vendor library" string search came up empty — the old binary
never had multi-vendor support compiled in at all, so no amount of
rootfs file-shuffling around it could have worked. (The first three
plugin libs don't exist in *either* rootfs — they're just per-chip
dlopen fallback attempts; only `librtkvnd.so` is actually shipped, and
it was already present and correct.)

**Fix applied (2026-07-18), not yet tested:** replaced
`usr/bin/blueware`, `etc/blueware-bw121.properties`,
`etc/blueware-bw123.properties`, and `etc/bluetooth/rtl8821cs_fw` with
Holden's matched 2024-build versions (copied as one set, since they're
version-paired). Verified the new binary is a valid ARM ELF
(`file` confirms dynamically-linked, non-corrupt). The properties diff
from the old file is trivial — just `HFP_RING_PATH=/etc/RingTone.wav`,
which we'd already added the underlying file for earlier this session.
Committed `5bf0bcd`.

- [ ] **Retest Bluetooth with the new `blueware` binary** — watch for
      `openning librtkvnd.so` / `set:fwdir=` / `set:cfgdir=` now
      appearing (these strings exist in the new binary; they never
      existed in the old one, so they could never have appeared no
      matter what rootfs files were present).
- [ ] Watch for the crash disappearing entirely — full HCI bring-up
      ending in `[BLUEWARE:ON][NAME:...]`.
- [ ] **Retest `mcu-handshake` (item 2) too** — separate failure
      mechanism, still worth confirming the `uartclk` fix independently
      now that Bluetooth has a real fix in place.

**Result: FAIL, same crash — even with the correct Holden blueware
binary + matching config/firmware.** This finally ruled out rootfs
content entirely: the crash is not a userspace/vendor-library version
mismatch, it has to be something feeding both HS UART ports from
underneath.

**New root cause found (2026-07-18), kernel-level, not yet
hardware-tested:** direct side-by-side comparison of `ark_hsuart.c`
(serves both `ttyHS0`/MCU and `ttyHS1`/Bluetooth, broken) against its
working sibling `ark_uart.c` (`ttyS0-3`, PL011-derived, functions
correctly) found two real, concrete gaps this time, not log-inference:

1. **The HS UART peripheral clock is never actually turned on.**
   `uap->clk` (`uart4clk`/`uart5clk`) is fetched via `devm_clk_get()`
   in `probe()`, but `clk_prepare_enable()` is never called anywhere
   in the file. `ark_uart.c`'s equivalent does call it. This isn't
   cosmetic — `drivers/clk/arkmicro/clk-sys.c`'s `clk_sys_ops` has
   real `.enable`/`.disable` callbacks that write an actual
   gate-enable bit to a hardware register, not a no-op rate-only
   clock. There was even a dangling `/* Shut down the clock producer
   */` comment in `shutdown()` with no code under it — this driver
   clearly intended to manage the clock and the calls were never
   written. Since `ttyHS0`/`ttyHS1` are only opened by userspace
   (`blueware`, `MsnCoreApp`), well after the kernel's boot-time
   "disable unused clocks" pass runs, the clock could be sitting
   gated off the entire time anything tries to talk to the MCU or
   Bluetooth chip — which would produce exactly this symptom
   (peripheral registers all accessible, IRQs fire, but no real
   bit-accurate serial data ever gets in or out).
2. `ark_hsuart_set_mctrl()` only ever asserted RTS, with no path to
   deassert it — the working driver's `pl011_set_mctrl()` handles
   every modem-control bit symmetrically. Lower-confidence/secondary;
   fixed for parity regardless.

Both fixes committed to `linux-arkmicro` (`33fd16e31`), build clean
(zImage + all 150 modules via `build_kernel.sh`).

- [x] **Retest Bluetooth with this kernel** — **CONFIRMED FIXED at the
      driver level.** `new uboot new kernel baseline v4.txt`: for the
      first time in the entire investigation, `realtek selected` /
      `openning librtkvnd.so` fire, the vendor init callback runs, and
      the chip returns its **real HCI identity**:
      `vendor:hci_ver:0a,hci_rev:000b,lmp_ver:0a,lmp_subver:8761,manu:005d`
      (RTL8761B). The old deterministic crash (`r0=0xfcee0104`, dead
      uninitialized-list read) is gone completely. The missing
      `clk_prepare_enable()` was the real root cause of "zero response
      ever" on both HS UART ports.

**New, different, downstream crash found in the same log — Bluetooth
is not fully working yet.** Right after printing that HCI identity,
`bt_fw_download_thread` (pushing the actual RTL8761B firmware patch to
the chip) crashes — same fault (`pgd = 88aa2e5c`) 4 times in a row in
this log, each time `blueware`'s own watchdog restarts it
(`Restart_MainThread:11` / `SYSTEM_RESET_KILLED:11`) and it loops back
to the same crash ~2.6s later. This is a **new** failure surfaced only
now that the clock fix lets real traffic through — it was never
reachable before. The oops dump in this capture is truncated (missing
the usual PC/register lines, `[00000001] *pgd=...` cut short) — likely
interleaved with `blueware`'s own concurrent stdout. Next step: a
cleaner capture (redirect `blueware`'s stdout to a file so it doesn't
interleave with the kernel oops on the same console) to get the full
register dump for root-causing this one the same way the clock bug was
found.

**Update (2026-07-18) — crash does not reproduce via the real usage
path.** All 4 crashes in the v4 log came from manually running
`killall blueware; /usr/bin/blueware /etc/blueware-bw121.properties`
*before* `start_msn`. Once `start_msn` ran, `MsnCoreApp`'s own
`start bt service:/usr/bin/blueware` (line 895, `t=161.559s`) got a
real `AT+DEVSTAT`/`AT+ADDR` response chain starting just ~200ms later
— far too fast to have gone through even one crash-restart cycle (each
cycle takes ~2.6s in the standalone loop). That instance never
crashed at all; BT Local Address, name-set, HFP config etc. all
succeeded cleanly. **Downgraded from blocking to
manual-testing-artifact** — not worth the risk of a binary patch to
`blueware` unless it's actually observed failing during normal use.
Root-caused via Ghidra regardless (see below) in case it resurfaces.

**Ghidra root-cause of the manual-test crash (for reference):** the
crash is inside `blueware` itself (`FUN_00073594`/`bt_fw_download_thread`
@ `0x73594`, faulting call at `0x738c4`) — an indirect call through a
completion-callback global (`DAT_000f856c`) that's supposed to be
armed via `FUN_000741d4` before each vendor command is sent, but
wasn't armed for whatever response this dispatch is handling,
producing a null-pointer call matching the kernel's `[00000001]`
faulting address. Also added the genuinely-missing
`etc/rtl8761bt_config` (sourced from the official
`Realtek-OpenSource/android_hardware_realtek` repo, `rtk1395`/`rtk1619`
branches — identical 33-byte file on both, main repo's `rtl8761bt_fw`
is an older/smaller build than what we already have from Holden, not
swapped in) — traced to gracefully fall through rather than cause this
specific crash, so unlikely to fix it alone, but correct to have.

- [ ] If this crash is ever observed to actually block Bluetooth
      during normal `MsnCoreApp` use (not just manual testing), capture
      a clean oops (`blueware > /tmp/blueware.log 2>&1 &`, then `dmesg`
      separately once it crashes, so the kernel oops isn't interleaved
      with blueware's own output) before considering a binary patch.
- [ ] **Retest `mcu-handshake` (item 2) with this kernel** — same
      driver serves `ttyHS0`, so the clock fix may unblock MCU
      handshake too now.

**Root cause of "zero response on both HS UART ports": CONFIRMED
FIXED** (missing `clk_prepare_enable()` in `ark_hsuart.c`). Bluetooth
has a new, later-stage, downstream crash in firmware download that
needs its own root-cause pass — real progress, not resolved yet. The
rootfs-version theory is fully ruled out; ignore it going forward.

**CONFIRMED WORKING END-TO-END on real hardware (2026-07-19, `new
uboot new kernel baseline v8.txt`).** Not just chip ID this time — a
real phone paired and connected:
```
[360.699]  Bluetooth connected: "Pixel 9 Pro" "04006EAF29C4" "04:00:6E:AF:29:C4"
[363.196]  recv bluetooth connect phone type: "unkown"
```
The system correctly detected the connected phone and switched into
`MsnCarLife` mode (wireless CarLife/Android-Auto-style projection),
then began trying to join the phone's WiFi hotspot (`link_29c4`) for
the actual projection session — the full real-world chain (BT pair →
phone detect → CarLife handoff) working for the first time in this
project. This is the practical resolution of everything tracked in
this section — the manual-test-only crash (still present, see above)
doesn't matter for real use, exactly as the earlier timing analysis
predicted.

**New, separate, later-stage issue found in the same log:** the
WiFi join to the phone's hotspot never completes — `wpa_supplicant`
initializes fine (`Successfully initialized wpa_supplicant`) but
`WIFIManager network ssid: "link_29c4" is found: false` repeats every
~9s all the way to `391s` (log ends there, still retrying). Worth its
own investigation if CarLife's actual video projection is the next
target — separate from anything in this Bluetooth section.

---

## 4. BD37033 — is the amp stuck muted?

**Theory:** `muteSpeakerAtts()` mutes all channels at startup via I2C;
if the later unmute/volume-set write silently fails (same I2C
pin-sharing issue), the chip stays muted forever — total silence, no
crash, no visible error.

```sh
i2c-dump /dev/i2c-2 0x40 8
```
- [ ] Check byte offset 6 (register 6). Is bit 7 set (value `0x80` or
      higher)? → stuck muted, confirms the theory.
- [ ] Is bit 7 clear? → chip isn't stuck muted, silence is something
      else (worth a fresh look).

**Result (register 6 value):** _____________________

---

## 4b. SoC's own internal DAC mute — new, separate fix

**Change:** `ark_sddac_mute()` (`sound/soc/arkmicro/ark1668-sddac-codec.c`)
was a complete no-op — never wrote the mute/unmute register at all.
Found by noticing repeated `ark_audio_mute` lines in stock's boot log
that don't exist anywhere in ours. This is **upstream** of BD37033 in
the audio chain (SoC's own internal DAC, not the external amp) — even
if BD37033 turns out fine, this could independently explain silence.

- [ ] Confirm audio plays at all now (basic test: any sound source).
- [ ] If item 4 above showed BD37033 NOT stuck muted, but there's still
      no sound, this fix is the more likely explanation — retest 4 vs
      4b independently if both are in play.

**Result:** _____________________

---

## 5. `/proc/arktool` — confirm the new kernel port initialized

**Change:** new `drivers/misc/ark_tool.c`, ported from stock, hooked to
`uart0`'s (console) first open.

```sh
cat /proc/arktool
```
- [x] Confirm it exists at all (didn't before this session).
- [x] Confirm status shows `ready`.
- [x] Confirm both `lcd_base`/`pinmux_base` show non-null mapped
      addresses.
- [ ] `dmesg | grep ark_tool` — confirm the init log line appeared
      early in boot (matches stock's `arktool display reg init` timing,
      before rootfs mount).

**Result:** PASS (2026-07-18) — `ready`, `lcd_base -> 688c8bc0`,
`pinmux_base -> ee9b6f02`, `frames_received: 0` / `checksum_errors: 0`
(expected — no separate source sending frames on ttyS0, which doubles
as the console). `dmesg` timing not yet checked.

---

## 6. Lower priority / optional

- [ ] `i2c-read-raw /dev/i2c-0 0x10` and `0x11` — only if you have
      spare time; last check strongly suggested this is a scanning
      false-positive (no real device), so not worth much time.
- [ ] `fb-alpha-test` — only relevant if item 1's red tint is still
      present after the LCD fix; run it to get the band-by-band
      alpha/channel-order comparison described in its README.

---

## 7. Reversing camera / ITU656 `dvr` capture pipeline (2026-07-19, new)

**Change:** `open dvr device /tmp/dev/dvr failure.` has appeared in
*every single boot log this entire project*, always dismissed as
noise. Traced it to a real, previously-disabled subsystem: stock's
kernel has a full reversing-camera driver (`dvr_rn6752_probe`,
`dvr_detect_carback_signal`, `dvr_enter_carback`/`_exit_carback`, a
complete `/dev/dvr` character device). Our reconstructed kernel
already had the *exact same code already ported* sitting in
`drivers/soc/arkmicro/itu656/` (`rn6752.c` — the I2C camera decoder,
1210 lines; `ark1668_itu656.c` — the capture pipeline + `dvr` char
device, 2269 lines) — just switched off in the kernel config
(`CONFIG_ARK1668_ITU656`/`CONFIG_RN6752` both unset). Confirmed real
and active on stock hardware via three independent signals: a real
`itu656_load.sh` script in the stock rootfs (creates `/tmp/dev/dvr`),
U-Boot's own boot-time `itu656bypinfo check ok` validation of
`arkdata.ini` on Prado's own real dumped log, and a dedicated U-Boot
`itu656` command + `ITU656` register block.

Enabling this surfaced a much bigger, unrelated problem: the
`ark1668_defconfig` re-apply needed to pick up the new config also
dropped `CONFIG_INET`, `CONFIG_IPV6`, `CONFIG_WIRELESS`, `CONFIG_WLAN`,
and all 4 RTL8xxx WiFi driver configs — none of those had ever
actually been captured in the checked-in defconfig, only ever set by
hand in a stale `.config` that kept getting reused without
re-applying defconfig (`build_kernel.sh` skips defconfig application
whenever `.config` already exists). **Recovered and verified** — see
`linux-arkmicro` `5f9fde926` for the full trace (cross-referenced
against stale-but-proven-working `.ko` build artifacts still in the
tree, validated by diffing a from-scratch `.config` regeneration
against the known-working resolved config: identical except
build-timestamp metadata).

Also fixed a real conflict: `CONFIG_ARK7116` (a different, unused
camera decoder chip) was also enabled in the defconfig and defines the
same global symbols as `rn6752.c` — caused a link error when both were
on. Disabled `ARK7116` since RN6752 is the chip actually on this board
(confirmed via the DTS's `dvr_rn6752@2c` I2C node and the already-
succeeding `dvr_rn6752_probe:init done` boot-log line).

DTS wiring already correct, no changes needed: `dvr_rn6752@2c` (I2C,
already probes successfully) and `itu656in@e0800000`'s `compatible =
"arkmicro,ark1668-itu656"` (verified matches `ark1668_itu656.c`'s own
`of_match_table` exactly).

**Update (2026-07-19) — reverted, kernel panics on every boot.**
`new uboot new kernel baseline v6.txt`: with the USB-boot hang fixed
(item 8 below), boot got far enough for `MsnCoreApp` to actually reach
`dvr_ioctl()` for the first time ever — and it panics the entire
system, every single boot, not a graceful failure like before:
```
Unable to handle kernel NULL pointer dereference at virtual address 00000000
PC is at devm_kmalloc+0x74/0x8c
LR is at dvr_ioctl+0x6a0/0x86c
...
Kernel panic - not syncing: stack-protector: Kernel stack is corrupted
```
Traced via `objdump` on our own compiled `vmlinux`: `dvr_ioctl()`
reads a function pointer from `dvr_dev->start`/`->stop` (byte offset
`0x200`) and calls through it guarded only by a non-NULL check.
`dvr_dev` is `devm_kzalloc()`'d (should be all-zero unless explicitly
written), but at panic time that field held a value that happened to
decode to `devm_kmalloc`'s real kernel address — called with garbage
arguments, NULL deref, corrupted stack, panic. A real memory-
corruption bug somewhere in ~2269 lines of decades-old vendor code,
not something the config flag alone fixes.

Tried disabling just `ARK1668_ITU656` — doesn't link. `RN6752` and
`ARK_CARBACK` both reference symbols defined only inside
`ark1668_itu656.c`; the three are one linked unit, not independently
toggleable as currently structured. **All three disabled together**
(`linux-arkmicro` `3a8e2568a`), back to the exact stable state that
predated enabling this — validated the same way as the other config
fixes (fresh defconfig regeneration diffed against known-good state,
identical except metadata).

- [x] ~~Flash this kernel/DTB...~~ — superseded, reverted before
      further hardware testing of this specific feature.
- [ ] **Properly re-enabling this needs a dedicated investigation**
      into what's actually writing garbage into `dvr_dev->start`/
      `->stop` before touching this config again — not a config
      change, a real source-level bug hunt (likely another Ghidra
      pass, this time on our *own* driver rather than stock's).
- [ ] **Still worth confirming**: WiFi AP (`hostapd`) and DHCP work
      exactly as before now that ITU656/RN6752/ARK_CARBACK are back
      off — this is the subsystem that was at risk during the earlier
      defconfig recovery (item 7 above, still applies).

---

## 8. `bootusb` hang — USB stick as root, fixed (2026-07-19)

**Symptom:** `new uboot new kernel baseline v5.txt` hung forever at
`Waiting for root device /dev/sda2...`. This boot mode (root on a USB
stick, no initramfs) needs the USB host controller driver built into
the kernel image itself — it structurally cannot work as a loadable
module, since nothing can load a module from a filesystem that needs
that same module to become reachable in the first place.

**Root cause:** `CONFIG_USB_MUSB_HDRC`/`CONFIG_USB_MUSB_ARKMICRO` were
`=m` in the checked-in defconfig (since its very first commit), but
every prior successful boot log through v4 — all using this same
`bootusb` path — shows `musb-hdrc`'s own init lines printing
synchronously during early kernel boot, well before "Waiting for root
device", which only a built-in driver can do. Confirms these had been
live-patched to `=y` in some past session's `.config` that was never
captured back into the defconfig, and got silently lost the moment
`--defconfig` was force-reapplied for the ITU656 work above (see the
`linux-arkmicro` README's new "`.config` vs `ark1668_defconfig`"
section for the full pattern and why this keeps happening).

**Fixed** in `linux-arkmicro` `db0d63877`, validated the same way as
the other config fixes.

- [x] **Confirmed fixed by the user directly** — `bootusb` boots again
      (2026-07-19).

---

## 8b. No real sound card — fixed and confirmed working (2026-07-19)

**Symptom:** `new uboot new kernel baseline v9.txt`: `ALSA device
list: #0: Dummy 1` — only the dummy placeholder ever registered, no
real sound card. Broken since as early as v7 (same symptom present
there too), just not noticed until now. v4's log, by contrast, shows
a real `#0: ARK-SDDAC` card.

**Root cause:** `CONFIG_SND_SOC_ARK` and everything under it
(`SND_SOC_ARK1668_I2S`, `SND_SOC_ARK1668_ADC`/`DAC`,
`SND_SOC_BD37033`) were never captured in the checked-in defconfig at
all — a **fifth** instance of the same `--defconfig`-regeneration
pattern from 2026-07-19. Without this, the whole audio path was
compiled out entirely — the ark1668 I2S driver, the internal DAC
`ark_sddac_mute()` fix from `1c3e87e50`, and the external BD37033 amp
`MsnCoreApp`'s `Sound_BD37033::muteSpeakerAtts` talks to.

**Update: `95783755f` alone wasn't enough — real evidence found on the
next fresh-boot test.** With that fix in, the I2S driver now genuinely
probes (soft-reset, clock-gate setup all succeed) but then fails:
```
ark1668-i2s e4000000.i2s-dac: Could not register PCM
ark1668-i2s e8200000.i2s-adc: Could not register PCM
```
Traced to `devm_snd_dmaengine_pcm_register()` needing a working DMA
channel from `dwdma0` (the I2S DTS nodes' `dmas=` target, compatible
`"arkmicro,ark-dma"`) — `CONFIG_ARK_DMA` (the actual driver for that
compatible string, `drivers/dma/ark-dma.c`) was **also** never
captured in the defconfig — a sixth instance of the same pattern.
Fixed in `linux-arkmicro` `b0bff0082` (also disabled `CONFIG_DW_DMAC`,
the unrelated generic Synopsys DMA driver — nothing in this board's
DTS uses it, and it was link-conflicting with `ark-dma.c` over
several identically-named symbols). Validated the same way as the
other six fixes today.

**Update: `b0bff0082` also wasn't the end of it.** "Could not register
PCM" was gone on the next test, but a new failure appeared right
after:
```
drv_bd37033 2-0040: bd37033_write_bytes: i2c_transfer failed
drv_bd37033 2-0040: bd37033_write_byte: i2c_transfer timeout
```
**Confirmed by the user this I2C failure is pre-existing on stock
firmware too** — not something this reconstruction introduced, and
never fixable. So the real question was why stock still registers
`#0: ARK-SDDAC` despite it. Answer: `ark1668_limcet_p305.dts` wired
BD37033 as a hard `simple-audio-card,aux-devs` dependency (added
2026-07-14 to expose `PA Volume`/`PA Mute` mixer controls — see
`docs/AUDIO_SUBSYSTEM_INVESTIGATION.md`), and `soc_probe_aux_devices()`
(confirmed in `sound/soc/soc-core.c`) aborts the **entire** card's
registration if any aux-dev's `probe()` fails — which BD37033's does,
via a real `i2c_transfer()` call. Removed the aux-dev wiring and
disabled `CONFIG_SND_SOC_BD37033` entirely, in `linux-arkmicro`
`fe7198962`. Loses `PA Volume`/`PA Mute` mixer exposure (which never
worked reliably anyway per the 2026-07-14 note), but basic playback/
capture through `ARK-SDDAC` never depended on BD37033 at all — that's
a separate external amp `MsnCoreApp`'s own userspace path already
talks to independently.

**Update: `fe7198962` still wasn't the end of it — 8th instance found
2026-07-19 during a proactive audit.** After the user reported "many of
our commits from yesterday didn't stick," cross-checked every
`CONFIG_` symbol mentioned across `docs/*.md` against the checked-in
kernel `.config`/defconfig. `CONFIG_SND_SIMPLE_CARD` (the
`sound/soc/generic/simple-card.c` machine-driver framework that
`ark1668_limcet_p305.dts`'s `compatible = "simple-audio-card"` sound
node actually binds to) was disabled — without it, none of the three
fixes above matter, since the DTS sound node has no driver to bind to
at all and `ARK-SDDAC` never registers, full stop. Fixed in
`linux-arkmicro` `a88acc9e0`, validated the same way as the other
seven fixes today. Also re-verified (this session) that all seven
prior fixes (INET/IPV6/WIRELESS/WLAN, MUSB, ARK_DISPLAY, SND_SOC_ARK,
ARK_DMA, TOUCHSCREEN_ARK1680) are still present in the current
checked-in defconfig — none had silently dropped.

- [x] Flash this kernel. Confirm `ALSA device list: #0: ARK-SDDAC` (or
      similar real card name) appears in dmesg, not `Dummy`. Confirmed
      via `new uboot new kernel baseline v11.txt`:
      `[ 2.020195] ALSA device list: [ 2.025889]   #1: ARK-SDDAC`, no
      `"Could not register PCM"` and no BD37033-caused failure.
- [x] Confirm actual audio playback works. Confirmed via
      `audio-test.sh`'s static-noise playback on `hw:1,0` — user
      reported audible static and correct device enumeration.
      (`audio-test.sh`'s BD37033 mixer-control check still fails, as
      expected — that's the pre-existing/unfixable I2C issue from
      earlier in this section, not a regression.)

**Update: `CONFIG_SND_DUMMY` disabled (2026-07-19, `f54d66515`).** Now
that `ARK-SDDAC` is real and confirmed working, the `Dummy` placeholder
card was removed so `aplay -l` only shows the real device.

---

## 8c. `usb0` `dr_mode="host"` attempt — reverted (2026-07-19)

Tried fixing the multi-second USB boot-time retry cycling (item 8's
neighbor symptom — `"Cannot enable... attempt power cycle"` on
`usb0`) by setting its DTS `dr_mode` to `"host"` instead of `"otg"`,
skipping the ID-pin negotiation entirely for boot. **Made things
worse and was reverted** — see `docs/WIRELESS_AND_INIT.md` §7 for the
full writeup. Short version: `usb0` itself registered perfectly
cleanly, but the USB boot stick (confirmed same physical port every
time) stopped enumerating there and moved to `usb1` instead, going
through the same cycling and this time hanging permanently instead of
eventually recovering.

Reverted in `linux-arkmicro` `d44cce385` — both `usb0`/`usb1` back to
`dr_mode="otg"`, the proven-working state. `switchotg.sh`'s fix
(correct sysfs paths, root-mounted-on-usb0 safety guard, main repo
`9aa0fec`) was **not** reverted — still valid regardless.

- [x] **Confirmed by the user directly that the revert restores the
      known-working boot behavior** (same physical port, same
      cycling-then-eventually-works pattern as every log before this
      attempt).
- [ ] **Not re-attempted.** Root cause of the regression is genuinely
      not understood — needs real investigation (why does `usb0`'s
      own dr_mode affect where `usb1` enumerates a device?) before
      trying `dr_mode="host"` again, not just flipping the value.

---

## 9. `/dev/ark_display` missing — fixed (2026-07-19)

**Symptom:** `new uboot new kernel baseline v7.txt` showed
`open /dev/ark_display fail` repeating throughout `MsnCoreApp`'s log —
absent in v4, which instead shows `ark_display: registered
/dev/ark_display` and successful `ARKDISP_GET_SCREEN_INFO`/
`ARKDISP_GET_SET_VDE_CFG` calls.

**Root cause:** `CONFIG_ARK_DISPLAY` was never captured in the checked-in
`ark1668_defconfig` at all — a **fourth** instance of the same
`--defconfig`-regeneration pattern from today (INET/WLAN, MUSB,
RN6752/ITU656/ARK_CARBACK). Missing from every kernel build since,
including the one with `fd6e03414`'s `ARKDISP_SET_VDE_CFG`
real-register-write changes — **those have never actually run on
hardware yet**, since the device never existed for `MsnCoreApp` to
open. Fixed in `linux-arkmicro` `71d16e73e`.

**Also possibly explains v7's touch failure** (`Open touch input event
failed!`, fd=-1, vs v4's clean success) — v4's working touch open
directly coincided with `ark_display` working; v7's failure directly
coincided with it missing. Not confirmed as the same root cause yet,
but touch is worth retesting fresh alongside this fix rather than
treating it as a separate, new regression.

- [ ] Flash this kernel. Confirm `ark_display: registered
      /dev/ark_display` appears in dmesg, and `open /dev/ark_display
      fail` is gone from `MsnCoreApp`'s log.
- [ ] Retest touch (item 2/6 above) now that `ark_display` is back —
      see if it starts working again on its own.
- [ ] Try adjusting contrast/brightness/saturation in the UI (if
      exposed anywhere) and confirm it now has a real visible effect —
      this is the first real hardware test of `fd6e03414`'s register
      writes.

---

## If anything regresses

All of tonight's changes are two clean commits:
- Main repo: `96eef2e` (mcu-handshake), `d559908`/`ec03977` (Bluetooth),
  `c6a3d48`/earlier (docs, tools)
- `linux-arkmicro`: `3bc02e43e` (arktool), `a8e3ecc77` (LCD/carback/gt911)

Nothing here is a guess-and-hope change — every fix has a disassembly-
confirmed root cause recorded in the corresponding doc
(`docs/WIRELESS_AND_INIT.md` §6, `docs/ARK1680_TS_REVERSE_ENGINEERING.md`,
`tools/mcu-handshake/README.md`, `docs/BD37033.md`) — if something looks
wrong, start there rather than re-guessing from scratch.
