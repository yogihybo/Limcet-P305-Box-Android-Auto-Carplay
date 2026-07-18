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

- [ ] **Retest Bluetooth with this kernel** — watch for the crash
      disappearing, `openning librtkvnd.so` / vendor init actually
      completing, full HCI bring-up.
- [ ] **Retest `mcu-handshake` (item 2) with this kernel too** — same
      driver serves `ttyHS0`, so this could be the shared root cause
      for both failures after all.

**Root cause: was open, now has a second, more mechanistic
kernel-level fix pending hardware confirmation** (the rootfs-version
theory above is now ruled out — keep that context, but this is the
active lead). If this doesn't resolve it, note that our *entire*
rootfs is running ~3 years behind Holden's confirmed-good build — a
broader "which other files are actually version-critical vs cosmetic"
pass may still be needed separately, not just Bluetooth-specific
files.

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
