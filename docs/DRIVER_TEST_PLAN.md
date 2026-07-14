# Driver test plan — peripherals not yet confirmed working (2026-07-14)

Companion to `PIN_MASTER_LIST.md`'s "Driver source reference" table. That
table's "Confirmed by operation" column has several `PROBES OK` /
`NOT CONFIRMED` rows — this doc is the concrete plan to actually resolve
each one, one heading per peripheral.

**Ground rule, carried over from the 2026-07-13 incident (see
`PIN_MASTER_LIST.md`'s "Open items"): prefer software-only observation
before any live electrical/pin-level test.** Every plan below is ordered
from lowest-risk (read a log, read a sysfs file, run existing tooling
against an already-wired peripheral) to highest-risk (driving new pins).
Don't skip ahead to a risky step if an earlier one hasn't been tried yet.
And per the boot-log caveat now in `PIN_MASTER_LIST.md`: a clean probe
message is not a pass condition for any of these — only a real functional
observation (data received, signal changes, ACK response) counts.

---

## Automated test scripts (2026-07-14)

Five POSIX shell scripts now automate the mechanical parts of the plans
below — copy the whole `tools/` directory onto the device and run
directly from the live root shell. Each prints `[PASS]`/`[FAIL]`/
`[UNKNOWN]` per check and a summary line; each README explains exactly
what a pass does and doesn't prove. **None of these replace the manual
reasoning below** — a clean run is a starting point, not a substitute
for the "what would actually prove this works" judgment calls in each
section (per the boot-log-evidence caveat: a script reporting `[PASS]`
is still just a mechanical check, not a functional guarantee).

| Script | Covers | Notes |
|---|---|---|
| `tools/touch-selftest/touch-selftest.sh` | Section 1 (resistive touch) | needs a real physical touch during two timed windows |
| `tools/uart-test/uart-test.sh` | Sections 2-3 (MCU link + MSNEry link) | passive-only; auto-tries fallback bauds |
| `tools/audio-test/audio-test.sh` | Section 6 (BD37033 control path) | can't listen for you — only confirms the mechanical steps around an audible test |
| `tools/bt-test/bt-test.sh` | Bluetooth (already CONFIRMED in `PIN_MASTER_LIST.md`) | regression check + traffic monitor; targets `blueware`/`ttyHS1`, not `rtkbt` |
| `tools/usb-test/usb-test.sh` | USB (already CONFIRMED) | regression check against the known-working boot-log baseline |
| `tools/mmc-test/mmc-test.sh` | MMC (mmc0 CONFIRMED; mmc1 = Section 8) | **read-only by design**, given the 2026-07-13 incident |

None of these needed a `timeout` applet — this project's busybox build
doesn't have one, so timed windows use background process + `sleep` +
`kill` instead. Worth knowing if you write more of these. Also worth
knowing: `strings`' default 4-character minimum silently hides short
applet names (`dd`, `wc`, `cat`, `od`, ...) when checking what a busybox
build supports — use `strings -n 2` (or lower) to check for these, a
mistake caught and corrected while building `uart-test.sh`.

---

## 1. Resistive touch (`ark1680_ts`) — physical touch input

**Why uncertain:** driver registers cleanly (`input: ark1680-ts as
.../input0`, `irq=20`) but no evidence this session that a physical touch
actually produces input events on this exact kernel build.

**Goal:** confirm `ABS_X`/`ABS_Y`/`BTN_TOUCH` events actually arrive when
the panel is touched.

**Test steps (already-built tooling exists — `tools/ark1680-ts-test/`,
lowest risk first):**
1. `ark-ts-test regs` — dumps the ADC/TSC block directly via `/dev/mem`,
   independent of the driver. Touch the panel while re-running it a few
   times; `raw_x`/`raw_y` should visibly change. **If they never move,
   the fault is upstream of software** (pinmux/clock bits, or the panel
   isn't actually wired/seated) — stop here, this isn't a driver bug.
2. `dmesg | grep ark1680_ts` — confirm probe succeeded, review the
   driver's own probe-time register dump.
3. `echo 1 > /sys/module/ark1680_ts/parameters/debug` then `dmesg -w` —
   turn on per-IRQ tracing (raw samples, filtered coordinates,
   stable/unstable verdict) while touching the panel.
4. `ark-ts-test events /dev/input/eventN` (find N via
   `cat /proc/bus/input/devices | grep -A5 ark1680-ts`) — confirms
   userspace actually receives real evdev events.

**Pass condition:** step 4 prints `ABS_X`/`ABS_Y`/`BTN_TOUCH`/`SYN_REPORT`
events that track real finger position during a physical touch.

**Record findings in:** `docs/boot_experiment_log.md` (this tool's own
README asks for this) and update `PIN_MASTER_LIST.md`'s status row.

---

## 2. MCU UART link (`uart4`, `/dev/ttyHS0`)

**Why uncertain:** the tty node is confirmed to exist (`ttyHS0 at MMIO
0xe4f00000 (irq=38)... is a ARK HS UART`), but no evidence this session
of actual MCU protocol traffic (key events, `recv track:`, status frames)
flowing over it on this kernel build.

**Goal:** confirm two-way traffic with the real STM32 MCU using the
documented `BoxP300` protocol (`MCU_ADAPTERS.md`).

**Test steps (lowest risk first — this is a read of an existing wired
link, not a new pin):**
1. `killall MsnCoreApp` — the app holds `MCUPortName`/`MSNEryPortName`
   open at runtime (confirmed via `grep -a` finding both literal strings
   in `libMcuCenter.so`, 2026-07-14 — these aren't vestigial keys), so
   stop it first to free the port for a passive listener. **Not `rcS`,
   as previously assumed** — the actual respawn mechanism was an
   unconditional `MsnCoreApp -qws&` in `/etc/profile` firing on every new
   login shell, now commented out in this project's own reconstructed
   rootfs (`.../rootfs/etc/profile`, 2026-07-14) specifically so this
   kind of test isn't fought by it. On that rootfs, `killall` is now
   durable; on stock/unmodified `/etc/profile`, a fresh login session
   would still relaunch it.
2. Confirm the device node exists: `ls -l /dev/ttyHS0`.
3. **Passive listen first** — since the MCU is documented to send
   periodic/idle status frames regardless of host activity:
   ```sh
   busybox microcom -s 115200 /dev/ttyHS0        # try 38400 if garbage
   # or, to capture to a file while watching hex:
   busybox cat /dev/ttyHS0 | busybox hexdump -C | tee /data/ttyHS0.log
   ```
   Watch for any bytes at all before touching anything physical — this
   alone confirms whether the UART is electrically live and the MCU is
   transmitting.
4. If frames appear, cross-reference the `0x2E` header + command byte
   against the `BoxP300` command table in `MCU_ADAPTERS.md` to confirm
   they parse as valid frames, not noise.
5. If step 3 is silent, toggle one physical input at a time (reverse
   gear, a steering-wheel button, ACC on/off — the exact procedure
   `MCU_ADAPTERS.md`'s "Capturing the codes live on the device" section
   already lays out) and watch for a frame appearing in response.
6. Only once raw frames are confirmed: enable the app-level debug flag
   (`touch /data/mcudebug_flag`, restart `MsnCoreApp`) to see the
   library's own parsed interpretation, per `MCU_ADAPTERS.md` Method A.

**Pass condition:** step 3 or 5 shows real `0x2E`-prefixed frames with
sane checksums — proves the physical link and MCU firmware are alive.
Full protocol correctness (right command decode) is a separate, later
question from "is the wire even working."

**Note:** don't conflate this with the Limcet activation-gate question —
per `project_limcet_activation_gate` memory, that switch works independent
of Linux/MsnCoreApp, so it's not expected to block this test.

---

## 3. MSNEry link (`/dev/ttyS2`) — unidentified secondary peripheral

**Why uncertain:** unlike the `TOUCHSERIAL`/`COMMANDSERIAL=/dev/ttyS2`
env vars in `/etc/profile` (already confirmed dead/vestigial — referenced
by no binary), `MSNEryPortName="/dev/ttyS2"` **is** a real, live-read
config key — `libMcuCenter.so` references the literal string
`MSNEryPortName` (confirmed via `grep -a`, since a plain `grep -rl`
without `-a` silently skips binary matches — don't repeat that mistake).
What's actually on the other end of this link ("MSN Ery" — meaning
unknown) has never been identified. This is a distinct, ordinary UART
(`ttyS2`), not part of the hsuart pair — don't confuse it with the MCU
link.

**Goal:** identify whether anything is actually connected to this link,
and if so, what.

**Test steps (same passive-first approach as the MCU link above):**
1. `killall MsnCoreApp` (same reasoning as above — this app is the one
   confirmed to reference this port).
2. `ls -l /dev/ttyS2`.
3. Passive listen, baud unconfirmed — try 115200 first, then
   9600/19200/38400:
   ```sh
   busybox microcom -s 115200 /dev/ttyS2
   busybox cat /dev/ttyS2 | busybox hexdump -C | tee /data/ttyS2.log
   ```
4. If silent at every baud, toggle physical inputs one at a time while
   watching **both** `ttyHS0` and `ttyS2` simultaneously (two sessions,
   or background one to a log file with `tee`) — this disambiguates
   which physical events route to which link, in case "MSN Ery" turns
   out to be a second, distinct signal source rather than nothing at all.

**Pass condition:** any real traffic identified and attributed to a
specific source — even "confirmed silent/unused" is a useful, valid
outcome here, since the goal is identification, not proving a feature
works.

---

## 4. `ark_carback` (reverse-gear trigger, GPIO 5)

**Why uncertain:** no `carback` probe line found in the latest boot log
at all — unclear if the driver is even binding, separate from whether
the trigger itself works.

**Goal:** confirm the driver binds, and that toggling the physical
reverse-signal line produces the expected IRQ/event.

**Test steps (lowest risk first — GPIO 5 wiring itself is already
triple-confirmed, so this is testing the *signal*, not searching for the
pin):**
1. `dmesg | grep -i carback` — confirm whether the driver even attempts
   to bind this boot. If nothing at all appears, check whether
   `CONFIG_ARK_CARBACK` (or its actual Kconfig symbol —
   grep `drivers/soc/arkmicro/Kconfig` in the build tree) is enabled in
   `Limcet Hardware/kernel_dot_config`, and whether the DTS node
   (`carback@0` in `ark1668-limcet-prado.dts:16-22`) is actually reaching
   the kernel (i.e. this exact DTS was the one built/flashed).
2. `cat /sys/kernel/debug/gpio | grep -i carback` (or equivalent) to see
   if GPIO 5 shows as claimed by this driver specifically.
3. `cat /dev/carback` or whatever the driver's chrdev interface exposes
   (see `ark_carback_probe`'s `device_create(..., "carback")` — check
   `/dev/carback` exists) — read it while toggling reverse gear (or
   simulating the signal safely, e.g. via the car's reverse-light
   circuit if accessible, not by driving GPIO 5 directly from software).
4. `dmesg -w` while toggling reverse gear — watch for the interrupt
   handler firing (`ark_carback_intr_handler`) and any log line it emits.

**Pass condition:** step 4 shows a real IRQ/event correlated with
physically engaging reverse gear.

**Safety note:** don't drive GPIO 5 as an output from software to
"simulate" the signal — it's wired as an input from a real vehicle
signal; forcing it out is unnecessary risk for a pin already confirmed
correct. Test with the real signal source.

---

## 5. `rn6752` camera decoder

**Why uncertain:** only the recurring `### rn6752_eq_work reset` boot-log
line was found — no clear probe-success message, and no confirmation the
actual camera video path works.

**Goal:** confirm the decoder is alive on the bus and that video actually
reaches the framebuffer/display pipeline when a camera is attached.

**Test steps (lowest risk first):**
1. `dmesg | grep -i rn6752` — full context around the `eq_work reset`
   line; check for any accompanying probe success/failure message this
   session's earlier grep may have missed.
2. `tools/i2c-scan/i2c-scan` against `i2c-gpio1` (the bus this device is
   confirmed to live on, addr `0x2c`) — confirms the chip still ACKs at
   all, independent of the driver. This tool already exists and is
   built for exactly this kind of bus-presence check.
3. If it ACKs but the driver isn't binding, check whether the DTS node
   (`dvr_rn6752@2c`, `ark1668-limcet-prado.dts:55-60`) and its
   `reset-gpio`/`carback-config` properties match what
   `drivers/soc/arkmicro/itu656/rn6752.c` expects (grep that driver's
   `of_property_read_*` calls against the DTS property names).
4. With a real AHD camera attached to the reverse-camera input, trigger
   reverse gear (which should also engage `dvr_enter_carback()` per the
   `ark_carback_probe` decompile from `PIN_MASTER_LIST.md`) and check
   whether video appears on `/dev/fb0` (`tools/lcd-test/lcd-test info`
   first, to confirm the framebuffer itself is in a sane state) or
   through whatever userspace path displays the camera feed.

**Pass condition:** step 4 shows real camera video, not just a bus ACK.

---

## 6. `BD37033` audio codec — I2C control path

**Status (2026-07-14): two separate bugs found, one fixed.**
`tools/audio-test/audio-test.sh` and every boot log captured on
2026-07-14 (`new kernel bootlog new uboot usb probe v12.txt`,
`v13.txt`) showed no `PA Volume`/`PA Mute` mixer control and
`bd37033_write_byte timeout` at probe time on the DTS-corrected
`i2c-gpio-1` bus (GPIO9/GPIO121, addr `0x41`).

1. **Missing mixer control — root-caused and fixed.** The `sound`
   simple-audio-card node never referenced `&amp` (`drv_bd37033`) in
   any dai-link or `aux-devs` list, so its `PA Volume`/etc controls
   (registered unconditionally by `bd37033_drv_probe()`, regardless of
   I2C success) never got bound into the card and so never reached
   `amixer`. Fixed by adding `simple-audio-card,aux-devs = <&amp>;` —
   see `AUDIO_SUBSYSTEM_INVESTIGATION.md`. Compiles clean via `dtc`, not
   yet kernel-rebuilt/hardware-tested.
2. **Write timeouts — root-caused and fixed, 2026-07-14: wrong I2C
   address.** The DTS had `reg = <0x41>`, but disassembly of the
   vendor's own shipped userspace audio-control code
   (`Sound_BD37033::Sound_BD37033()` in `libMsnSound.so` +
   `arki2c_open()` in `libMsnCommons.so`) shows the real runtime control
   path uses `ioctl(fd, I2C_SLAVE, 0x40)` — address `0x40`, matching the
   BD37033 datasheet's public address. `0x41` was only ever supported by
   disassembling *stock's kernel* board file plus an `i2c-scan` `XX`
   marker, neither of which proves the chip answers there. Fixed:
   `reg = <0x40>` (`drv_bd37033@40`) in `ark1668_limcet_p305.dts` and
   the docs-repo reference copy. Kernel DTB rebuilt clean, not yet
   hardware-tested. Basic I2S audio output (raw data path, no codec
   control) is still confirmed working independently of this.

**Goal:** flash and boot-test with the corrected address — confirm
`bd37033_write_byte timeout` disappears from `dmesg`, then confirm
`amixer sset 'PA' <n>` produces both a clean I2C transaction and an
actual audible volume change.

**Test steps (lowest risk first):**
1. `dmesg | grep -i bd37033` — check if the driver attempts to bind at
   all this boot; if nothing appears, check the same Kconfig/DTS-match
   question as `rn6752` above.
2. `tools/i2c-scan/i2c-scan` against `i2c-gpio2` (`sda_pin=9,
   scl_pin=121`, addr `0x41`) to confirm the chip still ACKs.
3. If a userspace mixer control exists for this codec (check
   `amixer scontrols` / `amixer contents` if `alsa-utils` is present, or
   whatever this rootfs's volume-control mechanism is), change volume
   while audio plays and listen for an actual audible change — the I2S
   path already confirmed working means you'll hear *something*
   regardless, so the real test is whether volume/tone changes have any
   audible effect, proving the I2C control link works.
4. If no mixer control exists yet, this may need a small test write via
   the driver's own sysfs/debugfs interface (check
   `sound/soc/arkmicro/BD37033.c` for one) or a userspace `i2cset`-style
   tool (not currently in `tools/` — would need building, same pattern
   as `i2c-scan.c`).

**Pass condition:** step 3 (or 4) shows an audible change correlated with
an issued I2C register write — proves the control path, not just the
data path.

---

## 7. `/dev/ark_display` misc shim — full `MsnCoreApp` stability

**Why uncertain:** the shim itself loads (`ark_display: registered
/dev/ark_display`) and fixes the specific ioctl (`ARKDISP_GET_SCREEN_INFO`)
it targets, but whether this fully unblocks `MsnCoreApp` end-to-end
(no more segfault, full UI runs) wasn't separately re-verified this
session.

**Goal:** confirm `MsnCoreApp` starts and runs without the segfault this
shim was written to fix.

**Test steps (lowest risk first):**
1. `lcd-test info` (`tools/lcd-test/`) — confirms `/dev/ark_display`
   replies to `ARKDISP_GET_SCREEN_INFO` with sane values, independent of
   `MsnCoreApp`.
2. Run `MsnCoreApp` directly from a shell (not via `rcS`) so its stdout/
   stderr is visible: `killall MsnCoreApp; cd <app dir>; ./MsnCoreApp
   2>&1 | tee /data/msn.log` — watch for the segfault this shim was
   meant to prevent.
3. If it no longer segfaults at that specific point, use `tools/strace/`
   (already built, see its README) to confirm `arkapi_get_screen_info()`
   completes and the app proceeds past `onFirstInit()` into normal
   operation, not just past the one crash site into a different one.

**Pass condition:** step 2 shows `MsnCoreApp` reaching a stable running
UI state, not just avoiding the one specific crash.

---

## 8. `mmc1` — identify actual purpose

**Why uncertain:** DTS comments it as "SDIO WiFi Controller," but WiFi is
now confirmed to be the USB RTL8811CU instead — so `mmc1`'s real role on
this hardware is currently just a stale/wrong comment, not a known fact.

**Goal:** determine what, if anything, is actually attached to `mmc1`.

**Test steps (lowest risk first — this is investigation, not a "make it
work" task, since we don't yet know what it should do):**
1. `dmesg | grep -i mmc1` (full context, not just the bus-speed
   negotiation lines already seen) — check for any device
   enumeration/card-detect message.
2. `ls /sys/class/mmc_host/mmc1/` and `cat
   /sys/class/mmc_host/mmc1/mmc1:*/type` if a child device node exists —
   reveals SDIO vs SD vs nothing attached, without touching hardware.
3. Check the stock 3.4 board file (same `vmlinux.elf` disassembly
   technique used throughout `PIN_MASTER_LIST.md`) for what stock
   registers on its second MMC controller — the same "reproduce stock's
   ground truth from the binary, no live risk" method already validated
   this project this week.
4. Cross-reference against a schematic/board photo if one becomes
   available, since this is exactly the kind of question static analysis
   plus live (non-probing) log inspection can fully answer without new
   physical risk.

**Pass condition:** a definitive answer for what's wired to `mmc1` (even
if the answer is "nothing, dead code" — that's a valid, useful outcome
too) — this heading is about closing an open question, not proving a
feature works.

---

## 9. GPIO 95 `apple_encpy_ic_rst` — confirm chip presence (not a driver test)

**Why included here:** not a driver-readiness question like the others,
but the same open item from `PIN_MASTER_LIST.md` — worth planning
alongside these since resolving it prevents wasted future effort.

**Goal:** determine whether a CarPlay/MFi authentication chip is actually
populated on this hardware revision, same as was done for the IR sensor.

**Test steps (lowest risk first):**
1. Physical inspection / board photo review, if available — the same
   kind of direct confirmation that settled the IR sensor question.
2. If a chip is confirmed present, cross-check GPIO 95 (`PBANK_2` offset
   31) against the pinctrl DTS's other `PBANK_2` groups (pwm1-3, i2s1)
   for a mux conflict, per the open item already in `PIN_MASTER_LIST.md`.
3. Only pursue driver/DTS work for this chip once presence is confirmed
   — don't build support for hardware that may not exist, per the same
   lesson learned from IR.

---

## Cross-references

- `PIN_MASTER_LIST.md` — the "Driver source reference" table this plan
  responds to, and the pin-safety incident history behind the
  "software-first" testing order used throughout this doc.
- `PIN_BLOCK_DIAGRAM.txt` — quick-reference companion.
- `MCU_ADAPTERS.md` — `BoxP300` protocol detail for the MCU UART test.
- `ARK1680_TS_REVERSE_ENGINEERING.md` — prior touch findings (mostly
  gathered on stock firmware, not this kernel — see test 1 above).
- `tools/i2c-scan/`, `tools/ark1680-ts-test/`, `tools/lcd-test/`,
  `tools/strace/`, `tools/gpio-i2c-probe/` — existing test tooling this
  plan reuses rather than duplicating.
