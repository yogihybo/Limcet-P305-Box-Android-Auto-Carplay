# Audio Subsystem Investigation

**Status:** Reference
**Last Updated:** 2026-07-15

## Overview

Tracks getting sound working on the 4.19.192 kernel port: the `SoundAdapter`
crash root cause, the kernel driver chain that had to be enabled, DTS
corrections (some based on live hardware, some based on stock-kernel
disassembly), and live test results.

## Background: how this started

`MsnCoreApp` was crashing at `SettingWindow::sendSoundData()+0xc4`
(`NULL+8` dereference) partway through Settings UI init. A vendor-supplied
crash minidump (`/data/minidump/err-9.log`) gave a fully symbolized call
stack pinning the fault to `libSetting.so`, called from
`SettingWindow::onFirstInit()` → `SettingWindow::SettingWindow()` →
`SettingPlugin::createMainWindow()`.

The terminal log showed the real trigger immediately before the crash:

```
SoundAdapter Create Failed, Not Support ICType: 0
Load App Plugin 403 "/usr/lib/libMsnSound.so"
```

`SoundAdapter::getInstance()` (in `libMsnSound.so`) never null-checks
whether the sound-IC construction succeeded — if `ICType` isn't one of a
handful of hardcoded values, it falls straight through to the "Not
Support" failure path, returns garbage, and the caller dereferences it.

## Root cause chain

1. `ALSA device list: #0: Dummy 1` — no real ALSA sound card ever
   registered, only the kernel's dummy card.
2. `msnprofile/MsnProductInfo.ini` had `SoundType=0`, which was never a
   valid value to begin with (see the `SoundICType` switch table below).
3. `SettingWindow::onFirstInit()` calls `sendSoundData()` regardless of
   whether `SoundAdapter::getInstance()` actually returned something
   usable → crash.

### `SoundICType` switch table (decoded from `libMsnSound.so`)

Disassembly of `SoundAdapter::getInstance(SoundICType)` gives the full
dispatch:

| `SoundType` value | Class constructed | IC |
|---|---|---|
| 2 or 4 | `Sound_PT2312` | Princeton PT2312 |
| 3 | `Sound_BD37033` | Rohm BD37033 |
| 5 | `Sound_MCU` | MCU-based audio control |
| 128 (0x80) | `Sound_MCU_OnlyEQ` | MCU-based, EQ only |
| anything else (incl. 0) | — | falls into "Not Support" failure path |

## Kernel driver chain that had to be enabled

None of this was enabled in the 4.19.192 `.config` going into this
investigation. Each layer was found missing one at a time by testing,
not all at once:

1. **`CONFIG_SND_SOC_ARK1668_I2S`, `_ADC`, `_DAC`** — the ARK1668 I2S
   controller + SDDAC (playback) + SDADC (capture) codec drivers.
   Without these only the ALSA dummy card exists.
2. **DTS compatible-string mismatch** — `ark1668.dtsi` had
   `compatible = "arkmicro,ark-i2s"` / `"arkmicro,ark-sddac"` /
   `"arkmicro,ark-sdadc"`, but the drivers actually match on
   `"arkmicro,ark1668-i2s"` / `"arkmicro,ark1668-sddac"` /
   `"arkmicro,ark1668-sdadc"`. Total mismatch on all three — drivers
   never even attempted to probe, no errors printed, totally silent.
   Fixed by correcting the DTS strings to match the drivers.
3. **`CONFIG_ARK_DMA`** — the DMA controller driver for `dwdma0`
   (`compatible = "arkmicro,ark-dma"`). Without it,
   `devm_snd_dmaengine_pcm_register()` in the I2S driver fails with
   `Could not register PCM` (no DMA channel to bind to). This is also
   very likely why `mmc1` was silently falling back to PIO mode the
   whole time (`Failed to get external DMA channel`).
4. **`CONFIG_DW_DMAC` conflict** — enabling `ARK_DMA` collided with the
   generic upstream DW_DMAC driver (duplicate symbol names:
   `dw_dma_filter`, `dw_dma_enable`, `dw_dma_disable`). DW_DMAC was
   already dead weight on this board (never bound to anything useful —
   see the `mmc1` PIO fallback above), so disabled it in favor of
   `ARK_DMA`.
5. **`CONFIG_SND_SIMPLE_CARD`** — the DTS uses
   `compatible = "simple-audio-card"` to bind `i2s_dac`+`sddac` and
   `i2s_adc`+`sdadc` into an actual playable ALSA card. Missing this
   meant the codec/I2S drivers could each probe individually but
   nothing glued them into a card device.

After all five: `card 1: ark1668audio [ark1668-audio], device 1:
e4000000.i2s-dac-ark-sddac-codec e4000000.sddac-1` appears in `aplay -l`,
and boot log shows clean DAI-link mapping:

```
asoc-simple-card sound: ahb:sdadc@0 <-> e8200000.i2s-adc mapping ok
asoc-simple-card sound: e4000000.sddac <-> e4000000.i2s-dac mapping ok
```

## BD37033: does real hardware exist?

Initial schematic pass said no external audio-processor IC exists
("SDDAC direct to amp"). Arkmicro's own ARK1668E reference block diagram
(`http://www.arkmicro.com.cn/uploadfiles/2021/07/20210714150417532.jpg`)
appeared to corroborate this — `AUDIO` runs straight from the SoC to a
`Power AMP.` block with nothing in between.

Based on that, first attempt was a **stub**: `SoundType=3` (routes to
`Sound_BD37033`) + `CONFIG_SND_SOC_BD37033` + `CONFIG_SND_SOC_BD37033_NOP`
(every I2C register read/write silently returns 0, satisfying the app's
software requirement without any real hardware behind it). This got
`SoundAdapter Create` to succeed and `aplay -l` showed the dummy-only
device, no crash — but the boot-time speaker "pop" (a normal DAC
power-on transient that was present in earlier builds) *disappeared*,
suggesting something downstream was now sitting muted.

**Correction**: a later, closer physical inspection found an actual
**BD37033FV** chip on the board (chip marking). The schematic/reference
diagram were both wrong for this variant — same class of mistake as the
earlier GT911 bus assignment. `CONFIG_SND_SOC_BD37033_NOP` was reverted
(disabled) so the driver talks to the real chip.

### Bus and address correction via stock-kernel disassembly

The existing DTS had `drv_bd37033@40` on `i2c-gpio-1` (SDA=GPIO9,
SCL=GPIO121) — inherited from the original DTS, never independently
verified. Since the device wasn't accessible for a live `i2c-scan`,
verification was done by disassembling the stock 3.4.0 kernel
(`Prado firmware dump/mtd5_kernel/extracted/vmlinux.elf`, unstripped):

- `ark1680_add_device_audio()` registers 3 I2C devices via
  `analog_i2c_add_device_i2c_2()` (numeric bus "2"), traced by following
  the `i2c_board_info` array pointer through to
  `platform_device_register()`.
- That platform_device's `.id` field confirms bus "2"; its
  `platform_data` pointer resolves to an `i2c_gpio_platform_data` struct
  with `sda_pin=9, scl_pin=121` — i.e. exactly what this DTS calls
  `i2c-gpio-1`.
- The **same** 3-entry `i2c_board_info` array also contains `Goodix-TS`
  (addr `0x5d`) and `rn6752` (addr `0x2c`) — both devices already
  **live-confirmed** via `tools/i2c-scan` to actually be wired to
  `i2c-gpio-0` (SDA=GPIO3, SCL=GPIO2), not GPIO9/GPIO121 as the stock
  board file claims.
- `drv_bd37033`'s address field in that same struct reads `0x41`, not
  `0x40` as the inherited DTS had it.

Two out of three devices from that exact stock array are proven wrong in
the same direction (real wiring is `i2c-gpio-0`, not what the stock
board file's numeric bus resolves to). On that 2-for-2 precedent,
`drv_bd37033` was moved from `i2c-gpio-1` to `i2c-gpio-0`, address
corrected to `0x41`, and `i2c-gpio-1` removed entirely (nothing left on
it). **This bus placement is inference, not a live-scan confirmation** —
flagged in the DTS comment. Needs an `i2c-scan` on `i2c-gpio-0` for
`0x41` once the device is accessible again.

### `MsnProductInfo.ini`

```
SoundType=0   →   SoundType=3
```

Edited in `Prado firmware reconstructed/mtd6_rootfs/rootfs/msnprofile/MsnProductInfo.ini`.

## Live test log

### Test 1 — `aplay -l` before any fixes
Only the dummy card:
```
card 0: Dummy [Dummy], device 0: Dummy PCM [Dummy PCM]
```

### Test 2 — `aplay -l` after I2S/DAC/ADC/DMA/simple-card fixes
Real card appears:
```
card 1: ark1668audio [ark1668-audio], device 1: e4000000.i2s-dac-ark-sddac-codec e4000000.sddac-1 [e4000000.i2s-dac-ark-sddac-codec e4000000.sddac-1]
  Subdevices: 1/1
  Subdevice #0: subdevice #0
```

### Test 3 — boot-time speaker pop
- Present in the build *before* BD37033 was added at all.
- **Gone** after adding BD37033 in NOP-stub mode (`SoundType=3` +
  `CONFIG_SND_SOC_BD37033_NOP=y`). Not yet re-tested with NOP mode
  disabled (real hardware mode) — pending.

### Test 4 — `start_msn` crash status
Confirmed still crashing at the same `sendSoundData()+0xc4` point after
the NOP-stub build. Root cause suspected to be `SoundType` not actually
reaching the running device (stale SD build) or `Sound_BD37033`'s
constructor hitting a different bug once actually instantiated — not
yet isolated, pending a fresh crash capture on the real-hardware-mode
build.

### Test 5 — manual playback attempt

```sh
aplay -D hw:1,0 -f S16_LE -r 44100 -c 2 /dev/urandom
```
```
aplay: main:722: audio open error: No such file or directory
```
Wrong device index — `aplay -l` showed `device 1`, not `device 0`, for
card 1. Corrected:

```sh
aplay -D hw:1,1 -f S16_LE -r 44100 -c 2 /dev/urandom
```
```
Playing raw data '/dev/urandom' : Signed 16 bit Little Endian, Rate 44100 Hz, Stereo
...
set_params:1312 chunk_size=1024 buffer_size=22528
...
aplay: pcm_write:1953: write error: Input/output error
```
Device opens and negotiates params successfully now, but fails with EIO
on the actual PCM write — a DMA/hardware-level failure once real data
transfer starts, not an addressing/device-node issue. **Root cause not
yet found.** Next step: correlate with `dmesg` output from the same
failure (kernel-side DMA/FIFO error should print something that surfaces
as this EIO) — pending, not yet captured.

## Useful commands going forward

```sh
# List ALSA devices
aplay -l

# Check driver probe status
dmesg | grep -iE "i2s|sddac|sdadc|asoc|snd_soc|bd37033|drv_bd37033"

# Mixer state (before/after app init, to catch anything muting output)
amixer -c 1 scontrols
amixer -c 1 contents

# Tone test (needs speaker-test from alsa-utils; may not be present in this rootfs)
speaker-test -D hw:1,1 -c 2 -t sine -f 1000

# Noise test (fallback if speaker-test is unavailable)
aplay -D hw:1,1 -f S16_LE -r 44100 -c 2 /dev/urandom

# Confirm SoundType actually landed on the device
cat /msnprofile/MsnProductInfo.ini | grep SoundType
```

## Open items

- [ ] Correlate the `pcm_write` EIO with `dmesg` output from the same
      failure.
- [ ] Confirm `SoundType=3` actually reaches the live device (not just
      the rootfs source tree) before concluding `Sound_BD37033`'s
      constructor is the remaining crash cause.
- [ ] Live `i2c-scan` on `i2c-gpio-0` for address `0x41` once the device
      is accessible, to confirm the BD37033 bus placement made on
      disassembly-based inference.
- [ ] Re-test the boot-time speaker pop with NOP mode disabled (real
      hardware mode) to see if it returns.
- [ ] Check `amixer -c 1 contents` before/after `start_msn` to see if
      the app is muting/zeroing the SDDAC output as a side effect of a
      (possibly still-failing) `SoundAdapter` init sequence.
- [x] See "`sendSoundData()` crash — exact trigger found via
      disassembly" below (2026-07-13) — the crash is no longer at the
      point Test 4 described; it's moved further into the call, and the
      new location is a distinct, well-evidenced bug.

## `sendSoundData()` crash — exact trigger found via disassembly (2026-07-13)

Test 4 above ("still crashing at the same `sendSoundData()+0xc4` point")
is confirmed still true on a fresh capture (`docs/matching crash files
v2/err-9.log`, a live on-device minidump — timestamp reads 2020 only
because the unit's RTC has no battery backup and every boot runs `date
-s "2020-12-03 ..."` before anything else, see `docs/matching crash
files v2/crash3.strace` line ~2709), **but the app now gets much
further before crashing**: `docs/matching crash files v2/crash3.strace`
shows `MsnCoreApp` successfully opening `/dev/snd/controlC0` and
enumerating ALSA mixer controls (`SNDRV_CTL_IOCTL_ELEM_INFO` in a loop)
— i.e. a real sound card now exists and responds, unlike the
dummy-card-only state Test 4 was captured against. So the I2S/DMA/
simple-card/`SoundType=3` fixes above did their job; this is a
*different, later* failure in the same function.

**Root cause, traced via disassembly of `libSetting.so`
(`SettingWindow::onFirstInit()` and `SettingWindow::sendSoundData()`,
`arm-linux-gnueabihf-objdump -C -d`, cross-checked against the live
register dump in `err-9.log`):**

1. `onFirstInit()` (`0x3dcac`) reads a per-object flag at `this+0x60`
   (set earlier in the same function from
   `MsnApplication::getTag(AppTagId=0x10009, ...)`, `bne`/`movne`
   pattern at `0x3e170`-`0x3e17c` — nonzero tag value → flag `true`).
2. **If that flag is `0`** (`0x3e34c`-`0x3e358`,
   `ldrb r3,[this,#96]; cmp r3,#0; beq 0x3e810`), execution **skips
   the entire EQ/volume-parameter bootstrap block** — `initSoundParams()`,
   the `isBoxProduct()` check, and ~15 `MsnIniConfig`/
   `MsnApplication::getFactorySetting()` calls that would otherwise
   populate several byte/int fields in the `SettingWindow` object
   (`this+0x118`, `0x11c`, `0x120`, `0x122` — EQ preset name strings
   like `"Original"`/`"Classical"`, frequency-band lists, etc., visible
   as literal pool strings in that block) — and jumps straight to a
   **minimal fallback call**: `sendSoundData(17, 88, (const char*)&this[0x123], 1)`
   at `0x3e810`-`0x3e82c`. `this[0x123]` here is a single ini-derived
   byte (set earlier and unconditionally at `0x3e288` from
   `MsnIniConfig::value(...).toUInt()`), being passed as if it were a
   C string.
3. **This is the code path stock hardware never takes.** The long
   initialization block (taken when the tag is nonzero) never itself
   calls `sendSoundData()` in `onFirstInit()` — it just populates state
   and returns. Only the `this+0x60 == 0` short-circuit calls
   `sendSoundData()` from here, with this minimal, mostly-empty
   argument set.
4. Inside `sendSoundData()` (`0x34fa8`), that first mostly-empty
   message **is built and dispatched successfully** (`MsnEvent`
   construction, `makeProtocolPackage()`, `setByteArrayParams()`,
   `MsnApplication::dispatchMsnEvent()` all complete at `0x34fd4`-`0x3504c`,
   no crash here). Immediately after, the function starts building a
   **second** protocol message and at `0x3506c` does
   `r5 = [sp+68]; r0 = [r5]` — a `QByteArray`/`QString` refcount-deref
   pattern (`blx r7` calls the ARM kernel user helper at the fixed
   address `0xffff0fc0`, i.e. `__kuser_cmpxchg`, standard for
   `QAtomicInt`). **`[sp+68]` is never written anywhere in this
   function** (confirmed by disassembling the complete 0x160-byte
   function body) — it's an uninitialized local. The live crash
   register dump confirms this exactly: `r5 = 0x00000008`, fault
   address `0x00000008` — the "pointer" is literally the small
   leftover stack value `8` (matches a stray `mov r1, #8` used earlier
   in the same function for an unrelated purpose), not a valid object.
5. **Conclusion: this is a genuine bug in the shipped `libSetting.so`**
   — `sendSoundData()`'s second-message code path reads an
   uninitialized local under the specific (minimal-args) calling
   pattern used by the `this+0x60 == 0` fallback branch. Stock hardware
   presumably always has `AppTagId 0x10009` set to a nonzero value (so
   `onFirstInit()` always takes the long initialization path, which
   never reaches this broken call), meaning this exact fallback branch
   is very likely **untested dead code in the original firmware** —
   the same class of bug as the `ark_display` fallback-path segfault
   found earlier in this investigation.

**Not yet found:** who calls `MsnApplication::setTag(0x10009, ...)` and
under what condition (grepped ~99 `setTag` call sites in `MsnCoreApp`
alone; didn't isolate the one setting tag `0x10009` specifically in this
pass). Best working hypothesis, given the timing (this tag is read
during Settings-window init, right after the sound-card subsystem
comes up) is that it reflects **whether a real sound IC was
successfully detected/initialized** — i.e. it may still trace back to
the open items above (BD37033 bus/address not live-confirmed,
`Sound_BD37033` constructor status, `pcm_write` EIO). If so, finishing
those open items might make `onFirstInit()` take the long/safe path and
sidestep this bug entirely, without needing to touch `libSetting.so`
at all.

**Next steps (superseded — see live re-test below):**
- [x] ~~Identify the `setTag(0x10009, ...)` call site~~ — moot, see below:
      the `this+0x60 == 0` fallback branch theory does not match what
      actually happens on live hardware.
- [ ] If a direct patch turns out to be necessary: the minimal fix is
      in `sendSoundData()` at `0x3506c` — needs to skip/guard the
      second-message block instead of unconditionally dereferencing the
      uninitialized local. This would be a binary patch (no source
      available), same constraints as `MSNCOREAPP_DECONSTRUCTION.md`'s
      UI-patch workflow — but this is register-logic surgery, not a
      `QRect` immediate, so much higher risk.

## Live re-test on real hardware (2026-07-13, serial console) — re-entrancy bug, not the fallback path

Got live serial console access and re-ran `start_msn` directly (fresh
`dmesg -c` first, then `start_msn` in the foreground). Two things
immediately overturned the previous theory:

1. **`SoundAdapter Create Success, ICType: 3`** — the sound adapter
   initializes successfully now. The earlier "`SoundAdapter Create
   Failed`" / `AppTag 0x10009 == 0` fallback-branch theory is **wrong**;
   that failure path isn't what's being hit at all.
2. **New crash dump (`/data/minidump/err-6.log`) has a completely
   different call stack** than `err-9.log`. Crash is still at
   `SettingWindow::sendSoundData()+0xc4` — confirming the uninitialized-
   local bug at `0x3506c` found via disassembly is real and still the
   proximate cause — but this time it's reached via:

   ```
   SettingWindow::initSoundParams()+0x6c
     -> SettingWindow::sendSoundData()+0xa8   (first call, dispatches OK)
       -> MsnApplication::dispatchMsnEvent()   (synchronous, not queued)
         -> MsnSoundPlugin::customEvent()
           -> Sound_BD37033::onRecvAppProtocol()
             -> Sound_BD37033::onRecvSoundProtocol()+0x7e0
               -> SettingWindow::sendSoundData()+0xc4   *** CRASH ***
   ```

   **This is re-entrancy, not a skipped-initialization edge case.**
   `initSoundParams()` calls `sendSoundData()` as part of completely
   normal, expected operation (not the `this+0x60==0` fallback from the
   earlier theory). That first call's `dispatchMsnEvent()` runs its
   handler chain *synchronously* (not queued to the event loop), which
   round-trips through `Sound_BD37033`'s protocol handler and calls
   `sendSoundData()` again, **re-entrantly, from inside the first
   call's own event-dispatch**. It's this nested second call that lands
   in the same uninitialized-`sp+68` code path identified earlier.

**Why this doesn't happen on stock hardware:** the specific arguments
`Sound_BD37033::onRecvSoundProtocol()` passes to its re-entrant
`sendSoundData()` call almost certainly depend on values read back from
the BD37033 chip (current volume/EQ/balance state) — and we independently
confirmed via `dmesg` that **every write to the BD37033 over
`i2c-gpio-0` times out** (`bd37033_write_byte timeout`, `BD37033.c`
`i2c_transfer()` failing all 5 retries). If the chip never actually
receives/acks these writes, whatever `onRecvSoundProtocol` reads back to
build its response is stale/default/garbage relative to what stock
hardware (with working I2C) would have — plausibly landing on the
specific parameter combination that hits the buggy branch in
`sendSoundData()`. This is a hypothesis, not yet proven byte-for-byte,
but it ties the I2C failure and the crash into one coherent chain
without requiring a `libSetting.so` binary patch as the primary fix.

**UPDATE — root cause of the I2C failures found, and it's not clock
stretching:** see `docs/DISPLAY_SUBSYSTEM.md`. `i2c-gpio-0`'s
SCL/SDA (GPIO2/GPIO3) are pin-mux-conflicted with the LCD's active
RGB888 r0/r1 data lines — confirmed live via
`/sys/kernel/debug/pinctrl/e4900000.pinctrl/pinmux-pins`, which shows
those exact pins currently muxed to the LCD, not GPIO. The bus was
never electrically functional; `i2c-gpio,scl-output-only` is a red
herring.

**UPDATE 2 — confirmed via live i2c-scan on real stock hardware
(2026-07-13):** BD37033's *actual* populated bus is stock's `i2c-gpio2`
(`sda_pin=9, scl_pin=121`) — **not** `i2c-gpio-0`/GPIO2-3, where this
project's DTS currently has it. An earlier session moved it there based
on a misread of `i2c-scan`'s `XX` marker (which only proves a driver is
bound, not that the device answers — see the corrected methodology note
in `I2C_GPIO0_LCD_PIN_CONFLICT.md`). Live stock hardware shows BD37033
bound (`XX`) at `0x41` on `i2c-gpio2`, off the LCD-conflicted pins
entirely. **Fix: move `drv_bd37033` back to `sda_pin=9, scl_pin=121`
in the DTS** (this project's own `i2c-gpio-1`, removed in an earlier
session) — the hope was that this would resolve the write-timeout
failures without needing to touch the LCD/`i2c-gpio-0` conflict at all.
**Correction (2026-07-14): it did not.** See "BD37033 write timeouts
persist on i2c-gpio-1" below — `bd37033_write_byte timeout` still
appears in every boot log captured after this move, including the one
this doc previously cited as confirming the fix. rn6752 stays on
`i2c-gpio-0`/GPIO2-3 — confirmed to be its real bus on stock hardware
too, LCD sharing and all (see the recurring `rn6752_eq_work reset`
workaround as a likely symptom, not a sign the bus is totally dead).

**`i2c-scan` correction:** re-checked `tools/i2c-scan/i2c-scan.c` — its
`XX` marker means `ioctl(fd, I2C_SLAVE, addr) < 0` (address already
bound to a kernel driver), **not** a live bus ACK test. So confirming
`0x41` shows `XX` only re-confirms the DTS binding, it does *not*
independently verify the chip answers on the bus — the `dmesg`
`bd37033_write_byte timeout` messages are the real (and negative)
signal here.

**Next steps:**
- [x] Fix the I2C write failures / bus placement — **attempted,
      2026-07-13**: moved `drv_bd37033` back to `i2c-gpio-1`
      (GPIO9/GPIO121), its confirmed-correct bus from live stock
      hardware testing (see `I2C_GPIO0_LCD_PIN_CONFLICT.md`). Rebuilt
      and boot-tested. **This corrected the DTS bus/address (necessary,
      matches stock) but did not stop the write timeouts** — see the
      2026-07-14 correction below. Root cause of the timeouts is still
      open.
- [x] Re-test `start_msn` with the bus fix — **crash persists**, and the
      call path actually *simplified*: `err-7.log` (full kernel+app
      boot #31, `docs/new kernel audio working log v1.log`) shows
      `SettingWindow::onFirstInit()+0x6c4` → `initSoundParams()+0x6c`
      (right at the start of that function) → `sendSoundData()+0xc4`
      **directly** — no re-entrant round-trip through `Sound_BD37033`
      needed this time. `SoundAdapter Create Success, ICType: 3` and
      `initSoundParams()` reaches the buggy code on its very first
      top-level call. (The `Open I2C Device: 2 "80"  FD: 16` line here
      only shows userspace successfully opened `/dev/i2c-2` — it does
      **not** mean the kernel driver's writes to the chip succeeded;
      `bd37033_write_byte timeout` is present in this same boot log,
      see below. This doc previously conflated the two — corrected
      2026-07-14.)
- [x] **Conclusion: the uninitialized `sp+68` local in `sendSoundData()`
      (`0x3506c` in `libSetting.so`) is a standalone bug in the shipped
      binary, unconditionally reachable on ordinary EQ init** — not an
      edge case gated by I2C/DTS/SoundType correctness. No amount of
      kernel/DTS/config fixing can route around it; it will crash
      `MsnCoreApp` at Settings-window init on every boot regardless of
      hardware state. A binary patch to `sendSoundData()` is now the
      only remaining path forward for this crash specifically.
- [ ] Binary patch `sendSoundData()` at `0x3506c` — guard or skip the
      second-message block instead of unconditionally dereferencing the
      uninitialized local. Not yet attempted.
- [x] **Found and fixed a real, separate audio bug while comparing
      against stock's boot log (2026-07-13).** Our own boot logs show 6
      `ALSA lib pcm_dmix.c: unable to open slave` errors right at
      `MsnCoreApp` startup, every single run; stock's boot log has
      zero. Root cause: `/etc/asound.conf`'s `dmix`/`softvol*` PCMs
      hardcode their slave as `pcm "hw:0,0"` (card 0, device 0), but
      this project's DTS (`ark1668_limcet_p305.dts`) listed the ADC/
      capture `dai-link` first and the DAC/playback `dai-link` second —
      `simple-audio-card` assigns PCM device numbers by DAI-link order,
      so playback landed on device 1 (confirmed directly:
      `aplay -l` in `new kernel audio working log v1.log` shows only
      `device 1: e4000000.i2s-dac-ark-sddac-codec`, never device 0).
      `hw:0,0` was therefore capture-only/invalid for playback, so
      every dmix-based PCM open failed. **Fix:** reordered the two
      `dai-link` blocks (DAC/playback now `@0`, ADC/capture now `@1`)
      in both `ark1668_limcet_p305.dts` and the docs-repo reference
      copy — no `asound.conf` change needed. Compiles clean via `dtc`,
      decompiled to confirm the DAC link's `bitclock-master`/
      `frame-master` properties now sit on `dai-link@0`. **Not yet
      hardware-tested.** This is unrelated to the `sendSoundData()`
      crash chase but is a real, independently-worth-having fix — it's
      very plausible the crash's uninitialized-stack-garbage
      sensitivity (see above) is itself downstream of this: 6 failed
      ALSA opens run a materially different amount of code/stack usage
      right before `initSoundParams()`/`sendSoundData()` than stock's
      clean path, which could easily be exactly the kind of
      "environment-dependent stack garbage" difference already
      identified as the mechanism. **Worth re-testing the crash after
      this fix alone, before attempting any binary patch** — it may
      turn out to be unnecessary.

## Chased the trigger failure — found a real, incomplete driver port (2026-07-13)

After the DAI-link fix above, the dmix error changed from `unable to
open slave` to **`unable to initialize slave`** /
`snd1_pcm_direct_initialize_slave: unable to start PCM stream`
(`docs/start_msn crash log v1.log`) — the device now opens and
`hw_params` succeeds cleanly (confirmed via `ark_i2s_startup`/
`ark_sddac_startup` printing with no kernel-side errors, 6 times, once
per `softvol*` PCM), but the actual PCM start fails. The crash itself
is unaffected (identical trace, `err-1.log`).

**Root cause, found in `linux/sound/soc/arkmicro/ark1668_i2s.c`:** the
DMA-request-enable register bits are never actually set.

- `ark_i2s_startup()`'s playback branch has the `TDMAENA` (TX DMA
  enable) register write **fully commented out** (lines ~162-164 in
  the pre-fix source).
- The capture branch explicitly **clears** `RDMAENA` as part of a
  register mask/set sequence and never re-enables it — the intended
  re-enable a few lines later is also commented out.
- `ark_i2s_trigger()`'s `SNDRV_PCM_TRIGGER_START` path calls
  `ark_i2s_txctrl()`/`ark_i2s_rxctrl()`, which are **empty stub
  functions** (`{}`, no body) — the `/* TODO: start i2s */
  /* TODO: Start DMA */` comments next to the calls were never
  implemented.

Effect: `devm_snd_dmaengine_pcm_register()`'s generic `dmaengine_pcm`
framework can arm the DMA *channel*, but without `TDMAENA`/`RDMAENA`
set, the I2S *peripheral itself* never asserts its hardware DMA-request
line, so the channel never actually receives data to transfer. The
stream silently never progresses — exactly matching clean
probe/open/`hw_params` followed by a silent failure only at
trigger/start. This looks like a driver port that was functionally
incomplete for real DMA-triggered audio from the start, independent of
anything else in this investigation.

**Fix applied:** uncommented/re-enabled the `TDMAENA` write in the
playback branch and added the `RDMAENA` write back in the capture
branch of `ark_i2s_startup()`. Compiles clean (only pre-existing,
unrelated unused-variable/function warnings). `ark_i2s_txctrl()`/
`ark_i2s_rxctrl()` remain empty stubs — not yet clear whether they need
real implementations too, or whether the DMA-enable bits alone are
sufficient (the generic dmaengine framework may handle the rest).
**Not yet hardware-tested** — kernel rebuild in progress.

**Still open:** even if this fixes real audio playback, it's unrelated
to the `sendSoundData()` crash chase — that's a separate userspace bug
in `libSetting.so`, not explained by this kernel-side DMA issue. Worth
testing both independently once this build is verified.

## TDMAENA fix alone was insufficient — found the real blocker: `device_prep_dma_cyclic` was never wired up

Live-tested the TDMAENA/RDMAENA fix (`docs/start_msn crash log v1.log`).
Real progress but not fixed: `dmix`'s error changed from `unable to
open slave` to `unable to initialize slave` /
`snd1_pcm_direct_initialize_slave: unable to start PCM stream` — the
device now opens and negotiates `hw_params` cleanly, but still fails at
actual stream start. Raw `aplay -D hw:0,0` (bypassing `dmix`) got
further still: clean open + `hw_params`, then failed at the first
`pcm_write` with `Input/output error` — and `dmesg` showed **nothing**
for this failure at all (last kernel message was the open-time
`ark_i2s_startup`/`ark_sddac_startup` prints, nothing logged for the
write itself).

**Root cause, found in `drivers/dma/ark-dma.c`:** this driver (a fork of
mainline Linux's old `dw_dmac`) has a complete, working cyclic-DMA
implementation (`dw_dma_cyclic_prep()`/`_start()`/`_stop()`/`_free()`,
all `EXPORT_SYMBOL`'d) — but it predates the standard `dmaengine`
cyclic API and was **never wired into the generic `dma_device` ops
table** (`dw->dma.device_prep_dma_cyclic` was simply never assigned,
and `DMA_CYCLIC` was never added to `dw->dma.cap_mask`). ALSA's
`dmaengine_pcm` framework — what `devm_snd_dmaengine_pcm_register()` in
`sound/soc/arkmicro/ark1668_i2s.c` uses — calls
`dmaengine_prep_dma_cyclic()`, which resolves to a NULL op and fails
**silently**, before any hardware register is touched. This explains
both symptoms at once: no kernel-side error (the failure never reaches
the driver at all) and why the TDMAENA fix alone couldn't be sufficient
(the DMA channel never got a transfer descriptor in the first place,
regardless of whether the I2S peripheral would have requested it).

**Also found:** the *normal* one-shot start path
(`dwc_issue_pending()` → `dwc_dostart_first_queued()` →
`dwc_dostart()` → `dwc_initialize()`) only unmasks `MASK.XFER`/
`MASK.ERROR`, **not `MASK.BLOCK`** — the interrupt `dwc_handle_cyclic()`
depends on for period-boundary detection. Only the existing
`dw_dma_cyclic_start()` correctly unmasks `MASK.BLOCK`. So simply
wiring a `device_prep_dma_cyclic` callback and letting cyclic
descriptors flow through the *existing* queue/`issue_pending` machinery
would have silently reintroduced a very similar bug one layer up.

**Fix applied** (`drivers/dma/ark-dma.c`):
1. Added forward declarations for `dw_dma_cyclic_start()`/
   `dw_dma_cyclic_prep()` (no header prototype existed; critically,
   `dw_dma_cyclic_prep()` returns a pointer, so an implicit declaration
   would have defaulted to `int` and truncated/corrupted the result).
2. New `dwc_prep_dma_cyclic()` — thin wrapper calling the existing
   `dw_dma_cyclic_prep()`, returning `&cdesc->desc[0]->txd` (already
   fully initialized by `dwc_desc_get()`). Registered as
   `dw->dma.device_prep_dma_cyclic`. Added `DMA_CYCLIC` to
   `dw->dma.cap_mask`.
3. `dwc_tx_submit()`: cyclic descriptors now bridge the generic
   dmaengine callback (`tx->callback`/`callback_param`, set by the
   framework after prep returns) onto the legacy
   `dwc->cdesc->period_callback`/`period_callback_param` fields that
   `dwc_handle_cyclic()` actually reads — confirmed exact type match
   (`dma_async_tx_callback` = `void (*)(void *)`, same as
   `period_callback`). Cyclic descriptors are **not** added to the
   normal one-shot `dwc->queue` (harmless either way since
   `dwc_scan_descriptors()` never runs for a cyclic channel, but
   cleaner and avoids any chance of the one-shot path mismanaging it).
4. `dwc_issue_pending()`: cyclic channels now call the existing,
   already-correct `dw_dma_cyclic_start()` directly (which unmasks
   `MASK.BLOCK` and starts the ring) instead of the normal
   `dwc_dostart_first_queued()` path — reuses tested logic rather than
   duplicating the interrupt-masking behavior.

Compiles clean (`drivers/dma/ark-dma.o`, zero warnings). Kernel rebuilt
(`zImage`, modules, `zImage.w_dtb` reassembled).

**Live-tested with `ARKDMA_DBG` printk instrumentation added at every
step.** First `aplay -D hw:0,0` attempt: `dwc_prep_dma_cyclic` called
(previously never reached at all), `dw_dma_cyclic_prep` succeeded (22
periods), `dwc_tx_submit` bridged a real non-NULL callback,
`dwc_issue_pending` → `dw_dma_cyclic_start` returned 0 (success — DMA
channel genuinely armed and started). **Every step of the new plumbing
works exactly as designed.**

**Second `aplay` attempt in the same session immediately failed at
`dw_dma_cyclic_prep`, err=-16 (`-EBUSY`).** Root cause:
`dwc_terminate_all()` (the generic `device_terminate_all` callback,
invoked on stream stop/close) never knew about cyclic state — cyclic
descriptors don't touch `dwc->queue`/`active_list` (by design, see
`dwc_tx_submit()` above), so `dwc_terminate_all()`'s existing one-shot
cleanup was a complete no-op for them, and `DW_DMA_IS_CYCLIC` stayed
set forever. `dw_dma_cyclic_prep()`'s own
`test_and_set_bit(DW_DMA_IS_CYCLIC, ...)` guard then permanently
refuses any later attempt on that channel, matching exactly what was
observed. **Fix:** `dwc_terminate_all()` now calls the existing
`dw_dma_cyclic_free()` (disables channel, frees `dwc->cdesc` and its
descriptors, clears `DW_DMA_IS_CYCLIC`) when the channel is in cyclic
mode, instead of falling through to the one-shot cleanup path.
Compiles clean. Kernel rebuilt again. **Not yet re-tested** — this
should make repeated `aplay` attempts work reliably; still need to
confirm actual audio data transfer completes correctly (BLOCK
interrupts firing, no xrun) on a clean single run before calling this
fully resolved.
- [x] **Cross-checked against `docs/boot log.txt` (real stock boot,
      same binaries) — the bug is NOT an unconditional crash.** Stock
      hits the exact same broken config (`SoundAdapter Create Failed,
      Not Support ICType: 0`, i.e. `SoundType=0`, confirmed live this
      session to be stock's actual factory value) and takes the same
      abbreviated `sendSoundData()` fallback path — and does **not**
      crash. Boot continues normally all the way to `MsnCoreApp End
      init` and a fully working session. Since this is the identical
      `libSetting.so` binary (this reconstructed rootfs is built from
      this same firmware dump), the crash is **not an inherent defect
      that always fires** — it's an uninitialized-stack-value bug whose
      outcome depends on whatever garbage happens to occupy that stack
      slot, which is sensitive to the surrounding runtime environment
      (kernel version, call history/depth, timing) even with
      byte-identical code. On stock's exact runtime state, the garbage
      value apparently doesn't crash when dereferenced (e.g. happens to
      be 0 or a safe address); on our 4.19 reconstruction's runtime
      state, it reliably does, every single time tested so far.
      **Practical implication:** a binary patch to make `sendSoundData()`
      safe regardless of stack contents is still the right fix (this
      project's kernel/libc/runtime will likely never exactly match
      stock's stack layout), but don't expect to find a *root* cause
      beyond "different environment, different garbage on the stack" —
      full explanation would need live register/stack inspection via a
      debugger, which isn't available on this target.
- [x] Confirmed: the device **auto-reboots after this crash** every
      time (minidump-then-reboot sequence, consistent across every
      capture so far — `err-9.log`, `err-6.log`, `err-7.log`), so each
      test iteration costs a full boot cycle.

## Stock driver decompilation — comparing against `vmlinux.elf` (2026-07-13)

Disassembled stock's Linux 3.4.0 `vmlinux.elf` (`Prado firmware
dump/mtd5_kernel/extracted/vmlinux.elf`, unstripped) to check our
4.19 port's DMA/I2S plumbing against a known-working ground truth,
using `nm`/`objdump -d`.

**Confirmed shared codebase lineage:** stock's kernel contains
byte-identical symbol names for the entire cyclic-DMA API
(`dw_dma_cyclic_prep/_start/_stop/_free`, `dwc_handle_cyclic`,
`dw_dma_tasklet`, `dw_dma_interrupt`) and the platform PCM driver
(`ark_pcm_*`) as our `ark-dma.c`/`ark1668_i2s.c` — same vendor fork,
different eras.

**Key architectural finding: stock never used the generic
`dmaengine_pcm` framework at all.** Disassembly of stock's
`ark_pcm_trigger` (0x802f5408), `ark_pcm_prepare_dma` (0x802f552c),
`ark_pcm_prepare` (0x802f55c8), and `ark_pcm_dma_period_done`
(0x802f5628) shows a small, hand-written ALSA platform driver that:
- calls `dw_dma_cyclic_prep()` **directly** from `.prepare`, then
  manually writes the returned `dw_cyclic_desc`'s
  `period_callback`/`period_callback_param` fields (no
  `dma_async_tx_descriptor`, no `tx_submit`, no
  `dmaengine_submit()` involved at all)
- calls `dw_dma_cyclic_start()`/`dw_dma_cyclic_stop()` **directly**
  from `.trigger`'s own cmd dispatch (no `dma_async_issue_pending()`)
- wires `period_callback` to `ark_pcm_dma_period_done`, which simply
  dereferences a private struct to get the `snd_pcm_substream*` and
  calls `snd_pcm_period_elapsed()` — trivial, no extra logic

Our 4.19 port instead uses `devm_snd_dmaengine_pcm_register()`
(`sound/soc/arkmicro/ark1668_i2s.c`), the modern generic dmaengine
slave-DMA framework, which drives the same low-level
`dw_dma_cyclic_*` functions through an additional layer
(`device_prep_dma_cyclic` → `dma_async_tx_descriptor` →
`tx_submit` → `dma_async_issue_pending`) that stock's vendor code
never exercised — this generic-framework cyclic path is new,
unproven code on this SoC, built entirely by this reconstruction
effort (see "TDMAENA fix alone was insufficient" above).

**Cross-checked every layer our bridge touches against stock and
found no discrepancy:**
- `dw_dma_cyclic_prep/_start/_stop/_free` in our `ark-dma.c` are
  verbatim identical to stock's (same register writes, same LLI
  construction, same `MASK.BLOCK` handling) — confirmed by direct
  disassembly comparison of `dw_dma_cyclic_start`.
- `ark1668_i2s.c`'s DMA slave config
  (`playback_dma_data.addr = mem->start + ARK_I2SSDDAC_SADR`,
  `addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES`, `maxburst = 16`) is
  correctly populated and reaches `dwc->dma_sconfig` via
  `dwc_config()` (`device_config` callback, wired at
  `ark-dma.c:1723`) — `dw_dma_cyclic_prep()`'s `sconfig->dst_addr`
  read will not be zero/bogus.
- DT DMA channel assignment for `i2s_dac` (`dmas = <&dwdma0 0 1 0>,
  <&dwdma0 1 0 1>` in `ark1668.dtsi`) resolves through
  `dw_dma_of_xlate()` to real `src_id`/`dst_id` hardware
  handshake-interface numbers — this is inherited DT, unmodified by
  this reconstruction, not a suspect.

**Conclusion: the remaining `pcm_write` `Input/output error` is very
unlikely to be in `ark-dma.c`'s low-level cyclic logic itself** (it's
identical to stock) **or in the DMA slave/address configuration**
(also correctly wired). The most likely remaining suspect is
something specific to how the *generic dmaengine_pcm framework*
layer behaves differently from stock's direct hand-rolled calls —
e.g. timing/ordering around `.prepare` vs `.trigger`, or a
bookkeeping detail in the `dma_async_tx_descriptor` bridge
(`dwc_tx_submit`) that has no equivalent in stock to have gotten
right or wrong. This is exactly what the pending IRQ-trace
instrumentation (`dw_dma_interrupt`/`dwc_handle_cyclic` printks,
already built into the currently-flashed kernel — see previous
section) is designed to answer: if `dw_dma_interrupt` never fires at
all, the bug is upstream of the interrupt (channel not actually
armed despite `dw_dma_cyclic_start()` returning 0 — e.g. clock
gating, IRQ line not requested/enabled at the platform level); if it
fires but `dwc_handle_cyclic` never advances/calls the period
callback correctly, the bug is in the `dwc_tx_submit` callback
bridge. **This test still needs to be run** (`aplay -D hw:0,0` then
`dmesg | grep ARKDMA_DBG`) — that live result is now the most
information-dense next step, more so than further static analysis.

## Live IRQ-trace test run — found and fixed the real bug (2026-07-13)

Ran `aplay -D hw:0,0 -f S16_LE -r 44100 -c 2 -d 3 /dev/urandom` with
the `ARKDMA_DBG` instrumentation. Result: `dwc_prep_dma_cyclic` →
`dw_dma_cyclic_prep` (22 periods) → `dwc_tx_submit` (callback bridged)
→ `dwc_issue_pending` → `dw_dma_cyclic_start` all succeed (`ret=0`) —
then **immediately** `pcm_write` fails with `Input/output error` and
`dwc_terminate_all` tears the channel down. Critically: **zero**
`dw_dma_interrupt`/`dwc_handle_cyclic` printk lines appear anywhere in
the log, before or after — the DMA controller never got the chance to
raise a single real interrupt.

**Root cause found in `dwc_tx_status()` / `dwc_scan_descriptors()`**
(`drivers/dma/ark-dma.c`): the generic `dmaengine_pcm` framework polls
`.pointer()` (→ `dmaengine_tx_status()` → our `device_tx_status` =
`dwc_tx_status`) during normal playback to report the ALSA hardware
pointer — something stock's direct `dw_dma_cyclic_*` callers **never
did** (see decompilation findings above), so this code path was never
exercised before this reconstruction wired up the generic framework.

`dwc_tx_status()` unconditionally called `dwc_scan_descriptors()`,
which was written only for the one-shot queue/`active_list` path and
has an ordering bug: it checks the raw `RAW.XFER` status bit — a
hardware flag latched independently of IRQ masking — **before**
bailing out on an empty `active_list` (cyclic transfers never
populate `active_list`, by design — see the `dwc_tx_submit()` fix
earlier). If `RAW.XFER` is ever latched during a cyclic transfer, it
falls into `dwc_complete_all()`, which checks whether the channel is
still enabled — true by design for a running cyclic loop — logs `BUG:
XFER bit set, but channel not idle!`, and calls `dwc_chan_disable()`,
**actively killing the just-started cyclic transfer**, almost
certainly on the very first `.pointer()` poll right after
`dma_async_issue_pending()`, before the hardware could reach its
first real block boundary. This exactly explains the symptom: instant
failure, zero interrupts ever observed.

**Fix applied:** `dwc_tx_status()` now checks
`test_bit(DW_DMA_IS_CYCLIC, &dwc->flags)` up front and, for cyclic
channels, skips `dwc_scan_descriptors()` entirely — mirroring the
cyclic special-casing already done for `dwc_tx_submit()`,
`dwc_issue_pending()`, and `dwc_terminate_all()`. Residue is now
computed directly from the current descriptor's sent-byte count
(`dwc_get_sent()`) instead of always reporting 0 — this also resolves
the previously-deferred "cyclic residue always reports 0" gap noted
in the `device_prep_dma_cyclic` section above, since both bugs traced
back to the same unguarded function.

Kernel rebuild + live re-test pending.

## BD37033 write timeouts persist on i2c-gpio-1 — 2026-07-13 "fix" corrected (2026-07-14)

`tools/audio-test/audio-test.sh` (run on-device 2026-07-14) failed all
three of its mechanical checks: no `PA Volume`/`PA Mute` mixer control,
no I2C ACK at `0x41`, and (in that particular run) no ALSA card at all
in `/proc/asound/cards`.

Cross-checked against the two boot logs captured today
(`docs/new kernel bootlog new uboot usb probe v12.txt` 14:42 and
`v13.txt` 14:57) plus the log this doc previously cited as confirming
the i2c-gpio-1 bus-move fix (`docs/new kernel audio working log
v1.log`, 2026-07-13). Findings:

- **The "no ALSA card at all" result does not reproduce.** Both of
  today's logs show `ALSA device list: #0: ark1668-audio` and clean
  `sddac <-> i2s-dac` / `sdadc <-> i2s-adc` DAI-link mapping. Whatever
  produced the empty `/proc/asound/cards` in the audio-test.sh run was
  specific to that boot/timing, not a standing regression — re-run the
  script to see if it reproduces.
- **The BD37033 write-timeout is real, current, and was never actually
  fixed.** All three logs — v12, v13, and the 2026-07-13 log this doc
  called "working" — show identical `bd37033_write_byte timeout` spam
  at probe time, on the corrected `i2c-gpio-1` (GPIO9/GPIO121) bus.
  Moving the DTS node off `i2c-gpio-0` was still the right call (it
  matches stock's confirmed-live wiring, and got the DTS/address
  correct: `0x41` not `0x40`), but it did not resolve the timeout
  itself. The earlier "Open I2C Device: 2 ... FD: 16" observation that
  this doc previously treated as evidence the fix worked was userspace
  successfully opening `/dev/i2c-2` — unrelated to whether the driver's
  writes reach the chip. That conflation has been corrected above.

**Root cause of the missing `PA Volume`/`PA Mute` controls found
(2026-07-14) — separate from the write-timeout question.** The DTS's
`sound { }` `simple-audio-card` node only ever listed two dai-links
(`i2s_dac`+`sddac`, `i2s_adc`+`sdadc`) — `&amp` (`drv_bd37033`) was
never referenced by the card at all. `bd37033_drv_probe()`
(`sound/soc/arkmicro/BD37033.c`) unconditionally calls
`devm_snd_soc_register_component()` regardless of whether the I2C
writes in `bd37033_init()` succeeded, so the component *does* register
with ASoC — but a component's `snd_kcontrol_new` array (`PA Volume`
etc, `bd37033_controls[]`) only gets added to a card's exposed control
list when that component is bound into a card via a dai-link (or
`aux-devs`, ASoC's mechanism for a control-only "device" with no DAI
stream of its own — see `sound/soc/generic/simple-card.c`'s
`asoc_simple_card_parse_aux_devs()`). Since `&amp` was in neither the
card's dai-links nor an `aux-devs` list, its controls could never reach
`amixer`, independent of whether the I2C bus itself works.

**Fix applied (2026-07-14):** added
`simple-audio-card,aux-devs = <&amp>;` to the `sound` node in
`ark1668_limcet_p305.dts` (and mirrored in the docs-repo reference copy,
`Limcet Hardware/ark1668-limcet-prado.dts`). Verified by hand-compiling
with `dtc`: the resulting dtb's `aux-devs` phandle (`0x32`) matches
`drv_bd37033@41`'s own phandle. Not yet kernel-rebuilt or hardware
re-tested.

**Still not root-caused: the write timeouts themselves.** Whether the
mixer control's *reads/writes* actually reach the chip is a separate,
still-open question — **Correction: the "benign on stock too" claim is independently verified,
not just asserted.** `docs/DISPLAY_SUBSYSTEM.md` (~line 217)
documents a live `dmesg` captured immediately after a stock-hardware
reboot (before the ring buffer could wrap), showing the identical
`bd37033_write_byte timeout` × 8 right after `bd37033_drv_probe`, on
stock's own shipped firmware, on BD37033's own correct factory bus.
This is a first-hand hardware observation, not a repeated assumption —
an earlier version of this section understated that. Working hypothesis
(from that doc): the driver's very first probe-time write races the
codec's own power-up sequence, and this is present in the vendor's own
shipped firmware, i.e. not something this project broke or needs to fix
for the probe-time burst specifically.

**Root cause found (2026-07-14): the I2C address itself was wrong.**
Disassembled the vendor's own shipped userspace audio-control code —
the code that actually drives the chip at runtime, entirely bypassing
this project's kernel ALSA driver — to get ground truth on the real
wire address:

- `libMsnSound.so`'s `Sound_BD37033::Sound_BD37033()` constructs its
  `I2COperator` with `bus=2, addr=0x80` (`mov r1, #2` / `mov r2, #128`
  immediately before the `bl I2COperator::I2COperator`) — matches the
  `Open I2C Device: 2 "80"` line seen in every live boot log.
- `libMsnCommons.so`'s `arki2c_open(bus, addr)` does
  `ioctl(fd, I2C_SLAVE, addr >> 1)` (`asr r2, r6, #1` then `movw r1,
  #1795` = `0x703` = `I2C_SLAVE`, then `bl ioctl`) — i.e. the real
  7-bit wire address is `0x80 >> 1 = 0x40`.

**`0x40` is the correct address** — matching the BD37033 datasheet's
public 7-bit address, not `0x41`. The `0x41` this project had been
using came only from disassembling *stock's kernel* board file
(`i2c_board_info` struct) plus an `i2c-scan` `XX` marker on the
resulting sysfs entry — both just prove a kernel `i2c_client` got
*registered* at `0x41`, never that the physical chip *answers* there
(the same class of mistake the `XX`-marker note elsewhere in this repo
already flags). The vendor's own real, shipped, presumably-working
runtime control path never goes through that kernel client at all — it
opens `/dev/i2c-2` directly and issues raw ioctls at `0x40`.

This also fully explains the `bd37033_write_byte timeout` seen
throughout this entire investigation, including in stock's own kernel
dmesg (`I2C_GPIO0_LCD_PIN_CONFLICT.md`): stock's *kernel-level*
`i2c_client` was always misaddressed too — it just never mattered,
because stock's real audio control bypasses the kernel driver entirely.

**Fix applied:** `drv_bd37033@41`/`reg = <0x41>` changed to
`drv_bd37033@40`/`reg = <0x40>` in `ark1668_limcet_p305.dts` (and
mirrored in `Limcet Hardware/ark1668-limcet-prado.dts`). Kernel DTB
rebuilt clean.

## Live hardware re-test of the address fix (2026-07-14) — still failing, address was not the (whole) story

Flashed the `0x40`-corrected image and re-tested on real hardware.

**Confirmed the new image was actually running:**
`ls /sys/bus/i2c/devices/ | grep '2-004'` → `2-0040` (not `2-0041`) —
the kernel really did bind the BD37033 `i2c_client` at the corrected
address this time, not a stale/unflashed image.

**Result: `bd37033_write_byte timeout` still appears, same ~16-line
burst as before the address fix.** `tools/audio-test/audio-test.sh`
also still shows no ALSA card in `/proc/asound/cards` on this same run
(unresolved, separate anomaly — see the script's own step 1; step 3's
mixer-control list proves a card exists moments later in the same run,
so this isn't a real "card never registered" state, more likely a
`/proc/asound/cards` population quirk specific to that check — not yet
root-caused).

Fixed two stale references in `tools/audio-test/audio-test.sh` found
during this: step 2 was still grepping `i2c-scan` output for `0x41`
(now `0x40`), and step 4's `| head -1` failed with `head: not found` on
this rootfs's busybox build (swapped for `sed -n '1p'`). Also: ALSA's
simple-mixer layer collapses `SOC_SINGLE_EXT("PA Volume", ...)` into a
bare `'PA'` simple control (like `"Master Playback Volume"` →
`"Master"`) — step 3's grep now matches both forms. With these fixes,
step 3 correctly shows the full BD37033 control set exists and is
bound (`PA`, `PA Mute`, `PA Fader-*`, `EQ Bass/Middle/Treble`, etc — the
`aux-devs` fix confirmed working end-to-end).

### `i2c-gpio-1`'s SDA (GPIO9) has the same LCD pin conflict as `i2c-gpio-0`

`I2C_GPIO0_LCD_PIN_CONFLICT.md` already proved `i2c-gpio-0` (GPIO2/3)
shares physical pins with the LCD's RGB888 data bus, and explicitly
flagged as an **open, never-completed item**: every other pin
assignment needed the same check before trusting it. That check was
never done for `i2c-gpio-1` (GPIO9/GPIO121, BD37033's bus) until now:

```
# cat /sys/kernel/debug/pinctrl/e4900000.pinctrl/pinmux-pins | grep -E "pin 9 |pin 121 "
pin 9 (pin9): e0500000.lcd (GPIO UNCLAIMED) function lcd group lcd-rgb-0
pin 121 (pin121): (MUX UNCLAIMED) e4600060.gpio:121
```

GPIO9 (SDA) is muxed to the LCD's RGB888 group — specifically `r7`, the
**most significant bit of the red channel** (`ark1668-pinctrl.dtsi`'s
`pinctrl_lcd_rgb888`/`lcd-rgb-0` group, `ARK_PBANK_0` offset 9). Being
an MSB rather than a droppable LSB makes it less likely this is a
stale/overbroad pinctrl claim and more likely a genuine, actively-toggling
electrical conflict on this board. SCL (pin 121) is unaffected, cleanly
owned as plain GPIO. Full details and cross-reference in
`I2C_GPIO0_LCD_PIN_CONFLICT.md`'s new "`i2c-gpio-1` has the identical
conflict" section.

This supersedes the earlier "probe-time race with the codec's power-up
sequence, apparently benign" theory (recorded above) — that theory was
formed without ever checking pin 9's live mux state, and a genuine
pin-sharing conflict is a far more complete explanation: it accounts
for every single write failing, not just ones at probe time.

### Unbind test — inconclusive, LCD driver's software claim released but timeout persists

Tested directly: unbound the LCD framebuffer driver
(`echo e0500000.lcd > /sys/bus/platform/drivers/ark1668_lcdfb/unbind`)
to release its pinctrl claim on pin 9, then re-checked:

```
pin 9 (pin9): (MUX UNCLAIMED) (GPIO UNCLAIMED)
```

Software-level claim is gone. But issuing a real BD37033 write
afterward (`amixer sset PA 10` variants — tried plain, `10%`, `-c 0`,
and `cset numid=...`, all of which actually reached the driver this
time) **still produces `bd37033_write_byte timeout` on every attempt.**

This is a real, clean negative result, but doesn't disprove the pin-
conflict theory by itself: `platform_driver.remove()` tearing down
software/pinctrl bookkeeping does **not** guarantee the SoC's physical
pinmux *register* gets reprogrammed back to a default/GPIO state —
many drivers never touch that register again after their initial
`pinctrl-0` apply at probe time. So "unclaimed" in the debug view may
only mean nobody's *tracking* the pin anymore, not that the pad is
electrically free.

**To settle this, need the raw hardware register value, not pinctrl's
software view.** Traced the exact register from `pinctrl-ark.c`:

- Our board's `compatible = "arkmicro,ark1668-pinctrl"` takes the
  hardcoded-map branch (`ark1668_pin_map`), not the DT-configurable
  `"arkmicro,arke-pinctrl"` branch that reads a `pad-reg-offset`
  property — our board has no such property (confirmed: absent from
  `ark1668.dtsi`), so `pad_reg_offset` stays `0` (zeroed on alloc).
- `PAD_NUMS_PER_REG = 10`, `BITS_PER_PAD = 3` (mask `0x7`).
- `PIN_REG_OFFSET(9) = (9/10)*4 = 0`, `PIN_BIT_OFFSET(9) = (9%10)*3 = 27`.
- Register address = `regbase (0xe4900000) + pad_reg_offset (0) +
  PIN_REG_OFFSET(9) (0)` = **`0xe4900000`**, 3-bit mux field at
  **bits [29:27]**.
- Mux values (`include/dt-bindings/pinctrl/ark-pinfunc.h`): `ARK_PVAL_1
  = 1` is `lcd-rgb-0` (what `r7` is set to). Also found a **third,
  unused pinctrl state** in the vendor's reference DTS,
  `pinctrl_lcd_hi_impedance`/`lcd-hi-impedance`
  (`ark1668e-pinctrl.dtsi`), which sets the same pins to `ARK_PVAL_5`
  — an explicit tri-state/hi-Z mode for exactly this class of pin,
  though not wired up as a selectable state anywhere in this board's
  own DTS.

**Correction to the register math above:** the generic
`PIN_REG_OFFSET`/`PIN_BIT_OFFSET` formula (`PAD_NUMS_PER_REG=10`,
3-bit fields) only applies to the DT-configurable `"arkmicro,arke-pinctrl"`
branch. Our board's compatible string (`"arkmicro,ark1668-pinctrl"`)
uses a separate, explicit per-pin lookup table instead —
`ark1668_pin_map[]` — where array index N is pin N directly, each entry
an explicit `{reg, offset, mask}`. Pin 9's real entry is
**`{0x1c0, 28, 0xf}`** — a **4-bit** field at bits `[31:28]` of register
`0xe4900000 + 0x1c0 = 0xe49001c0`, not the 3-bit-at-0xe4900000 guess
above. Built and verified a small tool for this,
`tools/pin-dump/pin-dump.sh` — reads every one of the 131 pins' live
mux value via `devmem`, using this exact table extracted from
`pinctrl-ark.c`. Sanity-tested locally against a stub `devmem` (pin 9
correctly decodes to `PVAL=1`) before running on real hardware.

**Live result, our board:** `pin 9: reg=0x1c0 offset=28 mask=0xf ->
PVAL=1` — confirms the debugfs `pinmux-pins` read: the *hardware*
register, not just pinctrl's software bookkeeping, really is still set
to `lcd-rgb-0` even after unbinding `ark1668_lcdfb`. So the earlier
"unbind released the software claim but the timeout persisted" result
is now explained: unbinding a `platform_driver` doesn't reprogram this
register, as suspected — `ark1668_lcdfb`'s `remove()` never touches it,
so the pad genuinely never left LCD mode despite the driver being gone.

**Then ran the same tool on real stock hardware** (`devmem` is already
in stock's own busybox — no payload transplant needed, just copied the
script over via the existing `msn_autocopy` root-shell method) for a
byte-for-byte comparison (`docs/pindump stock.txt`, decoded into function
names via `tools/pin-dump/decode-pins.py` against `ark1668-pinctrl.dtsi`'s
named pin groups — see `docs/pindump stock annotated.txt`):

```
pin   9: reg=0x1c0 offset=28 mask=0xf -> PVAL=1
pin 121: reg=0x1f0 offset= 4 mask=0x1 -> PVAL=0
```

**Stock shows the exact same `PVAL=1` (LCD-muxed) on pin 9.** This is
not a state unique to our reconstruction or to having unbound a driver
— it's stock's normal, at-rest hardware state too, on a unit whose
audio is presumed to work in the field. A pin permanently stuck in the
same LCD-muxed state on *both* the working and non-working systems
can't, by itself, be the deciding factor between them — this
significantly weakens the "static pin conflict is the root cause"
theory as a complete explanation, even though the conflict itself
(pin 9 == LCD r7, confirmed at the hardware register level on both
boards) is real and not in question.

**What's left, still open:**
- We have never independently, audibly confirmed that stock's own
  BD37033 volume/EQ control actually produces a real sound change in
  the field — every piece of evidence so far (`i2c-scan` `XX` markers,
  kernel `i2c_client` registration, this pin dump) has been consistent
  with either "it genuinely works despite the shared pin" or "it never
  really worked either and nobody's audibly checked" per this project's
  own standing caution about weak boot-log/register evidence
  (`feedback_bootlog_evidence_weak`). This is now the single most
  valuable unresolved question — if stock hardware's own volume knob
  doesn't produce an audible change either, the "pin conflict" question
  is moot and something else entirely (chip populated but dead, wrong
  address still somehow, wiring damage) is more likely.
- If stock genuinely does work audibly, the shared static pin value
  implies the real mechanism must be *dynamic* — e.g. something
  switches the mux briefly (to `lcd-hi-impedance`/`ARK_PVAL_5` or
  similar) only around the moment of an actual I2C transaction, then
  switches back — rather than the pin ever sitting in a genuinely free
  state. Nothing found so far in either kernel points to code that does
  this; would need runtime instrumentation (e.g. polling this same
  register via `devmem` in a tight loop while a known-working stock
  volume change is triggered) to catch it in the act, if it exists.
- Alternatively the LCD's hardware IP may not continuously drive `r7`
  regardless of mux setting (e.g. only during active-display, not
  blanking/porch intervals) — bit-banged I2C at `i2c-gpio,delay-us=6`
  is slow enough that blanking-interval timing could matter. Not yet
  tested; would need to correlate LCD timing (`hactive`/`vactive`/
  porches in `ark1668_limcet_p305.dts`'s `display0` node) against
  transaction duration.


## First genuine audible confirmation, and a new architectural lead: `softmaster` (2026-07-14)

**Milestone: audio is now confirmed audibly working on stock hardware for
the first time in this entire investigation** — `aplay -D hw:0,0 -f
S16_LE -r 44100 -c 2 /dev/urandom` (and the `softvol`-routed variant)
produce real, audible noise through the physical speakers on stock. Every
prior "confirmation" in this project (boot logs, `i2c-scan` `XX` markers,
register dumps) was explicitly flagged as *not* proof of correct
operation per this project's own standing caution
(`feedback_bootlog_evidence_weak`) — this is the first time that caveat
has actually been resolved with a real listen. Confirms the physical
speaker wiring, amp, and SDDAC data path are genuine and functional on
this exact unit.

**Not yet tested: whether any volume control actually changes the audible
level.** Sound plays at a fixed level so far; `amixer sset` while
playback runs hasn't been tried yet (needs backgrounding — `aplay ... &`
then `amixer sset ...` in the same shell, since only one terminal is
available on this unit). This is the next, decisive test — pending.

### `softmaster`: a third, previously-unnoticed control path

Captured `amixer controls` (the raw/uncollapsed control list, unlike
`scontrols`'s simple-mixer view) on stock (`docs/audio log stock.txt`).
Confirms:

- Stock uses **one unified card**, `ARK-SDDAC`, with BD37033's controls
  bound onto it alongside SDDAC's own — architecturally the same pattern
  as this project's `aux-devs` fix, not just a workaround that happens to
  produce a similar-looking result.
- The raw name really is `'PA Volume'` (numid=23), confirming
  `scontrols`' bare `'PA'` is ALSA's simple-mixer collapsing
  `"<X> Volume"` into `"<X>"` — independently confirms that theory from
  earlier in this doc, not just plausible.
- **Six controls never seen in any of this project's own captures:
  `softmaster`, `softmaster1`-`softmaster5`.** Traced to `asound.conf`'s
  `softvol`/`softvol1`-`softvol5` PCM plugins — confirmed byte-identical
  between stock and this project's own `asound.conf` (only an unrelated
  `max_dB` value differs on one entry), so this isn't a stock-only
  feature we're missing, it's a **userspace ALSA-lib control that's
  created lazily**, the first time some process actually opens that
  specific `pcm.softvolN` device — not a persistent kernel control
  visible at boot. Every one of this project's own captures was taken
  before anything had opened it, which is why it never showed up before.

`pcm.softvol`'s slave is `dmix` (→ `hw:0,0`, SDDAC), applying `-51dB` to
`0dB` of **digital gain in userspace**, upstream of both SDDAC's own
hardware volume and BD37033 entirely — and `asound.conf`'s default
playback PCM routes through it. This makes `softmaster` a strong
candidate for what the factory head unit's actual volume knob/UI
controls in practice: a pure software attenuator that doesn't depend on
any I2C-controlled chip cooperating, which would elegantly explain
reliable field volume control regardless of whatever's actually going on
with BD37033's I2C path.

**Next steps:**
- [ ] With playback backgrounded, test `amixer sset softmaster <n>` for
      an audible change — the single most decisive open test right now,
      and should be tried before `PA Volume` since it's the more likely
      candidate for what real-world volume control actually uses.
- [ ] Then test `PA Volume` the same way, to separately determine
      whether BD37033 itself does anything audible.
- [ ] Then test `Left/Right Playback Volume` (SDDAC's own hardware
      volume, bypassing `softvol` via `hw:0,0` directly) for
      completeness — three independent, testable volume paths now
      identified on this one card.

## Architecture pivot: playback routed through external CS4334, not internal SDDAC (2026-07-16)

All prior sections of this doc assumed playback goes through the
internal `ark_sddac` sigma-delta DAC (`i2s_dac` DAI link). That
assumption was wrong for this board. `docs/KERNEL_REFERENCE.md`
(`ark_cs4334_dev` in the stock module table) and stock's own boot log
(`docs/logs/dmesg live device kernel 3.4 dmeg.txt`: `asoc: cs4334 <->
ark_i2s_dev.1 mapping ok`) both show stock's *actual* playback path uses
an **external Cirrus Logic CS4334 I2S DAC**, driven off the SoC's second
I2S instance (`ark_i2s_dev.1`, the "external I2S2" block) — not the
internal SDDAC. Everything through "First genuine audible confirmation"
above was real, but was exercising a DAC stock apparently doesn't
actually use for its own playback path in this configuration; that
internal-SDDAC audible test result doesn't tell us anything about
whether the CS4334 path stock really uses will work.

Commit `2f518ca4f` (build-tree repo, 2026-07-16) re-pointed the DTS at
CS4334:

- Playback `dai-link@0` now uses `i2s_adc` (the DT node with
  `external-i2s;`, physical base `0xe8200000` — this is the SoC's
  second/external I2S instance, matching stock's `ark_i2s_dev.1`) as CPU
  DAI, and a new `cs4334_codec` node (`compatible =
  "arkmicro,ark1668e_cs4334_codec"`) as codec DAI.
- Capture `dai-link@1` now uses the internal `i2s_dac`/`sdadc` pair.
- `ark1668_i2s.c`'s `ark_i2s_startup()` gained pinmux writes
  (`ARK_SYS_PAD_CTRL09`/`0A`/`0C`/`06`) specific to the external I2S2
  block when `physical_base == 0xe8200000`, plus a soft-reset release at
  `SYS_BASE (0xe4900000) + 0x6c` (bit 0 for I2S2, bit 2 for I2S1) in
  `ark1668_i2s_drv_probe()`.

Live result after this commit: **the CS4334 platform device now probes
and the ASoC card links up** ("chip now detected" — i.e. the DAI/card
registration stock hardware also gets), but **no audible output**.

### CS4334 codec driver itself is not the problem — verified against stock disassembly

Disassembled stock's `cs4334_*` functions from `vmlinux.elf`
(`arm-linux-gnueabihf-objdump -C -d`, symbols unstripped,
`cs4334_startup`/`_hw_params`/`_set_dai_sysclk`/`_set_dai_fmt` all at
`0x802f6604`-`0x802f6624`, `cs4334_probe` at `0x802f6680`). **Every one
of these is a trivial stub in stock too** — `mov r0,#0; bx lr`, or a bare
`kmem_cache_alloc` for private data with no register/GPIO access
anywhere. This project's own `linux/sound/soc/codecs/cs4334.c` is the
same kind of stub (`DBG()`-only bodies). **This is expected, not a bug**:
CS4334 is a control-less, hardware-strapped serial DAC (no I2C/SPI
register interface) — the codec driver's only job is to exist so ASoC's
DAI-link machinery has something to bind to. The CS4334 codec side is
therefore not a suspect for the "no audio" symptom; whatever's wrong is
upstream, in how the SoC's external I2S2 peripheral itself is clocked/
enabled.

(Also checked `ark1668e_audio_codec.c`/`ark1668e_i2s.c` in the same
directory — these implement a real DAPM mute/GPIO-control codec, but
they're gated behind `CONFIG_SND_SOC_ARK1668E_INTERNAL_ADAC`/
`SOC_ARK1668E`, a different SoC variant than this board's
`SOC_ARK1668`. Dead code for this build, not a missing wire-up.)

### Root cause candidate found: a second, previously-untouched SoC register block gates the external I2S2 clock domain

Disassembled stock's `ark_i2s_init_cfg()` (`0x802f5b4c`, called from
`ark_i2s_startup()` — the vendor equivalent of the pinmux/reset work this
project's own `ark_i2s_startup()` reimplemented inline) and the
`setup_i2s2()` helper it calls (`0x802f5aa0`) for the external-I2S
(`id==1`) path.

Both functions do register I/O against a **third address region**, at
constant `0xf6300000` (`movt r3, #0xf630`, offsets `0x1d8`-`0x1f0`) —
distinct from both `SYS_BASE` (`0xe4900000`, pinctrl/pad-config, already
patched via the soft-reset-release change above) and the I2S
peripheral's own register block (`i2s->base`, `0xe4000000`/`0xe8200000`).
`0xf6300000` isn't a physical address — decoded stock's io-descriptor
table (`ark1680_map_io` → `iotable_init(0x805b376c, 19)` →
`arm-linux-gnueabihf-objdump -s` on that literal-pool range) and it
resolves to **physical `0xe4a00000`**, length `0x1000` — the exact page
`ark1668.dtsi` already maps as `timer@e4a00000`. The offsets stock
touches (`0x1d8`-`0x1f4`) sit well above where a simple timer's own
counter/compare registers would live, so this is read as a shared
SoC-level page hosting both the timer *and* clock-gate/mux control bits
for other blocks, audio included — a common pattern on this era of SoC
(cf. `SYS_BASE`'s page already being shared between pinmux and the I2S
soft-reset bits used above).

**Exact bits, decoded from the disassembly:**
- `setup_i2s2()` (external I2S2 / CS4334 path only): `+0x1e4 |=
  0x3f000000`, `+0x1e8 |= 0x700`, `+0x1f0 |= 0x400`.
- `ark_i2s_init_cfg(..., id=1)` (external, after calling `setup_i2s2()`):
  additionally `+0x1d8 &= ~0x80000000`.
- `ark_i2s_init_cfg(..., id=0)` (internal I2S1/SDDAC path): only `+0x1f0
  |= 0x400` — the one bit common to both paths, consistent with it being
  a shared "audio block clock enable" rather than something instance-
  specific.

**None of this is touched anywhere in this project's kernel** — neither
the pre-existing pinmux/soft-reset patch nor anything earlier in this
doc's history reaches physical `0xe4a00000` at all. This fully explains
the "probes clean, DMA/ASoC layer reports success, chip detected, but
silent" symptom in a way nothing upstream of it does: if the external
I2S2 block's own clock/mux domain is still gated off at this SoC-global
level, the peripheral itself never asserts real bit-clock/data toward
CS4334 regardless of what the pad-mux or soft-reset-release bits (both
already fixed) or the ALSA/DMA layer (probe/hw_params/trigger, already
confirmed clean from the earlier SDDAC-path DMA work) are doing.

**Fix applied (2026-07-16, not yet hardware-tested):**
`ark1668_i2s_drv_probe()` in `ark1668_i2s.c` now `ioremap()`s
`0xe4a00000` (separately from `SYS_BASE`) and, for the external-I2S2
instance (`mem->start == 0xe8200000`), applies the four bits above; for
the internal-I2S1 instance (`mem->start == 0xe4000000`), applies just
the shared `+0x1f0 |= 0x400` bit. Placed in probe (one-time, matches
stock's own call site inside `ark_i2s_startup`/`init_cfg`, which in
practice only needs to run once per instance) rather than fleshing out
the pre-existing-but-dead `ark_i2s_init_cfg()`/`setup_i2s2()` stub
functions already in this file (found empty and unused during this
investigation — leftover vendor skeleton, called from nowhere; left
as-is, not part of this fix). Compiles clean (`make ... zImage`, only
pre-existing unrelated warnings). Kernel rebuild in progress as of this
writing.

**Caution — register semantics inferred, not confirmed via documentation
or datasheet.** The `0xf6300000`→`0xe4a00000` physical resolution is
solid (io-descriptor table, byte-exact). The *meaning* of bits
`0x3f000000`/`0x700`/`0x400`/`bit31` is inferred purely from "stock sets
them, nothing else in the driver graph does, and they're on a page also
used for the timer" — plausible (clock-gate/mux-select bits are exactly
the kind of thing vendors tuck into a spare corner of an otherwise-
unrelated peripheral's register page) but **not decoded against a
register-level datasheet**. This page is shared with the live timer
peripheral (`arkmicro,ark-timer`, currently in active use as the kernel
clocksource) — the offsets touched (`0x1d8`-`0x1f4`) are far from where
a low counter/compare register would typically sit, but this has not
been independently confirmed safe. **Needs a live boot test before
trusting further**: watch for clocksource instability/hangs (a broken
timer clocksource would likely manifest as boot hangs or wildly wrong
`jiffies`-based timing, not silent audio failure) in addition to
checking for actual sound.

**Live-tested (2026-07-16): no change.** Flashed and booted the kernel
with the `0xe4a00000` clock-gate patch above (CS4334 still the primary
playback dai-link at this point). Still silent — but the boot-time
"pop" (present since well before today, see "Test 3" earlier in this
doc) *was* heard, so the internal SDDAC block itself was still getting
probed/initialized at boot even with CS4334 as the nominal playback
link — a hint something was off in the whole premise, not just the
clock-gate bits.

## CS4334 pivot reverted — stock's own aplay -l proves SDDAC is the real (only) playback path (2026-07-16)

Live `aplay -l` on **stock** hardware (`docs/logs/audio log stock.txt`):

```
card 0: ARKSDDAC [ARK-SDDAC], device 0: SDDAC sddac-hifi-0 []
```

**Exactly one playback device, and it's SDDAC — not CS4334.** Stock's
`dmesg` (`docs/logs/dmesg live device kernel 3.4 dmeg.txt`) does show
both dai-links mapping at the ASoC level (`asoc: sddac-hifi <->
ark_i2s_dev.0 mapping ok` *and* `asoc: cs4334 <-> ark_i2s_dev.1 mapping
ok`), so CS4334 is a real, board-file-registered dai-link on stock too —
but it evidently never produces a usable PCM device, since `aplay -l`
never lists a second one. This is the same category of mistake this
project has hit before and has a standing caution about
(`feedback_bootlog_evidence_weak`): a "mapping ok" printk is DAI-link
*binding* success, not proof of a working playback path — exactly like
an I2C `XX` marker proving a driver bound, not that the chip answers.
The 2026-07-16 pivot to CS4334 (the "Architecture pivot" section above)
was built on that log line plus a `KERNEL_REFERENCE.md` module-table
entry, without checking `aplay -l` first. It shouldn't have been trusted
over the card's actual live playback-device enumeration, and the
live no-audio re-test (immediately above) is consistent with that: it's
very plausible CS4334's dai-link on stock is present in the driver graph
but genuinely dead for playback (why is still unknown — possibly a
capture-only or disabled-stream config on that specific dai-link in
stock's board file that this project's DTS pivot didn't replicate
either), same class of "registered but never actually exercised" vendor
code already found elsewhere in this firmware (BD37033, the
`sendSoundData()` fallback branch).

**Reverted** (`ark1668_limcet_p305.dts`): `dai-link@0` (playback) back
to `i2s_dac` (internal, physical `0xe4000000`) + `sddac` — the exact
configuration that produced this investigation's only prior confirmed
audible result ("First genuine audible confirmation... (2026-07-14)"
above). `dai-link@1` (capture) back to `i2s_adc` + `sdadc`. Also set
`simple-audio-card,name = "ARK-SDDAC"` and
`simple-audio-card,stream-name = "sddac-hifi"` on the playback link to
match stock's card/device naming exactly (cosmetic/diagnostic parity
only, not functionally required). `cs4334_codec` DT node left defined
but unreferenced by any dai-link — harmless, and stock's own driver
graph still includes an equivalent dead link, so this isn't asymmetric
with stock. The `0xe4a00000` clock-gate patch in `ark1668_i2s.c` is kept
(keyed off physical base address of the I2S *hardware instance*, not
which DT node/purpose is assigned to it, so it still applies correctly
to whichever direction now uses the external I2S2 instance — capture,
post-revert). Kernel rebuild in progress.

**Open question worth confirming, not yet investigated:** why does
CS4334's dai-link map fine but never expose a PCM on stock? If it turns
out to be genuinely inert on stock too (not just on this
reconstruction), that closes the loop cleanly. If instead it's a
capture-only or otherwise-restricted link on stock that this project's
DTS pivot got wrong in a fixable way, revisiting it later could still be
worthwhile — but that's speculative, and SDDAC being stock's actual
working path makes it the correct priority now regardless.

**Next steps:**
- [ ] Flash the reverted+rebuilt kernel, confirm no boot regression.
- [ ] Live audible test: `aplay -l` to confirm device index/naming now
      matches stock (`ARK-SDDAC`/`SDDAC sddac-hifi-0`), then `aplay -D
      hw:0,0 -f S16_LE -r 44100 -c 2 /dev/urandom` — this exact command
      on this exact dai-link configuration was the one confirmed-audible
      result in this whole investigation (2026-07-14, before the CS4334
      detour), so if it stays silent now, something regressed between
      then and now (the cyclic-DMA fixes, TDMAENA/RDMAENA, or something
      in today's soft-reset/clock-gate additions) rather than being a
      new problem to diagnose from scratch.
- [ ] If audible: retest `start_msn`/`MsnCoreApp` end-to-end, and the
      three volume-control paths (`softmaster`, `PA Volume`,
      `Left/Right Playback Volume`) queued up before the CS4334 detour.

## Live-tested (2026-07-16): reverted SDDAC path still silent — found the real blocker, printk starvation from the DMA debug instrumentation itself

Flashed the reverted build. Still no audio, but `aplay` now loops
cleanly: `dw_dma_cyclic_start ret=0` (channel really starts) followed
almost immediately (0.08-0.13ms later — far faster than one real
44.1kHz/1024-frame period) by `dwc_terminate_all` and an ALSA
`underrun!!!`, repeating every attempt. **Zero `dw_dma_interrupt`/
`dwc_handle_cyclic` lines appear during any of these live attempts.**

The decisive clue: hitting Ctrl-C immediately produced a *burst* of
`dw_dma_interrupt status=0x2` / `dwc_handle_cyclic chan_mask=0x40
block=0x40` lines, back-to-back, with no new prep/start calls in
between. Real hardware interrupts were firing (or at least pending) the
whole time — they just weren't being serviced during actual playback,
only draining once the process died.

**Root cause: the `ARKDMA_DBG` printk instrumentation added on
2026-07-13 to diagnose the *original* cyclic-DMA wiring (see "Live
IRQ-trace test run" above) was still live in this build and never
removed once its job was done.** Two of its printk sites are especially
bad:

- `dwc_handle_cyclic()`'s printk ran **"with dwc->lock held and all
  DMAC interrupts disabled"** (the function's own pre-existing comment)
  — i.e. every period-boundary interrupt blocked on a synchronous
  115200-baud UART write while IRQs were masked.
- `dwc_terminate_all()`'s cyclic path called **`dump_stack()`** (a
  15-30 line kernel backtrace) unconditionally on every single
  terminate — and terminate fires on every underrun, so every failed
  ~0.1ms attempt was immediately followed by a full stack dump over
  serial.

On this single-core SoC (`BogoMIPS 457`), that's enough synchronous
serial I/O to starve the CPU past the point where a ~23ms audio period
window can complete — ALSA's own software xrun/timeout logic gives up
and calls `dwc_terminate_all()` (triggering *another* `dump_stack()`)
long before the real DMA hardware ever gets a quiet enough window to
raise and have its interrupt serviced, even though the transfer really
is armed and running. This is a self-inflicted instrumentation bug, not
a hardware or clocking problem — consistent with the interrupts
existing (RAW status latched) but only draining in a backlog once the
debug-logging pressure stopped (process killed).

**Fix applied (2026-07-16):** removed all `ARKDMA_DBG` printks and the
`dump_stack()` call from `drivers/dma/ark-dma.c`
(`dwc_tx_submit`/`dwc_handle_cyclic`/`dw_dma_interrupt`/
`dwc_prep_dma_cyclic`/`dwc_terminate_all`/`dwc_issue_pending`) — pure
debug-log removal, no functional/logic changes to any of the cyclic-DMA
code paths those functions implement. Compiles clean. Kernel rebuild in
progress.

**Next steps:**
- [ ] Flash and re-test `aplay -D hw:0,0 -f S16_LE -r 44100 -c 2
      /dev/urandom` — if the printk-starvation theory is correct this
      should now run continuously without looping/underrunning, and
      produce audible noise.
- [ ] If still silent but no longer looping/underrunning: capture a
      clean `dmesg` during a several-second run and check whether
      `dw_dma_interrupt`/`dwc_handle_cyclic` fire at a steady ~23ms
      cadence (22 periods per buffer) — that would mean the DMA path is
      now fully healthy and the remaining problem is downstream
      (SDDAC/amp muted, wrong I2S format, etc.), not DMA/IRQ timing.
- [ ] If it still loops/underruns even without the debug prints: the
      printk-starvation theory is wrong or incomplete — fall back to the
      original live IRQ-trace methodology (this time without leaving the
      instrumentation in place afterward) to re-isolate where the
      remaining latency/blocking is coming from.

## Live-tested (2026-07-16): printk removal alone did not fix it — found the real bug, an inverted residue calculation

Flashed the printk-stripped build. `aplay -D hw:0,0 -f S16_LE -r 44100
-c 2 /dev/urandom` still underruns continuously — but now in a *tight*
loop with no debug output slowing it down: dozens of `underrun!!! (at
least 0.02-0.07 ms long)` lines per second, each one **far faster than
the ~23ms real period** (`set_params` reports `period_time=23219`,
`chunk_size=1024`). Printk starvation was real (confirmed by the
delayed interrupt burst after Ctrl-C in the previous test) but was
masking a second, actual logic bug underneath — removing the prints
didn't fix playback, it just made the same broken loop run faster.

**Root cause found in `dwc_tx_status()`'s cyclic branch** (added
2026-07-13, see "Live IRQ-trace test run" above — this is the same code
that fixed the *previous* cyclic bug, but introduced this one in the
process). It set:

```c
residue = dwc_get_sent(dwc);
```

`dwc_get_sent()` returns **bytes already transferred** out of the
current period — its own name and comment say so, and the pre-existing
one-shot path (`dwc_get_residue()`, a few lines above in the same file)
uses it correctly: `residue = desc->residue - dwc_get_sent(dwc)` —
i.e. *subtracted from* the total to get what's left. The cyclic branch
instead assigned the raw "sent" count directly as the residue, with no
subtraction. `dma_set_residue()` feeds this to the generic
`dmaengine_pcm` framework's `.pointer()` callback (what
`ark1668_i2s.c`'s playback path polls to report the ALSA hardware
pointer) — since residue is supposed to mean "bytes *remaining*", and
right after a period starts almost nothing has been sent yet, this bug
reported a near-zero residue **immediately**, which reads as "the
hardware pointer is already at/past the end of the buffer" — an
instant, spurious underrun on literally every single check, before any
real data could ever move. This fully explains the observed pattern:
`dw_dma_cyclic_start()` genuinely succeeds (`ret=0`, confirmed
repeatedly across every capture in this investigation) and real
interrupts do fire (confirmed by the Ctrl-C backlog in the previous
test) — the transfer itself was never the problem; every single
`.pointer()` poll was lying about where the hardware actually was.

**Fix applied (2026-07-16):** added a `period_len` field to `struct
dw_cyclic_desc` (`ark-dma.h`), populated from `dw_dma_cyclic_prep()`'s
existing `period_len` parameter, and changed the cyclic branch of
`dwc_tx_status()` to `residue = period_len - sent` (clamped to 0),
matching the subtraction pattern the one-shot path already used
correctly. Compiles clean. Kernel rebuild in progress.

**Next steps:**
- [ ] Flash and re-test `aplay -D hw:0,0 -f S16_LE -r 44100 -c 2
      /dev/urandom` — expect either sustained playback with real
      audible noise, or (if still broken) underruns no faster than the
      real ~23ms period instead of sub-millisecond, which would at
      least prove the residue fix is directionally correct even if
      something else still needs tuning.
- [ ] If audible: this closes the entire cyclic-DMA chapter of this
      investigation (started 2026-07-13) as fully resolved. Move on to
      re-testing `start_msn`/`MsnCoreApp` and the volume-control paths
      queued up earlier.

**Live-tested (2026-07-16): residue fix confirmed the DMA path is fully
healthy — `aplay -D hw:0,0 -f S16_LE -r 44100 -c 2 -d 5 /dev/urandom`
now runs clean for the full 5 seconds, no underruns at all.** Still
completely silent though (just the known startup pop) — this cleanly
separates the two problems that had been tangled together all session:
the digital I2S/DMA path (now provably correct) and something further
downstream in the analog chain (still broken). BD37033 remains
confirmed dead (`audio-test.sh`: no I2C ACK at 0x40, `PA` mixer value
doesn't even round-trip in ALSA's own cache) but per the "no volume
control in the UI, only overall in Settings" clarification and
MsnCoreApp not currently starting at all, BD37033 isn't the immediate
blocker for getting *any* signal out — SDDAC's own analog output is.

## Found the analog blocker: DAC/vref left powered down during playback (2026-07-16)

`ARK_I2SSDDAC_SACR0` has `DAC_PD` (bit 22) and `VREF_PD` (bit 21) —
power-down control for the DAC and its voltage reference. The
**capture** branch of `ark_i2s_startup()` explicitly sets both (correct
— DAC/vref aren't needed while recording) as part of its clear-then-set
sequence. The **playback** branch, immediately above it in the same
function, only ever `|=`s in unrelated bits (`I2SEN`/`TFTH`/
`SARADC_DATA`, all via read-modify-write) — it never clears `DAC_PD`/
`VREF_PD` back off. So whatever those two bits were left at (from a
prior capture stream, or possibly the hardware's power-on-reset
default) stays untouched through every playback attempt this entire
investigation has run. This fully explains the "digitally perfect,
completely silent" result: no amount of correct I2S/DMA/DMA-IRQ timing
can produce analog output if the DAC and its reference are powered off
at the analog stage itself. Same category as this project's other
found-late incomplete-vendor-stub bugs (TDMAENA/RDMAENA never
re-enabled, `device_prep_dma_cyclic` never wired up) — this one was
simply invisible until the DMA-timing bugs hiding it were fixed first.

**Fix applied (2026-07-16):** playback branch of `ark_i2s_startup()`
(`ark1668_i2s.c`) now explicitly clears `DAC_PD`/`VREF_PD` in the same
read-modify-write that sets `I2SEN`/`TFTH`/`SARADC_DATA`. Kernel rebuild
in progress.

**Next steps:**
- [ ] Flash and re-test `aplay -D hw:0,0 -f S16_LE -r 44100 -c 2
      /dev/urandom` — this is the single most likely fix to finally
      produce real audible noise, since it directly targets the last
      unexplained gap between "digital path proven correct" and
      "complete silence."
- [ ] If still silent: check `VREF_PD`'s wider blast radius — it may be
      shared between the DAC and SDADC's own reference (worth confirming
      capture still works correctly after this change, i.e. this fix
      didn't accidentally leave something in a bad state for the ADC
      path), and re-examine `left-volume`/`right-volume` DTS values
      (currently `112`) as the next most likely remaining attenuator.

## Found a real regression: `SoundType=3` fix had silently reverted to stock's `0` (2026-07-16)

Live crash capture from `MsnCoreApp` (`ark_display: ARKDISP_SET_VDE_CFG
-> ...` immediately followed by `SoundAdapter Create Failed, Not
Support ICType: 0`, `Load App Plugin 403 "/usr/lib/libMsnSound.so"`,
then a segfault with fault address `00000008`) matches this doc's own
already-root-caused `sendSoundData()` uninitialized-stack-local bug
(`0x3506c` in `libSetting.so`) byte-for-byte — but `ICType: 0` shouldn't
have been reachable at all, since `SoundType=0 → SoundType=3` was
already fixed and documented earlier in this file ("`MsnProductInfo.ini`"
section above).

Checked `firmware_source/prado_reconstructed/mtd6_rootfs/rootfs/
msnprofile/MsnProductInfo.ini` directly: it had

```
#SoundType=3
SoundType=0
```

i.e. the fix had been silently reverted back to stock's original value.
Almost certainly a side effect of commit `00825d0` ("change MsnProduct
info in reconstructed firmware to match original Prado dump") —
resyncing this file against the stock dump for unrelated reasons
clobbered this specific deliberate change without anyone noticing,
since nothing about that commit's stated purpose mentioned audio.

**Fix applied (2026-07-16):** restored `SoundType=3` (commented out the
stock `0` instead, matching the file's existing convention). This is a
plain rootfs text file, not a kernel change — needs the rootfs image
repackaged (`build_bootable_sdcard.sh`) and reflashed, no kernel
rebuild required.

**Process note for future sessions:** any future "resync configs
against the stock dump" pass needs to explicitly check for and
re-apply this project's own deliberate config deviations from stock
(`SoundType`, and likely others found elsewhere in this investigation)
rather than blindly overwriting with stock's values — this is the kind
of silent regression that's easy to reintroduce exactly this way again.

**Important caveat, established the same session:** fixing `SoundType`
alone does not stop the crash. It only changes *which* call path
reaches the buggy line in `sendSoundData()` (the normal EQ-init path
instead of the abbreviated fallback path) — see the full disassembly
below for why the crash is unconditional once you reach that function
at all, regardless of `SoundType`.

## `sendSoundData()` fully traced: what it does, why the "second message" crashes, and why stock doesn't (2026-07-16)

`libSetting.so` is unstripped (full C++ symbols/mangled names intact),
so this was traced precisely via `objdump -C -d` rather than inferred.

**Signature:** `SettingWindow::sendSoundData(unsigned char cmd,
unsigned char subcmd, const char *data, unsigned char len)` at
`0x34fa8`.

**What it actually does (the real, working part):**
1. Constructs an `MsnEvent(8, 403, 0xc73d)` on the stack — event type
   `8`, target plugin ID `403` (matches `libMsnSound.so`'s own plugin
   ID from the boot log's `Load App Plugin 403 "/usr/lib/libMsnSound.so"`
   line), event-type enum `0xc73d`.
2. Calls `makeProtocolPackage(0, cmd, subcmd, data, len)` — builds a
   `QByteArray`: header byte `0`, then `cmd`/`subcmd`, then the
   `data`/`len` payload. This **is** the actual sound/EQ command being
   built.
3. `MsnEvent::setByteArrayParams()` attaches that packet to the event.
4. `MsnApplication::dispatchMsnEvent()` — the real "send": hands the
   event synchronously to whatever's registered for plugin 403. This
   call itself completes without fault (confirmed by the 2026-07-13
   live re-test, where this exact dispatch round-trips synchronously
   into `Sound_BD37033::onRecvSoundProtocol()` before crashing).

**What crashes, precisely:** immediately after that dispatch returns
(`0x35050` onward), the function runs what *looks* like cleanup code
for two more local objects — Qt's standard implicit-sharing
refcount-decrement pattern (`__kuser_cmpxchg` at the fixed ARM helper
address `0xffff0fc0`, then a conditional `qFree()`/`QString::free()`)
— reading their data pointers from stack slots `[sp+68]` (crash site,
`0x3506c`) and `[sp+64]` (`0x350a4`, same pattern, would have crashed
next if the first one hadn't). **Neither slot is written anywhere in
the function.** There is no second `makeProtocolPackage()` call at
all — despite it looking like "building a second message" from the
crash-site call stack alone, the actual disassembly shows only cleanup
code, for objects whose construction is simply missing.

**Evidence this is a genuine binary/toolchain defect, not just a
logic bug:** the function has a real ARM EHABI exception-unwinding
landing pad at its end (`bl __cxa_end_cleanup` + a loop back through
`QByteArray::~QByteArray()`/`MsnEvent::~MsnEvent()`, `0x350e8`-`0x350fc`)
— a second, parallel copy of the same destructor sequence used if a
C++ exception unwinds through this function. The compiler only emits
that when it believes real RAII-managed objects exist at those stack
slots on every exit path. So the compiler *believed* `[sp+64]`/`[sp+68]`
held constructed objects needing destruction — the constructor calls
for them are just absent from the compiled output. This is more
consistent with a codegen/optimizer defect (an old ARM GCC + Qt4 + C++
exception-handling interaction dropping a constructor while keeping
its paired destructor) than a hand-written source bug, though the
original source isn't available to fully confirm which.

For contrast: `[sp+16]` (a separate local, visibly initialized at
`0x35050`-`0x35060` by loading Qt's `shared_null` sentinel + 8 — the
standard inlined default-`QString`/`QByteArray` construction pattern)
and `[sp+48]` (destructed via `QVariant::~QVariant()` at the very end,
which is safe to call on a zero-initialized/Invalid `QVariant` even
without an explicit constructor call, since Qt's `QVariant` dtor type-
checks before freeing) are both fine — only `[sp+64]`/`[sp+68]`
specifically are read-without-being-written in a way that actually
crashes.

**Why the identical binary boots fine on stock:** this is plain
uninitialized-stack-memory undefined behavior, not a deterministic
fault. `sub sp, sp, #84` reserves the frame but doesn't zero it —
`[sp+68]`'s "value" is whatever bytes were left over from whatever
function last used that exact stack region before `sendSoundData()`
was called. That depends on the full call history leading up to this
point (which is different between stock's 3.4 kernel/toolchain-built
Qt/libc and this reconstruction's 4.19 kernel/build), plus stack
layout/ASLR differences between kernel versions. On stock, whatever
ends up there apparently doesn't crash on dereference (most likely
`0`/NULL, which Qt's free helpers can no-op on safely, or some other
already-mapped address). On this reconstruction, the live crash
register dump confirms the same slot reliably holds the literal
integer `8` (traceable to a stray `mov r1, #8` used earlier in the
same function for an unrelated purpose, apparently left on the stack
at that offset) — `0x00000008` is unmapped, so it segfaults every time.
Same code, genuinely different garbage, not a difference in logic.

## Binary patch applied (2026-07-16)

Patched `firmware_source/prado_reconstructed/mtd6_rootfs/rootfs/usr/lib/libSetting.so`
only (the deployed copy — `firmware_dumps/Prado firmware dump/...`
reference copy left untouched for byte-for-byte comparison purposes).

Single 4-byte instruction change at file offset `0x35064` (== vaddr,
first `LOAD` segment has file-offset-equals-vaddr):

```
0x35064: e59d5044  ldr r5, [sp, #68]      -- was: start of the crash block
      -> ea000019  b   0x350d0            -- now: skip straight past both
                                              broken cleanup blocks
```

`0x350d0` is `add r0, sp, #48` / `bl QVariant::~QVariant()` — the next
*legitimate* destructor call after both broken blocks (`[sp+68]`'s and
`[sp+64]`'s). Verified via `objdump -C -d` post-patch: the branch lands
exactly there, `QVariant`/`QEvent` destructors and the normal function
epilogue (`add sp, sp, #84; pop {r4,r5,r6,r7,r8,r9,pc}`) are unaffected
and still run. File size unchanged (in-place single-instruction swap,
no relocation/offset shifts elsewhere in the file).

This does not touch the *working* part of the function (the real
message: `MsnEvent` construction, `makeProtocolPackage`,
`setByteArrayParams`, `dispatchMsnEvent`) — only removes the
unconditional dereference of two local objects that were never
constructed in the first place, so there is nothing correct being
skipped; those two blocks had no valid work to do regardless of
`SoundType`/call path.

**Live-tested (2026-07-16): real progress, but a second instance of the
same bug found.** With `SoundType=3` restored and the `sendSoundData()`
patch applied, `MsnCoreApp` gets dramatically further before crashing —
`SoundAdapter Create Success, ICType: 3`, GPIO mute toggling, balance,
subwoofer setup all complete — before hitting a new crash, fault address
`00000008` again. This time a real kernel-level register dump was
available (`docs/logs/msn app err-4.log.txt`), not just a Qt minidump,
giving an exact call stack: `SettingWindow::initSoundParams()+0x6f8` ←
`onFirstInit()+0x6c4` ← `SettingWindowC1()+0x3d4`. Register state at
fault matches the same signature as the first bug exactly:
`r7=0xffff0fc0` (`__kuser_cmpxchg`, the same Qt atomic-refcount helper),
`r5=0x00000008` (the "pointer" actually dereferenced).

Disassembled `initSoundParams()` (base `0x39b74`, crash at
`+0x6f8` = `0x3a26c`): `ldr r5, [sp, #444]` two instructions earlier,
then `ldr r0, [r5]` — dereferencing a stack slot (`sp+444`/`0x1bc`)
that, confirmed by grepping the disassembly of the entire function, is
referenced in exactly two places: this crash site and the conditional
`qFree()` inside the same broken cleanup block — **never written by a
constructor anywhere in the function.** Identical defect pattern to
`sendSoundData()`: a local `QByteArray`/`QString`-like object whose
construction is missing from the compiled output, but whose
refcount-decrement-then-maybe-free cleanup still runs unconditionally.

**Fix applied (2026-07-16):** same technique as before — single 4-byte
instruction change at file offset `0x3a26c` (== vaddr):
`ldr r0, [r5]` → `b 0x3a298`, branching past the broken block straight
to the next legitimate cleanup (`add r0, sp, #440; bl
QString::~QString()`). Verified via `objdump` that the branch lands
correctly and the earlier `sendSoundData()` patch (`0x35064`) is still
intact in the same file.

**Live-tested (2026-07-16): landing point was itself broken — corrected.**
First attempt branched to `0x3a298` (`add r0, sp, #440; bl
QString::~QString()`), reasoning it was "the next legitimate destructor
call" purely because it was the next sequential instruction. Wrong: a
fresh crash capture (`docs/logs/msn app err-7.log`) showed the exact
same defect one instruction further in — `QString::~QString()` itself
crashing, PC inside `MsnCoreApp`'s own copy of the destructor
(`_ZN7QStringD1Ev+0x14`), fault value `1` this time, called from
`initSoundParams()+0x72c` (one call further than the first crash's
`+0x6f8`). Systematically grepped the full function disassembly for
every stack offset referenced in this trailing block:

| Offset | Written anywhere in the function? | Verdict |
|---|---|---|
| `sp+444` (`0x1bc`) | No — only its own broken cleanup | broken (already patched) |
| `sp+440` (`0x1b8`) | No — only its own destructor call | **broken** |
| `sp+424` (`0x1a8`) | No — only its own destructor call | **broken** |
| `sp+20` | Yes, `0x39e98` (real `getSettingValue`/`toStringList()` code) | safe |
| `sp+60` (`0x3c`) | Yes, `0x39e04` (real empty-QByteArray init) | safe |

Three broken locals in a row in the same trailing block, not one —
"next instruction after the crash" is not a valid heuristic for
"safe landing point" on its own; each candidate needs the same
grep-for-any-`str`-to-that-offset check before trusting it.

**Fix corrected:** the same `0x3a26c` patch now branches to `0x3a2a8`
(`mov r0, r6; bl QEvent::~QEvent()`) instead of `0x3a298` — skipping
past both `sp+440` and `sp+424`'s broken destructor calls in addition
to the original `sp+444` one, landing on the destructor for `r6` (the
genuinely-constructed `MsnEvent` from earlier in this same function),
confirmed untouched between construction and this point. Everything
downstream of that (the `QList<QString>` destructor on `sp+20`, the
`QByteArray` cleanup on `sp+60`, and the final `sendSoundData(17, 80,
...)` call + epilogue) all checked out as legitimately initialized.

**Not yet hardware-tested.** Needs the rootfs image repackaged
(`build_bootable_sdcard.sh`) and reflashed, then a live retest. Given
three broken locals turned up in one trailing block alone, treat this
as a strong prior that more exist elsewhere in this binary (or others)
— if `MsnCoreApp` crashes again, get a fresh `err-N.log`/register dump
and, critically, **verify the chosen landing point itself** by grepping
for writes to every stack offset the trailing code touches, not just
the one at the immediate crash site.

## Superseded by CSTech-202511-IP17 rootfs (2026-07-16)

The binary-patching approach above chased individual uninitialized-stack
crashes one at a time in the Prado dump's `libSetting.so`. A newer
MsnCoreApp build extracted from a CarSyncTech Toyota unit
(`firmware_dumps/CarSyncTech Toyota/CSTech-202511-IP17/`, Nov 2025) was
found to boot the UI cleanly on real hardware with none of the
`sendSoundData()`/`initSoundParams()` crashes seen above — it appears to
be a later vendor build where these defects were already fixed upstream.
`build_bootable_sdcard.sh` now supports this rootfs (and its matching
userdata) as an alternative to the Prado reconstructed one, via the
`[p2] Use CarSyncTech CSTech-202511-IP17 rootfs` / `[p3] Use matching
CarSyncTech CSTech-202511-IP17 userdata` toggles (`--cstech-rootfs`,
`--cstech-userdata`). Both auto-extract from the checked-in
`rootfs.tar.gz`/`userdata.tar.gz` into a local non-vboxsf directory under
`~/Downloads/cstech-ip17-rootfs/` on first use, since the rootfs contains
1133 symlinks that a VirtualBox shared folder silently drops.

**libGAL.so fix is still required with this rootfs.** Initial `readelf
-d` inspection of the locally-extracted `libGAL.so` showed a complete,
structurally valid 29-entry `.dynamic` section, leading to an incorrect
assumption that `fix_libgal_dynamic_section` could be auto-disabled for
this rootfs. Live hardware testing contradicted this — MsnCoreApp still
segfaults on this rootfs without the fix applied. The auto-disable logic
was removed; the toggle now stays independently available (default ON)
for both rootfs sources. Lesson: static analysis of a library file in
isolation (readelf, objdump) doesn't rule out a runtime defect that only
manifests through the actual dynamic linker/loader path — same category
of mistake as the "next instruction after crash" heuristic above, just
one level up (verifying a *fix's applicability* by inspection instead of
verifying a *patch target* by inspection).

## Android Auto media audio stutters (video/touch confirmed good) — mechanism traced, root cause still OPEN (2026-07-28)

With this session's video fixes deployed, AA finally renders perfectly
and touch is responsive -- but media audio (tested with a podcast)
stutters, each glitch under a second, no button presses involved, and
no mic input is ever actually received during it. **Confirmed
identical on stock**: the real hardware and BT/WiFi interface are the
same, and stock's original firmware does not exhibit this. This
section documents everything traced so far -- a real, well-evidenced
mechanism, but not yet a proven explanation for the stock-vs-ours
difference. Don't cite this section as "solved."

### Dead ends, ruled out with real evidence (don't re-chase these)

- **BT UART DMA.** Our reconstructed kernel's `ark-hsuart` driver
  (`uart4`/`uart5`, MCU/BT) runs without DMA
  (`no TX/RX DMA channel available (no platform data)`). Checked the
  *real* stock 3.4 kernel dmesg
  (`docs/logs/archived/dmesg live device kernel 3.4 dmeg_260715.txt`)
  before building anything -- stock prints the near-identical
  `ark1680-hsuart ark1680-hsuart: no TX DMA channel!`. Stock never had
  DMA on these UARTs either. Dead end.
- **Bluetooth transport.** AA's actual media/video/control data goes
  entirely over WiFi (`Transport type is wlan!`, TLS over TCP) -- BT
  is only used for the initial RFCOMM handshake exchanging WiFi
  credentials. This independently confirms the BT-UART angle was never
  relevant to begin with.
- **ALSA buffer/period sizes.** `period_size`/`buffer_size` on all
  three slave devices in `/etc/asound.conf` (`dmix`, `dmix2`, `dsnoop`)
  are byte-for-byte identical between `firmware_source/mtd6_rootfs`
  and the real, untouched `firmware_dumps/Prado firmware dump`. Not
  changed by this project.
- **Mixer/channel routing.** Same 5 `softvol0-5` controls, same
  `dmix`/`dmix2`/`dsnoop` topology in both copies of `asound.conf`.
  Only difference found: `softvol3`'s `max_dB` (5.0 vs 10.0, a volume
  *ceiling*, unrelated to timing/stutter).
- **Audio codec.** Not applicable -- AA sends linear PCM directly for
  all three channels (48kHz/stereo media, 16kHz/mono x2 speech+system),
  confirmed by the exact channel/rate/bit-depth match to AA's known
  protocol spec and the total absence of any audio codec library
  linked into `sink`/`libAndroidAuto.so`/`libAutoDongle.so`. There is
  no decode step to be slow.

### What's actually happening (confirmed via Ghidra decompile)

The stutter's timing matches a real, continuously-repeating cycle in
`sink`'s own logs, **not** a button press: `LinuxController::
voiceSessionNotificationCallback` (a genuine Google AA SDK symbol, not
ArkMicro code) fires status=1 ("Granted transient audio focus") then
status=2 back-to-back, no idle gap, every ~10-12 seconds for the
entire session, opening/closing a microphone *capture* stream
(`dsnoop`-based, separate from playback) each time with **no real mic
input ever received**. See `docs/MSN_APP_ARCHITECTURE.html` band 03
for where `sink` sits in the process architecture.

Each cycle, `MsnCoreApp::onLinkStatusChange` (status codes 33/34/35/36)
calls `switchToAppAudioChannel()`, which (traced via Ghidra decompile
of `MsnCoreApp` and `libMsnCommons.so`) ultimately calls
`SoftVolCtrl::setMute()`/`setVolume()` -> `SoftVolCtrl::amixer_cset()`.
That function does a **complete ALSA control-device cycle from
scratch on every single call**, not a lightweight cached update:

```
snd_ctl_open()              -- fresh open of /dev/snd/controlC0
snd_ctl_elem_info()          -- ioctl round-trip
snd_ctl_elem_read()           -- ioctl round-trip (read current value)
snd_ctl_ascii_value_parse()
snd_ctl_elem_write()          -- ioctl round-trip (write new value)
snd_ctl_close()
```

This is literally `amixer cset`'s own logic, reimplemented inline (the
function is even named after it). `softvol` is a pure *software*
mixing control (`type softvol` in `asound.conf`) -- it does not touch
the slow bit-banged I2C bus to the BD37033 hardware codec, ruling that
out as an added-latency source too.

`AudioApp: "MsnCarPlay"` in the logs is a red herring/mislabeling, not
an actual CarPlay/AndroidAuto conflict -- it's just the internal label
for whichever app currently holds the transient-focus slot, left over
from when CarPlay support was written before AA was retrofitted in.

### Why this isn't actually solved yet

All of the above is real and confirmed, but **`MsnCoreApp` and
`libMsnCommons.so` are both unmodified vendor binaries, and the
hardware is confirmed identical to stock** -- so this exact
`amixer_cset()` call sequence also runs on stock every ~10-12 seconds,
and stock does not stutter. Attributing the difference to "our kernel
has more concurrent load now (video decode, SIGIO delivery, per-frame
LCDC updates)" was the working theory but is not proven, and the user
directly and correctly pushed back on it as too hand-wavy given
identical hardware/interface and identical vendor code. Don't present
"CPU contention" as the answer without harder evidence.

**Real next steps, not yet done:**
- Live-profile an actual stutter (`htop` or better, a scheduling
  trace) at the exact moment `voiceSessionNotificationCallback`
  fires, to see whether there's measurable CPU/scheduling pressure at
  that instant or not -- this is the one test that would actually
  confirm or kill the contention theory rather than just asserting it.
- Compare our ALSA/sound kernel driver stack against stock's directly
  for anything that could make `snd_ctl_open`/`snd_ctl_elem_write`
  itself slower on identical hardware -- e.g. a different lock
  granularity, a probe-time difference, or something in the
  `ark1668-i2s`/machine-driver glue that isn't part of the userspace
  code traced above at all.
- Confirm whether `voiceSessionNotificationCallback`'s ~10-12s cycling
  itself happens on stock too (with no audible effect), or whether
  stock's `sink`/AA library genuinely doesn't cycle this way at all --
  this hasn't been checked, and would materially change where to look
  next.

### Correction (2026-07-28, same day): the real symptom doesn't match the theory above -- redirected to a much better-fitting cause, fixed

User clarified the actual symptom: **a continuous, regular, split-second
break in audio, happening no matter how long playback runs** -- not
something that lines up with a once-per-10-12-seconds event. That
timing rules out `voiceSessionNotificationCallback`/`amixer_cset` as
the primary explanation (that section above is kept for the record --
the ALSA `user_ctl_lock` finding is real, upstream-verified, and worth
keeping in mind for something *else* eventually, but it doesn't fire
often enough to be *this*).

A "continuous, regular, high-frequency" stutter matches something else
already found earlier this same session: `sink`'s video layer calls
`ARKFB_SET_FB_ADDR`/`ARKFB_GET_FB_ADDR` roughly **30 times a second**
(once per AA video frame, confirmed via strace when those ioctls were
first implemented). The kernel handlers for those ioctls (and the
earlier `ARKFB_SET_VIDEO_ADDR_RAW`) used
`spin_lock_irqsave(&sinfo->lock, ...)` around a couple of `writel`/
`readl` calls -- disabling **all interrupts on the CPU**, including
whatever services the audio DMA/period-elapsed IRQ, for the critical
section, ~30-90 times a second (up to 3 ioctls per frame).

**Checked whether that lock was even doing anything real**: it wasn't.
`ark1668_lcdfb_interrupt()` (the LCDC vsync IRQ handler) calls the
exact same `ark1668_lcdc_set_video_addr()` function from IRQ context
and does **not** take `sinfo->lock` at all -- and grep across the whole
driver (`ark1668_lcdfb.c` + `ark1668_lcdc_funcs.c`) confirms `sinfo->lock`
is used *nowhere* except these three ioctl handlers, never from IRQ
context. So the IRQ-disabling was providing zero protection against the
real concurrent writer (the vsync IRQ handler ignores this lock
entirely) -- it only ever serialized against another ioctl call in
process context, which never needed interrupts disabled to do safely.

**Fix**: downgraded all three sites from `spin_lock_irqsave`/
`spin_unlock_irqrestore` to a plain `spin_lock`/`spin_unlock` (no IRQ
disable), in `ark1668_lcdc_funcs.c`. Confirmed safe (no deadlock risk)
by exhaustively checking every use of `sinfo->lock` in the driver
before making the change, not just assuming it. Kernel compiles clean,
rebuilt, and `zImage.w_dtb` re-staged.

**Not yet hardware-tested.** This is a strong, well-reasoned fix for a
real, confirmed-pointless IRQ-disable window at exactly the frequency
that matches "continuous and regular" -- but it hasn't been verified on
the device yet. If the stutter persists after this, the ALSA
`user_ctl_lock` finding above is still worth keeping as a secondary
lead, and the "does stock even cycle voice-session status" question is
still open too.

### Update (2026-07-28, same day): hardware-tested, the IRQ-disable fix did NOT resolve it

User tested the `spin_lock_irqsave` -> `spin_lock` downgrade on the
device: **no change to audio stuttering**. The fix is kept -- it's a
real, verified-safe improvement (removes a genuinely pointless
interrupt-disable window) -- but it is not the root cause of this
symptom.

Ruled out two more candidates before escalating to live measurement:

- **WiFi power-save.** `rtl8811cu`'s own driver Makefile compiles
  `CONFIG_POWER_SAVING` out entirely, so the radio always runs in
  `PS_MODE_ACTIVE`. Not a source of periodic latency.
- **I2S/ALSA DMA engine driver.** Checked `ark1668`'s machine driver
  and DMA glue for anything custom; it uses the generic ALSA
  `dmaengine` framework with nothing unusual in the period/IRQ
  handling. No bug found here either.

At this point every static-analysis candidate examined so far has
either been ruled out or fixed-but-ineffective, so rather than keep
guessing new candidate mechanisms, the next step is to measure
interrupt/scheduling latency directly on the device with the kernel's
own `ftrace` infrastructure.

**Enabled `CONFIG_FTRACE` and the `irqsoff` tracer.** `CONFIG_FTRACE`
was previously disabled outright (`# CONFIG_FTRACE is not set`) despite
the hardware supporting it (`CONFIG_HAVE_DYNAMIC_FTRACE=y` was already
set). Added to `arch/arm/configs/ark1668_defconfig`:

```
CONFIG_FTRACE=y
CONFIG_FUNCTION_TRACER=y
CONFIG_FUNCTION_GRAPH_TRACER=y
CONFIG_IRQSOFF_TRACER=y
CONFIG_SCHED_TRACER=y
```

(`CONFIG_PREEMPT_TRACER` was attempted too but doesn't apply -- it
requires `CONFIG_PREEMPT`, and this kernel is built `PREEMPT_NONE`.)

Rebuilt via `./build_kernel.sh --defconfig` (the `--defconfig` flag is
required here specifically because the build script skips defconfig
application when a `.config` already exists, which it did). Build
succeeded; zImage grew from 3.9M to 4.5M as expected for the added
tracer instrumentation. Spot-checked the regenerated `.config` against
every setting this project has previously lost to defconfig
regeneration drift (see `docs/` kernel-defconfig-drift history) --
`CONFIG_INET`, `CONFIG_IPV6`, `CONFIG_WIRELESS`, `CONFIG_CFG80211`,
`CONFIG_MAC80211`, `CONFIG_WLAN`, `CONFIG_ARK_MEMALLOC`,
`CONFIG_ARK_HX170DEC`, and the RN6752-vs-ARK7116 choice were all
confirmed intact.

Committed (`38eece991`) and pushed to `linux-arkmicro`. `zImage.w_dtb`
rebuilt and staged. **Not yet hardware-tested.**

**How to capture a trace at the device** (once this kernel is
flashed/booted):

```sh
# if not already mounted:
mount -t debugfs none /sys/kernel/debug

echo irqsoff > /sys/kernel/debug/tracing/current_tracer
echo 1 > /sys/kernel/debug/tracing/tracing_on
# let Android Auto audio play and stutter for ~10-20 seconds
echo 0 > /sys/kernel/debug/tracing/tracing_on
cat /sys/kernel/debug/tracing/trace > /data/irqsoff_trace.txt
```

`irqsoff` records the single longest interrupts-disabled section seen
during the capture window (and resets each time `tracing_on` is
toggled back on), so if the stutter is periodic and short, prefer
several short capture windows over one long one. If `irqsoff` doesn't
turn up anything above its latency threshold, `current_tracer` can be
switched to `function_graph` for a broader look at what's running
around the stutter instead.

Once a trace is captured, pull `/data/irqsoff_trace.txt` off the
device and this investigation can move from static analysis to
measured evidence for the first time.

### Further static sweep while waiting on hardware (2026-07-28, same day)

While waiting for the ftrace-enabled kernel to be flashed and tested,
did a broader static pass across our custom kernel code for anything
else that could cause a periodic full-system stall.

**Ruled out (clean, no further action):**
- No `printk`/`dev_dbg` on the success path of the video ioctl
  handlers, the I2S trigger/DMA path, or the `hx170dec` IRQ handler --
  every print in those files is either error-path-only or already
  commented out.
- `CONFIG_DYNAMIC_DEBUG` is not set, so all `dev_dbg()` calls in the
  tree compile to no-ops regardless of console loglevel.
- No `msleep`/`udelay`/`mdelay`/`usleep_range` anywhere in the video
  ioctl, I2S, or hx170dec hot paths that could block under a lock.
- No `CONFIG_CPU_FREQ` at all -- the CPU runs at a fixed clock, so
  cpufreq governor transitions are not a possible cause.
- No `request_threaded_irq` use anywhere relevant to this SoC (the
  only hits in the tree are for unrelated, unused DMA platforms
  compiled in but not instantiated).
- No userspace code anywhere (`firmware_overlay`, `firmware_source`)
  calls `sched_setscheduler`/uses `SCHED_FIFO`/`SCHED_RR` -- nothing
  runs at realtime priority, so the kernel's default RT-bandwidth
  throttle (`sysctl_sched_rt_runtime`=950000/1000000, i.e. RT tasks
  capped at 95% per 1s period) should not be able to fire.

**One real, unresolved risk -- not confirmed as the cause, but not
ruled out either**: the DTS bootargs (`arch/arm/boot/dts/ark1668.dtsi`
and siblings) set `loglevel=8`, and
`CONFIG_MESSAGE_LOGLEVEL_DEFAULT=7`. Console loglevel 8 means *every*
plain `printk()` anywhere in the kernel -- not just code this project
wrote -- gets written out to the `ttyS0` serial console at 115200
baud. printk-to-slow-serial holding `console_sem` is a well-documented
cause of exactly this "continuous, regular, sub-second glitch"
pattern, if anything in the tree prints even occasionally during
playback. Nothing was found printing in the specific hot paths audited
above, but this sweep could not cover the WiFi driver (`rtl8821au`,
fetched from GitHub at build time via buildroot, not vendored in this
repo, so it couldn't be grepped directly) or the USB stack. Two ways
to make progress on this without more source access: (1) as a quick
test, drop `loglevel=8` to something like `loglevel=4` in the DTS and
see if the stutter changes at all; (2) check whether the ftrace
capture shows any `console_unlock`/printk-related activity
coinciding with a stutter.

Also noted but not chased further: `CONFIG_RT_GROUP_SCHED=y` and
`CONFIG_CGROUP_SCHED=y` are enabled, with the kernel's default RT
scheduling-bandwidth split active. Since nothing actually runs at RT
priority (confirmed above), this shouldn't be able to fire -- flagged
only in case the ftrace/sched trace turns up something scheduling
related that would make it relevant after all.

No new fix came out of this pass -- it's a narrowing/elimination step
to keep the live ftrace trace as the next real source of evidence,
rather than another untested static-analysis guess.

### ALSA XRUN debug logging enabled (2026-07-28, same day)

User checked at the device: **no console log output at all coincides
with a stutter**, which effectively rules out the `loglevel=8`/printk
theory above -- if any driver were printing during playback it would
have shown up. Asked for better audio-side logging instead of more
static guessing.

Rather than hand-roll new `printk`s into the I2S driver (which uses
the generic `dmaengine` PCM framework -- there's no custom
period-elapsed/DMA-callback code of ours to instrument; the real DMA
IRQ handling lives in `drivers/dma/ark-dma.c`, the Synopsys DesignWare
DMA controller driver, shared with every other DMA consumer on the
SoC), enabled ALSA core's own built-in ring-buffer overrun/underrun
detector logging instead. This is a standard, well-tested facility,
not new code:

```
CONFIG_SND_VERBOSE_PROCFS=y
CONFIG_SND_DEBUG=y
CONFIG_SND_PCM_XRUN_DEBUG=y
CONFIG_SND_VERBOSE_PRINTK=y
```

`SND_PCM_XRUN_DEBUG`'s own Kconfig help text: *"if you have trouble
with sound clicking when system is loaded, it may help to determine
the process or driver which causes the scheduling gaps."* -- a direct
match for our symptom. Normally, when ALSA's `snd_pcm_update_hw_ptr`
finds the ring buffer has run empty (or the DMA pointer has looped past
where it should be), it just silently transitions
`runtime->status->state` to `SNDRV_PCM_STATE_XRUN` -- no log line at
all unless this option is on. With it on, ALSA prints a timestamped
XRUN report to dmesg (via `SND_VERBOSE_PRINTK`, with source file/line)
the instant it detects one.

This directly tests two competing possibilities:
- **If XRUN messages appear in dmesg exactly when a stutter is heard**:
  proves it's a genuine buffer-underrun problem -- something is
  failing to keep the ALSA ring buffer fed in time (points back at
  scheduling latency / the DMA controller / CPU contention, and the
  ftrace `irqsoff` capture becomes the natural next step to find out
  *why* the refill is late).
- **If no XRUN messages appear despite audible stutters**: proves the
  ALSA buffer itself is *not* underrunning -- the glitch is being
  introduced somewhere else entirely (e.g. in the audio content
  actually arriving over the network from the phone, in `sink`'s own
  buffering/decoding, or downstream of ALSA in the analog output
  path) -- which would redirect the whole investigation away from the
  kernel DMA/scheduling angle it's been focused on.

Rebuilt (`./build_kernel.sh --defconfig`), verified all four configs
landed in `.config`, committed (`8125387c8`) and pushed to
`linux-arkmicro`. `zImage.w_dtb` re-staged. **Not yet hardware-tested.**
Once flashed, just play AA audio until a stutter is heard and check
`dmesg | grep -i xrun` (or the tail of `dmesg` generally, given
`CONFIG_PRINTK_TIME=y` is already on so every line is timestamped) --
this is now the second piece of evidence to gather at the device
alongside the `irqsoff` ftrace capture above.

### Result: no XRUN, ALSA's buffer is not underrunning (2026-07-28, same day)

User tested on the now-galcore-fixed kernel (see below): played AA
audio through multiple stutters, `dmesg | grep -i xrun` came back
empty. This is a real, clean negative result, not an inconclusive one
-- `SND_PCM_XRUN_DEBUG` logs unconditionally the moment ALSA's
`snd_pcm_update_hw_ptr` detects the ring buffer has run empty or the
DMA pointer has looped past where it should be. No log line means it
never happened.

**This rules out the entire kernel-side scheduling/DMA-refill theory
this investigation has been chasing since the IRQ-disable fix.** If
the DMA engine is being fed on schedule every period, the audible
click is being introduced either:

1. **Before ALSA** -- the PCM samples `sink` writes into the ring
   buffer already contain the gap/glitch by the time they're written
   (e.g. a network stall on the AA WiFi link causes `sink` to insert
   silence, drop, or duplicate a chunk of audio while still writing
   *something* continuously, so ALSA never sees an empty buffer even
   though the content itself has a defect), or
2. **After ALSA**, in the analog output path -- the digital I2S/DMA
   stream is perfect, but something intermittently disturbs the
   analog signal downstream (the external amp's I2C control channel,
   or a mute/volume-ramp pulse) independent of the PCM data itself.

**Real, concrete next steps, not yet done:**
- For (1): check `sink`'s own audio-write timing/content for gaps --
  either a targeted, short strace capture of its `snd_pcm_writei`/
  `writen` calls (not the wholesale trace reverted earlier this
  project for CPU-load reasons -- a brief, deliberate capture just
  around a stutter), or check WiFi link quality (`iw dev wlan0 station
  dump`, retransmission/signal stats) during a stutter for a timing
  correlation with the phone's AP.
- For (2): this investigation already found a real, frequently-firing
  mechanism earlier in this same document -- `switchToAppAudioChannel`
  -> `SoftVolCtrl::amixer_cset()`, previously ruled out as the
  *primary* explanation only because its known trigger
  (`voiceSessionNotificationCallback`'s ~10-12s cycle) was too
  infrequent to match "continuous, regular, sub-second". Worth
  re-checking whether `SoftVolCtrl`/`amixer_cset` (or the external amp
  it may also drive via I2C, not just the software `softvol` ALSA
  control) gets invoked from some OTHER, higher-frequency call site
  than the voice-session cycle -- this wasn't exhaustively ruled out,
  only the one specific trigger was.

### Added period-timing and trigger-event logging (2026-07-28, same day)

User asked for more logging across the audio pathways since `dmesg`
wasn't giving much signal (the XRUN check came back clean). Added two
kinds, deliberately placed to avoid flooding `dmesg` at audio-frame
rate:

**`sound/core/pcm_dmaengine.c`, `dmaengine_pcm_dma_complete()`** -- this
is the generic ALSA period-complete callback used by *any*
dmaengine-based ASoC driver, including ours (not something specific to
the I2S driver -- editing it here catches every period completion
regardless of which DMA channel/direction it's on). Now tracks the
actual wall-clock gap between successive period completions against
the period's expected duration (from `rate`/`period_size`):
- `trace_printk()` fires on **every** period -- goes to the ftrace ring
  buffer (`cat /sys/kernel/debug/tracing/trace`), not `dmesg`, so this
  is safe to leave running continuously with no console/printk
  overhead of its own.
- A rate-limited `printk()` (capped ~20/sec) fires only when a period
  arrives more than 25% off its expected duration -- this is the one
  line that reaches `dmesg`, and only for a genuine timing anomaly.

This directly closes the gap the XRUN check left open: XRUN only fires
once the buffer is completely empty, but a period that's late-yet-not-
buffer-starving would never trigger it and could still be audible.
Now that's directly observable.

**`sound/soc/arkmicro/ark1668_i2s.c`** -- added plain `printk`s (safe at
dmesg rate, these are rare events) at:
- `ark_i2s_trigger()` -- logs every START/STOP/PAUSE with a timestamp.
  This is also currently the *only* visibility into trigger activity
  at all, since `ark_i2s_txctrl`/`rxctrl` are no-op stubs (see the
  earlier "TODO comments" discussion in this document) -- if the
  stream is being stopped/restarted more often than expected during
  playback, this will now show it directly.
- `ark_i2s_hw_params()` -- logs stream open/format changes (rate,
  period size, periods, format).

Compiles clean, kernel rebuilt, `zImage.w_dtb` re-staged. Committed
(`d71ec3ef0`), pushed to `linux-arkmicro`. **Not yet hardware-tested.**

Once flashed: reproduce a few stutters, then check `dmesg` for any
`pcm_dmaengine: period jitter` lines (timing anomalies large enough to
matter) and `ark1668-i2s: trigger` lines (unexpected stream restarts).
For finer-grained detail than dmesg gives, `cat /sys/kernel/debug/
tracing/trace` has the full per-period timeline via the `trace_printk`
calls -- useful to correlate against the exact moment a stutter was
heard.

### Added mute/volume/mixer-control logging (2026-07-28, same day)

Follow-up request: cover mute, mixer, and channel-control activity too,
not just DMA/period timing. Two more additions, both to real, active
code paths (not per-audio-frame, so safe to log unconditionally):

**`sound/soc/arkmicro/ark1668-sddac-codec.c`, `ark_sddac_mute()`** --
this is a **real, hardware-active mute**: it writes the DAC's gain
register directly (`I2S_DACR0`), fixed 2026-07-18 after being a no-op
stub. ASoC's core calls `.digital_mute` automatically around stream
trigger/prepare transitions. If something is toggling this more often
than expected during otherwise-continuous playback, that alone would
produce an audible click/dropout **completely independent of ALSA's
digital buffer health** -- directly explaining why
`SND_PCM_XRUN_DEBUG` found nothing. This is arguably the single
strongest new lead added today. Also logged the L/R playback-volume
get/set kcontrols in the same file, even though their bodies are
currently no-op stubs -- worth knowing if they're ever actually
called.

**`sound/core/control.c`, `snd_ctl_elem_write()`** -- logs every
mixer/control write on the system, any card, any control, any name.
This is the generic ALSA core ioctl handler behind `amixer cset` and
`SoftVolCtrl::amixer_cset()` alike. Whether or not whatever
MsnCoreApp's channel-switching/volume code touches actually shows up
here settles a real open question: some ALSA-lib virtual/software
controls (e.g. an `asound.conf` `ctl { type softvol }` block) are
handled **entirely in userspace** and never reach the kernel control
API at all. If nothing logs here during a stutter despite the app
definitely adjusting *something*, that's a real, useful negative
result -- it would mean the investigation needs to move to userspace/
alsa-lib instrumentation instead of the kernel.

Compiles clean, kernel rebuilt, `zImage.w_dtb` re-staged, galcore.ko
(fixed earlier today) confirmed unaffected/still present. Committed
(`c6276c71a`), pushed to `linux-arkmicro`. **Not yet hardware-tested.**

Once flashed, alongside the earlier `pcm_dmaengine`/`ark1668-i2s` log
lines, watch `dmesg` for:
- `ark1668-sddac: digital_mute` -- any unexpected mute toggling during
  continuous playback is the strongest candidate to chase next.
- `snd_ctl: elem_write` -- confirms or rules out whether userspace
  volume/channel control calls reach the kernel at all.

### L/R playback volume was ALSO a no-op stub -- fixed (2026-07-28, same day)

User asked whether the volume stub found while adding logging actually
needs implementing, and whether stock has it. Checked stock's real
`vmlinux` (`firmware_dumps/Prado firmware dump/mtd5_kernel/extracted/
vmlinux.elf`) via `strings` and `objdump -d`: stock has real, active
`sddac_get_l_playback_volume`/`sddac_set_l_playback_volume`/`_r_`
functions and `"Left Playback Volume"`/`"Right Playback Volume"`
kcontrol name strings.

Disassembly confirmed **this is the exact same bug class as
`ark_sddac_mute`** before its 2026-07-18 fix: the correct
reconstruction already existed as commented-out code in this file, it
was just never enabled.
- `sddac_get_l_playback_volume` (stock `0x802f63c4`): reads back a
  cached software value (`dac->vol_l`), not a live register read --
  matches the commented-out body exactly.
- `sddac_set_l_playback_volume` (stock `0x802f63ec`) /
  `sddac_set_r_playback_volume` (stock `0x802f645c`): read-modify-write
  `I2S_DACR0` with the L field (bits `[6:0]`) or R field (bits
  `[14:8]`) replaced while preserving the other channel -- byte-exact
  match for the `DACR0_LVOL_MASK`/`DACR0_LVOL()`/`DACR0_RVOL_MASK`/
  `DACR0_RVOL()` macros already defined in `ark_i2s.h` and already used
  by `ark_sddac_mute`'s real implementation. **Same register** --
  meaning a volume-set landing here concurrently with a mute/unmute
  (both hitting `I2S_DACR0`) is now directly correlatable via the mute
  logging added earlier today.

Also found and fixed a **control-name mismatch**: our kcontrols were
named `"Left/Right Playback Volume 2"` (note the "2" suffix) while
stock's real name (confirmed via the vmlinux strings, and independently
via a commented reference line in `firmware_source/mtd6_rootfs/etc/
all.sh`: `amixer cset ... name='Left Playback Volume' 118`) has no "2"
suffix at all. If anything -- `SoftVolCtrl`, or any other amixer-name
lookup -- was ever trying to reach these controls by stock's real
name, the mismatch would have made that lookup silently fail.

Implemented both get/set functions for real (uncommented + fixed the
existing logic), renamed the controls to match stock exactly. Compiles
clean, kernel rebuilt, `zImage.w_dtb` re-staged. Committed
(`f419add39`), pushed to `linux-arkmicro`. **Not yet hardware-tested.**

### Systematic stub/gap sweep against stock's real vmlinux (2026-07-28, same day)

User asked whether there are other stubs/gaps like the mute and volume
ones. Compared every function in the currently-active audio path
against stock's real `vmlinux` (`firmware_dumps/Prado firmware dump/
mtd5_kernel/extracted/vmlinux.elf`) via `nm`/`objdump -d`.

**Confirmed genuinely inert in stock too -- our matching stubs are
correct, not bugs, leave alone:**
- `ark_i2s_trigger`/`txctrl`/`rxctrl` -- stock's `ark_i2s_trigger`
  (`0x802f5f54`) jumps to the same `mov r0,#0; pop {r4,pc}` epilogue
  for all six `SNDRV_PCM_TRIGGER_*` commands. Confirms the earlier
  "TODO comments" finding (this document, above) was right: real
  DMA start/stop is handled entirely by the generic ASoC/dmaengine
  PCM framework, independent of this DAI-level callback, on stock too.
- `ark_i2s_hw_params`, `ark_i2s_set_fmt`, `ark_i2s_probe`,
  `sddac_startup`, `sddac_hw_params`, `sddac_set_bias_level` -- every
  one of these is `mov r0,#0; bx lr` in stock, i.e. a real, intentional
  no-op. (We don't even define a `set_bias_level` at all -- ASoC
  treats that identically to stock's own no-op version, so the
  omission is fine.)

**Already-fixed real gaps this session**: `ark_sddac_mute` (fixed
2026-07-18) and L/R playback volume get/set + control-name mismatch
(fixed earlier today, see above).

**New finding, NOT a stub -- an architecture difference, not yet
acted on**: stock has a real, separate `ark_i2s_mclk_set_rate`/
`ark_i2s_mclk_get_rate` pair (`0x80019278`/`0x80019da0`) -- these are a
proper Linux CLK-framework provider (`struct clk_ops`), not part of
`ark_i2s_hw_params` at all (consistent with `ark_i2s_hw_params` itself
being a no-op in stock, confirmed above). Disassembling
`ark_i2s_mclk_set_rate` shows a **hardcoded lookup table**: a chain of
rate comparisons (specific `mov`/`movt` immediate constants matching
exact sample rates like 22050/44100/8000/etc.) each selecting a
specific, apparently hand-tuned divider constant (`r8` values seen:
21, 25, 30, 33, 51, 85) -- not a simple linear formula.

Our `ark_i2s_hw_params` (`ark1668_i2s.c`) does something stock never
does in this function at all: it computes `modulo = freq / rate`
inline via plain integer division and writes the result straight to
the NCO register (guarded by `if (!i2s->nco_reg) return 0;`). This
isn't "does nothing when it should do something" like the other stubs
-- it's the reverse: we're doing real work here that stock does
elsewhere, via a different mechanism (a proper clk provider with
per-rate-tuned constants, not a formula).

**Not yet fixed, deliberately -- documented only per explicit
instruction.** This wouldn't explain click/stutter-type symptoms (it's
a clock-divider precision question, not a buffering one), so it's a
lower-priority thread than the mute/mixer logging above. If revisited:
would need `ark_i2s_mclk_set_rate`'s full comparison table
reconstructed (the snippet disassembled so far only covers a few of
the branches) to confirm whether plain division ever produces a
different divisor than stock's table for any of this project's actual
supported sample rates, before deciding whether it's worth changing.

**Not audited** (out of scope for this sweep, not part of the active
`ARK-SDDAC` card): `BD37033.c`, `ark_dac_codec.c`, `arkn141_audio.c`,
`ark1668e_audio_codec.c`, `cs4334.c`, `cs5343.c` -- confirmed via
`sound/soc/arkmicro/Makefile`'s `obj-$(CONFIG_...)` guards and the
current `.config` that **none of these are compiled in at all**
(`CONFIG_SND_SOC_BD37033`/`ARK_INTERNAL_DAC`/`ARK_ARKN141`/
`ARK1668E_INTERNAL_ADAC`/`CS4334`/`CS5343` are all unset). They're
Arkmicro's shared multi-board BSP source (ark1668/ark1668e/arkn141/
arkn141s all coexist in this tree) -- present in the filesystem, zero
runtime relevance. Only `ark1668_i2s.c` + `ark1668-sddac-codec.c` (+
`ark1668-sdadc-codec.c` for capture) are actually compiled
(`CONFIG_SND_SOC_ARK1668_I2S/ADC/DAC=y` confirmed in `.config`) --
already fully covered by this sweep.

### galcore GPU driver: unrelated crash found and fixed along the way

While testing the above, the first hardware boot after enabling ftrace
hit an unrelated kernel Oops in `galcore` (GPU driver) during
`gpu_probe` -- root-caused to Vivante's ARM TrustZone "Trusted
Application" layer being unconditionally compiled in and probed even
though ARK1668 has no TrustZone/TEE hardware. Fixed by skipping that
non-essential construction step and rebuilding `galcore.ko`.
**HW-CONFIRMED 2026-07-28: boots cleanly, galcore loads.** Full detail
in `linux-arkmicro`'s `gpu-known-good-pairing/README.md` and
[[project_galcore_missing_modparams]] (memory) -- kept here only as a
pointer since it's unrelated to audio.

### Regression: FUNCTION_TRACER caused a new, unrelated galcore crash (2026-07-28, same day)

User flashed and booted the ftrace-enabled kernel. Boot succeeded, but
`galcore` (the GPU driver) crashed during probe with a kernel Oops:
`Unable to handle kernel NULL pointer dereference at virtual address
00000000`, in `gckOS_WriteRegisterEx`, called from
`gctaHARDWARE_Construct` -> `gcTA_Construct` -> `gckGALDEVICE_Construct`
-> `drv_init` -> `gpu_probe`. This crash had never been seen before on
this same `galcore.ko`/`rcS` combination.

Checked first whether this was a regression of the 2026-07-20
`registerMemBase`/`irqLine` modprobe-params fix
([[project_galcore_missing_modparams]]) -- it wasn't. `firmware_overlay/
etc/rc.d/rcS` still correctly has:
`modprobe galcore registerMemBase=0xE0F00000 irqLine=32
contiguousSize=0x800000 physSize=0x80000000 powerManagement=0`.

None of this session's audio-focused changes (ALSA XRUN debug config,
the `Atmel LCDC` -> `ARK1668 LCDC` log-string fix) touch anything
GPU-adjacent. The one plausible candidate: `CONFIG_FUNCTION_TRACER`
(added earlier the same day for the ftrace/irqsoff diagnostic).
`FUNCTION_TRACER`'s own Kconfig help text: it works "by using a
compiler feature to insert a small, 5-byte No-Operation" at every
kernel function's entry point, patched live at boot via
`CONFIG_DYNAMIC_FTRACE` -- confirmed active from the boot log itself:
`ftrace: allocating 21831 entries in 65 pages`, i.e. roughly 21,831
call sites patched system-wide at boot. That's a far larger, riskier
change than anything else in today's diff, on an
ARM32/Linaro-gcc-7.3 toolchain combination that's nowhere near as
heavily exercised upstream as x86 -- plausible as a source of new
instability surfacing in an unrelated out-of-tree driver.

Checked `kernel/trace/Kconfig` directly: neither `IRQSOFF_TRACER` nor
`SCHED_TRACER` (the two tracers this investigation actually needs)
list `FUNCTION_TRACER` as a dependency. So `CONFIG_FUNCTION_TRACER`
and `CONFIG_FUNCTION_GRAPH_TRACER` were dropped from
`arch/arm/configs/ark1668_defconfig`, keeping `CONFIG_FTRACE=y`,
`CONFIG_IRQSOFF_TRACER=y`, `CONFIG_SCHED_TRACER=y`. Rebuilt (zImage
shrank back from 4.5M to 4.2M, consistent with removing the
instrumentation), committed (`3ce38eaee`), pushed to `linux-arkmicro`,
`zImage.w_dtb` re-staged.

**Not yet re-tested on hardware.** If the galcore crash persists after
this change, `FUNCTION_TRACER` was not the actual cause and the real
trigger is still unidentified among today's other kernel changes
(least likely candidates, in order: the ALSA `SND_DEBUG`/`SND_PCM_XRUN_DEBUG`
config, the LCD driver log-string rename, or something not yet
considered).

### Update: FUNCTION_TRACER theory disproven, real cause found and fixed (2026-07-28, same day)

User flashed the `FUNCTION_TRACER`-free kernel. **Identical crash** --
byte-for-byte the same PC, LR, and register dump as before. This
conclusively rules out `FUNCTION_TRACER` (or anything else in the
day's kernel-config changes) as the cause: a config-sensitive bug
would not reproduce with an identical register dump across two
different kernel builds. This is a fully deterministic bug, unrelated
to the audio investigation entirely.

The second boot log also showed a new line just before the Oops:
`modprobe: vmalloc: allocation failure: 351358976 bytes`. Traced the
actual crash site properly this time using the driver's own upstream
source (`gpu-known-good-pairing/nxp-source-6.2.4.p1.8/`, the pristine
tag this project's deployed `galcore.ko` was built from): the fault is
in `hal/security_v1/gc_hal_ta_hardware.c`'s `gctaHARDWARE_Construct()`,
specifically its very first hardware-probe write --
`gctaOS_WriteRegister(hardware->ta->os, hardware->ta->core, 0x00000,
0x00000900)` -- which matches the crash's register dump exactly (`r6 :
00000900`, offset 0). `gc_hal_ta_hardware.c` is Vivante's **ARM
TrustZone "Trusted Application" layer** -- built for boards with real
TrustZone/TEE silicon (its own `os/emulator/` subdirectory is a
software stand-in for boards without it, meant to make this codepath
harmless everywhere -- but it isn't, here).

Checked why this path is even being exercised: `Kbuild` sets
`-DgcdENABLE_TRUST_APPLICATION=1` **unconditionally** -- unlike the
neighboring `gcdSECURITY` flag, it isn't gated behind the `SECURITY=1`
build variable. So `gckGALDEVICE_Construct()` always calls
`gcTA_Construct()` (confirmed via `gc_hal_kernel_device.c` --
unconditional inside the `if (device->irqLines[gcvCORE_MAJOR] != -1)`
block, i.e. it always runs once `registerMemBase`/`irqLine` are
correctly configured -- which explains why this exact crash was never
seen before the 2026-07-20 modparam fix: with the wrong, previously
undetected `registerMemBase` default, this codepath likely wrote
through a wrong-but-still-mapped address rather than a genuinely NULL
one). `gctaOS_WriteRegister()` (in the emulator glue) forwards to
`gckOS_WriteRegisterEx(Os->os, Core, ...)`, and whatever the TA-layer's
own `os` wrapper construction does here ends up with a NULL register
base for this SoC -- root cause not traced further into the emulator's
internals, but the outcome (and fix) don't require it.

**Fix**: this TA construction is provably non-essential -- it exists
purely to power on the GPU and read a chip ID for the TrustZone/
security codepath (irrelevant on ARK1668, which has no TrustZone/TEE
at all), and `globalTA[]` (the pointer it would populate) is
NULL-checked everywhere else it's read in the driver, including its
own teardown path. The real, load-bearing GPU/MMU setup
(`gckDEVICE_AddCore()`) is a separate, unaffected call a few lines
below it. Patched `gc_hal_kernel_device.c` to skip the
`gcTA_Construct()` call for `gcvCORE_MAJOR`, rebuilt `galcore.ko`
out-of-tree against this exact kernel (`vermagic=4.19.192` confirmed
matching, no SMP/PREEMPT flags, consistent with this build), and
deployed the new binary over both the `gpu-known-good-pairing/`
checkpoint and the actual staged `compiled_modules/lib/modules/
4.19.192/galcore.ko`. Full detail and exact rebuild commands in
`linux-arkmicro`'s `gpu-known-good-pairing/README.md`. Committed
(`f6ae2660b`), pushed. **HW-CONFIRMED 2026-07-28: boots cleanly, galcore
loads without the Oops.** Root cause and fix both verified correct.

Also restored `FUNCTION_TRACER`/`FUNCTION_GRAPH_TRACER` is **not**
being re-added -- keeping them off since they were never actually
needed for the audio investigation (only `IRQSOFF_TRACER`/
`SCHED_TRACER` are), and there's no reason to reintroduce the more
invasive whole-kernel instrumentation now that it's confirmed
unrelated to this crash.

### Deep DMA/PCM-architecture comparison against stock (2026-07-28, same day)

User asked for a deep comparison of stock's audio pathway against
ours, to help localize the stutter. Compared the actual DMA-level
mechanics via disassembly of stock's real `vmlinux`.

**Confirmed real architectural difference**: stock has its own
hand-written ASoC platform/PCM driver -- `ark_pcm_open`/`_hw_params`/
`_prepare_dma`/`_trigger`/`_dma_period_done`/`_pointer`/`_new`,
registered via its own `platform_driver_register()` call (confirmed
genuinely active: registered alongside `ark_i2s_driver_init`/
`sddac_codec_driver_init` at the same module-init point, not dead
code). Our reconstruction has none of this source anywhere --
`ark1668_i2s.c` even still has a dead, commented-out
`//#include "ark1668_pcm.h"` -- it never made it into whatever source
this project's kernel was rebuilt from. We use the generic ASoC
`dmaengine_pcm` framework instead, bridged onto our driver's legacy
cyclic-DMA API via `dwc_prep_dma_cyclic()` (`ark-dma.c`, added
2026-07-13 specifically to make that bridging work at all).

**But tracing the actual DMA mechanics, this turns out functionally
equivalent -- not the smoking gun it first looked like:**
- `ark_pcm_prepare_dma` (stock) calls `dw_dma_cyclic_prep()` -- the
  exact same low-level DesignWare cyclic-DMA API our own bridge
  ultimately calls.
- It sets the period-callback function pointer into the returned
  descriptor the same way our bridge code does
  (`cdesc->period_callback = ...`).
- `ark_pcm_dma_period_done` (stock's period-complete handler,
  `0x802f5628`) disassembles to a trivial 4-instruction wrapper: null
  check, then call `snd_pcm_period_elapsed()`. Our
  `dmaengine_pcm_dma_complete()` does the same thing (update position,
  call `snd_pcm_period_elapsed()`).

So both paths ultimately drive the same DesignWare DMA engine the same
way, with only a thin extra layer of generic-framework indirection on
our side -- a handful of extra instructions per period, not something
that would produce an audible periodic artifact on its own. Real
difference, but doesn't look like the cause.

**Not yet checked, still open**: the actual DMA burst size /
`dma_slave_config` values (our `maxburst=16` in
`ark1668_i2s_drv_probe()` -- couldn't trace where/how stock sets the
equivalent without deeper decompilation of `ark_pcm_open`/whatever
configures the DMA channel's slave config), and GIC interrupt-priority
configuration for the DMA/audio IRQ relative to WiFi/USB/video IRQs
(a classic source of this symptom class, not compared against stock
at all yet).

### Symptom re-characterized: "choppy, segmented" audio, not clicks/pops (2026-07-28, same day)

**Important correction from the user**: the actual audio output isn't
a clicking/popping noise (brief interruptions in an otherwise-normal
stream) -- it's **a choppy, segmented version of the audio stream
itself**. This is a materially different symptom class from what most
of this investigation (the IRQ-disable-window fix, the DMA/PCM
architecture comparison just above, XRUN debug logging) has been
built around, and changes where to look.

**Why this redirects the investigation**: a DMA/IRQ-timing glitch or a
late/dropped period would typically produce a brief click or pop at
the moment of disruption -- a healthy stream with an occasional
defect. "Choppy, segmented" describes something different: audio that
sounds broken into pieces throughout, much more consistent with the
underlying **audio content itself** having gaps, being duplicated, or
arriving out of order -- before it ever reaches ALSA -- than with a
kernel-side scheduling/DMA problem downstream of a healthy PCM stream.
This matches the "before ALSA" branch of the two-way fork already
identified after the clean `SND_PCM_XRUN_DEBUG` result (see above):
`sink` could be receiving genuinely incomplete/gapped audio over the
Android Auto WiFi link (packet loss/jitter/retransmission) and still
writing *something* continuously to the ALSA buffer, which is exactly
why no XRUN was ever seen despite the audible defect.

**Real next steps, not yet done**: check WiFi link quality during a
reproduced stutter (`iw dev wlan0 station dump` -- retransmission
count, signal/noise, any deauth/reassoc events -- for a timing
correlation with when segments sound choppy); if feasible, a short,
deliberate `strace` capture of `sink`'s own `snd_pcm_writei`/`writen`
call timing and buffer sizes during a stutter (not the wholesale trace
reverted earlier this project for CPU-load reasons -- a brief, targeted
one) to see whether the data *arriving* at ALSA is already
discontinuous. The kernel-side DMA/PCM architecture comparison above
remains recorded for completeness but is now a lower-priority thread
given this symptom clarification -- it was aimed at explaining clicks
from a healthy stream, not gaps in the stream's actual content.

### New leading theory: simultaneous Bluetooth A2DP + Android Auto audio mixing (2026-07-28, same day)

User asked directly whether audio could be playing back simultaneously
through both MsnCoreApp's Bluetooth path and the Android Auto path.
Checked the actual ALSA routing and code, and this is a genuinely
strong, previously-unexplored lead.

**Confirmed: the default ALSA output path is `dmix`-based, which
allows multiple simultaneous PCM clients with zero exclusivity.**
`firmware_source/mtd6_rootfs/etc/asound.conf`: `pcm.!default` ->
`asymed` -> `softvol` -> `dmix` -> `hw:0,0`. `dmix` is ALSA's software
mixing plugin, explicitly designed so multiple independent processes
can each open a playback stream and have ALSA mix them together in
real time -- there is no exclusive-open enforcement anywhere in this
chain. If two processes both hold an open `softvol`/`dmix` stream at
once, both play, mixed, simultaneously, with no error from ALSA at
all.

**Confirmed: Bluetooth A2DP is genuinely enabled and capable of
streaming audio.** `blueware.properties`: `A2DP_ENABLE=1`. `strings`
on the `blueware` binary shows a real, active A2DP audio-playback
path: `audio_control_play a2dp:[%d]`, plus AT-command-style controls
`+A2DPSTAT`/`+A2DPCONN`/`+A2DPDISC`/`+A2DPMUTE`/`+A2DPSR`/`+A2DPDEV`/
`+A2DPMUTED` -- a full, real A2DP sink implementation, not a stub.

**Confirmed gap: nothing in the Android Auto code path has ANY A2DP
awareness at all.** `strings`-swept `sink` (the AA daemon), `carplay`,
`libAndroidAuto.so`, and `libAutoDongle.so` for any A2DP-related
string -- zero matches in all four. None of them call `+A2DPMUTE`,
check A2DP connection state, or coordinate with `blueware` in any way.
If a phone maintains both its classic Bluetooth A2DP profile AND an
Android Auto WiFi session concurrently (a real, common phone-side
possibility -- AA-over-WiFi typically keeps the underlying BT
connection alive for pairing/control purposes even after the WiFi
link is up) and independently decides to stream audio over A2DP at
the same time `sink` is streaming AA media audio, **this system has
no mechanism anywhere to prevent both streams reaching `dmix`
simultaneously.**

**Why this fits "choppy, segmented" specifically, better than the
kernel-side theories chased so far**: two independently-clocked PCM
streams being software-mixed by `dmix`, especially if one of them
(A2DP, over a lossier/lower-bandwidth BT link, or intermittently
receiving silence/comfort-noise/control-tone bursts from the phone)
isn't feeding data as smoothly as the other, would produce exactly a
broken-up, segmented-sounding hybrid of the intended audio -- not a
clean stream with occasional clicks. This also cleanly explains the
clean `SND_PCM_XRUN_DEBUG` result from earlier: `dmix`'s slave device
never starves, because *something* (possibly two somethings) keeps
feeding it continuously.

**Not yet confirmed -- needs a live test, two easy options:**
1. During a reproduced stutter, check whether more than one process
   holds an open PCM handle to `dmix`/`hw:0,0` simultaneously (e.g.
   `fuser`/`lsof` on `/dev/snd/pcmC0D0p`, or checking for both `sink`
   and `blueware` appearing as active clients).
2. Simpler, more direct test: **disconnect/disable Bluetooth entirely
   on the phone (or turn off BT on the head unit) while keeping the
   Android Auto WiFi session active, and see if the stutter changes.**
   If it goes away or changes character, this theory is confirmed and
   the fix is straightforward (have `sink`/`MsnCoreApp` proactively
   call `blueware`'s own `+A2DPMUTE`/disconnect A2DP when an AA
   session becomes the active media source, mirroring what a
   well-behaved implementation should already be doing here).

This is now the leading theory for the "choppy, segmented" symptom --
more directly testable than the WiFi-packet-loss theory above, and
doesn't require any log capture, just a quick behavioral test (BT
on vs. off during an AA session).

### CORRECTION: proposed BT-disable test walked back; existing logs already show a much stronger, direct lead -- repeated real WiFi disassociation (2026-07-28, same day)

User asked whether Bluetooth needs to stay connected for Android Auto
to function -- checked the already-captured connection logs
(`docs/logs/android auto log v1/v2/v3.txt`) rather than guess, and
found two things that change the picture.

**Answer to the question, from real log evidence**: in this
implementation, yes -- BT appears to be part of the WiFi-session
recovery chain. Tracing the sequence around a reconnect in `v1.txt`:
`130.496s` `OnDisassoc(wlan0)` (WiFi drops) -> `133.7s` WiFi
reassociates -> `135.541s` `Write At atCommand: "AT+HFPCONN=..."` ->
`136.416s` `AT+A2DPMUTE=1` + `Bluetooth connected` ->
`138.915s` `enableHostApd start` (the WiFi AP AA runs over gets
re-enabled). The Bluetooth reconnect is what triggers re-enabling the
hotspt in our own code (`msncoreapp.cpp`'s `enableHostApd`/
`APP_NOTIFY_ENABLE_HOSTAPD` path) -- **deliberately disconnecting BT
mid-session would very likely disrupt this recovery mechanism and
break the AA session outright, rather than cleanly isolating whether
A2DP is mixing in.** The BT-disable test proposed above is walked
back -- do not run it as originally suggested.

**Much stronger, already-evidenced lead found in the same log**: the
WiFi link itself is disassociating and reconnecting repeatedly
throughout the session -- **9 separate `OnDisassoc(wlan0)` events in
`v1.txt` inside a ~3-minute session** (`42.4s`, `54.5s`, `71.6s`,
`130.5s`, `139.7s`, `146.7s`, `175.2s`, `184.7s`, `196.9s` -- roughly
one every 10-30 seconds), every one with `reason=8` (802.11
"disassociated because sending STA is leaving BSS" -- phone-side
initiated, not the AP rejecting/kicking the phone), cycling between
two different client MAC addresses (`f6:d6:cb:da:c0:aa` and
`7a:05:1c:c2:97:47`). `v2.txt`/`v3.txt` show none of this (worth
checking whether those were shorter sessions, different test
conditions, or genuinely cleaner runs -- not yet compared).

**CORRECTION, same day: checked, and this weakens the theory
significantly -- it's NOT a general/consistent problem.** Compared
session lengths, not just event counts, across every AA-session log
available: `v1.txt` spans kernel-time `20.6s`->`202.1s` (~180s active)
with 9 disassociations; `v2.txt` and `v3.txt` (~138s) both show
**zero**; a newly-captured comprehensive boot+session log,
`new uboot new kernel baseline v20_260728.txt` (~170s, includes a
CarPlay session), also shows **zero**. These are comparable-length
sessions, not "v1 just ran longer and had more chances to fail" --
**3 of 4 similar-length sessions had a completely clean WiFi
connection.** That means the repeated disassociation seen in `v1` was
most likely something specific to that one test run (interference,
phone-side behavior that session, etc.), not a standing bug in this
project's AP/driver setup that would explain a *general* stuttering
symptom. Kept on file as a real, confirmed event worth understanding
on its own terms -- but **downgraded from "the answer" to "one
possible contributing factor in some sessions, not the general
explanation."** Also checked the `HW EFUSE` calibration dump in the
v20 log for a blank/uncalibrated-TX-power theory -- the region that
looks like per-rate TX power table data (`0x020`-`0x048`) is
genuinely populated with plausible calibration-looking values, not
blank, weakening that theory too. Also checked whether either build
ships `regulatory.db` (a missing regulatory database could affect
channel/power selection) -- **neither stock nor our build ships it**,
so that's not a stock-vs-ours divergence either.

**Open question needed to make further progress on this thread**:
does the choppy/segmented audio happen in *every* AA session, or only
some of them? If it's present even in sessions with a clean WiFi
connection (like whatever session produced `v2`/`v3`/`v20`), WiFi
disassociation can be ruled out as the (or a) cause entirely for those
instances, and the investigation should look elsewhere. If it only
happens in sessions that also show disassociation events, that would
support it as a real, if intermittent, contributing factor. Not yet
answered.

**Why this fits "choppy, segmented" better than either prior theory,
without needing a new test at all -- the evidence already exists**: a
WiFi link dropping and reconstructing this often would put literal
gaps in the AA audio/video data stream every single time, independent
of any kernel-side DMA/IRQ timing or A2DP-mixing mechanism. This
doesn't rule out the A2DP-mixing theory (both could be contributing,
and the A2DP mute cycle is itself entangled with these same
reconnects per the trace above), but it's better-evidenced right now
since it's already visible in logs that exist, rather than requiring
a new capture.

**Real next steps, not yet done**: figure out WHY the WiFi link keeps
disassociating this often -- reason=8 being phone-initiated points at
either the phone's own WiFi power-management/Doze behavior, RF
interference, or possibly our AP's beacon/keep-alive timing tripping a
phone-side timeout. Two different cycling MAC addresses is also worth
understanding on its own (could be normal Android per-connection MAC
randomization, or could indicate the phone is maintaining two
separate WiFi links that are each independently unstable). Checking
`v2.txt`/`v3.txt` for whether they show the same pattern (or genuinely
don't) would help establish whether this is a consistent problem or
session-dependent.

### Detailed analysis of `new uboot new kernel baseline v20_260728.txt` -- the choppy window pinned down precisely (2026-07-28, same day)

User confirmed exactly where the reported choppy audio happened in
this log: the first ~10-second real media playback window, which was
then paused, followed by a separate microphone test. This lines up
precisely with a specific, identifiable stretch of the log.

**Session overview**: a full, clean AA wireless session -- BT pairing
-> RFCOMM handshake -> WiFi AP (`carplay_fc9f`) -> phone DHCP -> TLS
1.2 negotiation with a Pixel 9 Pro -> video sink opens at 800x480 ->
plays -> disconnects cleanly at the end (`reason=3`, a normal
disconnect, not the `reason=8` instability pattern seen in `v1.txt`).
No crashes, no kernel Oops, zero `OnDisassoc` events this session.

**The choppy window, pinned to exact lines**: `playbackStartCallback
status=39 streamtype=3` at `69.315s` -> 28x `play:225` (no per-line
timestamp, unlike the surrounding `[XX.XXX]`-prefixed MsnCoreApp
lines) -> `playbackStopCallback status=40 streamtype=3` at `79.855s`.
That's the entire real-media (podcast/music) playback for this
session -- user confirmed this is where they heard the choppiness,
and confirmed it was then paused (matches the stop callback exactly),
followed by a separate microphone test (matches the `streamtype=1`
voice-prompt blips and `MicrophoneReaderThread` activity later in the
log, `137s` onward, cycling via the already-documented
`voiceSessionNotificationCallback` mechanism).

**A real, concrete mismatch found in this exact window**: 28 `play:225`
calls span the full ~10.5-second window (`69.315s` -> `79.855s`).
`hw_params` earlier in the same log shows `period_size=1024` at
`rate=48000` -- one period is ~21ms of audio. If each `play:225`
corresponds to writing roughly one period, 28 calls is only ~600ms of
actual audio content stretched across 10,500ms of wall-clock time --
audio being written in bursts with real gaps between them, not
steadily, which is a good, direct match for "choppy, segmented"
during precisely the window it was heard. Averaged out, that's one
`play:225` call roughly every ~375ms -- far slower than the ~21ms
per-period cadence a healthy, continuously-fed stream would show. No
per-line timestamps exist on the `play:225` lines themselves, so the
exact gap pattern between individual calls (evenly spread vs. bursty
clusters) isn't visible from this log alone -- only the aggregate rate
across the window.

**Why this specific log can't go further**: it predates today's kernel
logging additions (`digital_mute`/`period jitter`/`elem_write`/
`ark1668-i2s: trigger` -- confirmed zero matches for any of those
strings in this file) and the `play:225` lines have no fine-grained
timestamps of their own. **This is exactly the window a fresh capture
on the current kernel should target** -- reproduce a short (~10-20s)
real media playback right after AA connects, on the currently-staged
kernel build, and check `dmesg` for `pcm_dmaengine: period jitter`,
`ark1668-sddac: digital_mute`, and `snd_ctl: elem_write` activity
during that exact window, plus whether `sink`'s own write cadence
(via a short, targeted strace if needed) matches the bursty pattern
suggested above.

### `play:225` identified precisely: it's the ALSA XRUN recovery path, not a routine write log (2026-07-28, same day)

User corrected the "bursty" read above -- the gaps between `play:225`
occurrences are even, not clustered. That correction led to actually
tracing what the log line represents, via disassembly, instead of
continuing to guess from its timing alone.

**Found and disassembled `AlsaHandle::play(unsigned char*, unsigned
int)` in `sink`** (`0x31a4c`, symbol intact, not stripped). It calls
`snd_pcm_writei()` in a loop and branches on the return value:
- `-11` (`EAGAIN`): print, `snd_pcm_wait(handle, 500)`, retry.
- `-32` (`EPIPE`): **this is the `play:225` line** -- prints, then
  calls `snd_pcm_prepare()` (the standard ALSA underrun/XRUN recovery
  sequence), then retries the write.
- Any other negative value: print the real error via
  `snd_strerror()`/`fprintf`, then abandon the write and return.

**`play:225` is not "wrote a chunk of audio" -- it's ALSA reporting a
genuine userspace-visible buffer underrun (XRUN) on every single
occurrence.** 28 occurrences across the ~10.5-second window pinned
down above means **28 real ALSA XRUNs happened during that exact
stretch of real media playback**, each one triggering an audible
underrun-recovery cycle -- a direct, mechanism-level explanation for
"choppy, segmented" audio, not a inference from indirect timing. User
confirmed the gaps between occurrences are even (~375ms apart, given
28 events across ~10.5s) -- not random bursts, something is starving
the ALSA write at a fairly regular interval specifically during this
window.

**This also reframes the "media never resumes" observation from the
same log**: playback being paused immediately after this window lines
up with 28 underruns in 10 seconds being a rough enough listening
experience to pause, rather than a separate audio-focus/resume bug as
first suspected.

**Reconciling with the earlier clean `SND_PCM_XRUN_DEBUG` result**:
that test predates this finding and may not have specifically
recaptured this exact post-connect window (the first ~10-20s of
playback right after an AA session establishes) -- the kernel-side
XRUN debug logging (already staged, confirmed working infrastructure)
should catch this directly if reproduced now. This is now the most
concrete, mechanism-confirmed lead in the whole investigation --
promoted above the WiFi-disassociation and A2DP-mixing theories, both
of which were built on indirect evidence.

**Real next step, well-defined**: reproduce media playback starting
immediately after an AA session connects (matching the `69.3s`-ish
timing relative to connection in this log), and check `dmesg` for
`XRUN` lines from `SND_PCM_XRUN_DEBUG` during that specific window --
if they appear with the same ~375ms-ish regular spacing, that
confirms the mechanism at the kernel level too and the investigation
can move to *why* the buffer underruns that regularly during exactly
this startup-adjacent window (candidates: concurrent video-decoder
initialization/first-frame work, DBus signaling load, or general CPU/
bus contention specific to the just-connected state, since this
window is right when the video sink/decoder is also standing up for
the first time in the session).

### `sink`'s own output redirected into dmesg (2026-07-28, same day)

To make the next test easier -- correlating `play:225`/XRUN events
against the kernel-side period-jitter/mute/mixer logging without
needing a separate terminal capture -- `firmware_overlay/usr/share/
dbus-1/services/com.arkmicro.auto.service`'s `Exec=` line now redirects
`sink`'s stdout/stderr straight into the kernel log:

```
Exec=/bin/sh -c "exec /usr/bin/sink > /dev/kmsg 2>&1"
```

**Known caveat, not yet verified on hardware**: glibc fully-buffers
`stdout` when it isn't attached to a TTY (true for both a plain file
redirect and `/dev/kmsg`), so `sink`'s `printf`-based lines (including
`play:225`) may arrive in bursts rather than immediately, only
flushing once the C library's internal buffer fills or the process
exits. This project's busybox build has no `stdbuf`/pty-allocating
`script` applet available to force line-buffering the usual way. Fine
for post-hoc correlation (comparing which lines appear before/after
which kernel events across a whole capture) even if buffered; not
reliable for real-time monitoring. If the buffering turns out to be
severe enough to matter (e.g. `play:225` lines arriving many seconds
late, out of useful order relative to kernel events), the next step
would be a small `LD_PRELOAD` shim forcing unbuffered stdio, rather
than assuming this simple redirect is sufficient.

Plain rootfs config change, no kernel rebuild needed. Committed
(`690454e`), pushed. **Not yet hardware-tested.**

### Investigating the XRUN root cause: single-core + unprioritized audio thread, but NOT confined to video-startup (2026-07-28, same day)

Asked to investigate what actually causes the XRUN underruns
identified above. Two structural facts confirmed via static analysis:

1. **This SoC is single-core.** `arch/arm/boot/dts/ark1668.dtsi` has
   exactly one `cpu@0` node, and `CONFIG_SMP` isn't set in the kernel
   config at all -- one CPU for the whole system.
2. **`sink` never elevates the audio-writing thread's priority
   anywhere.** Traced `LinuxAudioSink::init()` -- it registers with
   `GalReceiver` and starts a generic `WorkQueue`, standard
   normal-priority threading. Swept the whole binary for
   `pthread_setschedparam`/`sched_setscheduler`/`setpriority` -- zero
   matches. The thread that ends up calling `AlsaHandle::play()` has
   no protection against being starved by anything else that's
   demanding CPU at the same moment.

**Initial hypothesis, since corrected**: the first pass at this only
had one example to work from (the `v20` log's single ~10.5s playback
burst, right when the video pipeline was standing up for the first
time), so the working theory was "one-time video-init CPU burst
starves the unprioritized audio thread." **User has since confirmed
choppy audio also occurs at other points, well after video has been
running steady-state for a while** -- this rules out "only during
one-time video pipeline startup" as the full picture.

**What still holds**: the single-core + zero-priority-elevation facts
above are structural and confirmed, independent of when exactly the
starvation happens. They mean `sink`'s audio thread is *generally*
vulnerable to being starved by anything else CPU-heavy sharing the
one core, not just video-pipeline startup specifically. Ongoing video
decode itself remains a plausible recurring trigger (frame-to-frame
decode cost isn't constant -- keyframes/scene changes/larger encoded
frames could spike CPU periodically throughout a session, not just at
connect time), but nothing yet narrows the *recurring* trigger to one
specific source over any other candidate (WiFi servicing, DBus
traffic, etc.) -- this needs live evidence, not more static
inference.

**Real next step**: since this is now confirmed reproducible at
multiple points in a session (not a narrow one-time startup window),
it should be much more capturable with the `SCHED_TRACER` already
staged in the kernel (enabled earlier this session specifically for
this class of question, not yet actually used). A scheduling-latency
trace spanning a reproduced choppy moment -- at any point in a
session, not just right after connecting -- should show directly
what's running on the single core instead of the audio thread when it
needs to run.

**Available, no-rebuild experiment, not yet tried**: busybox has
`chrt` compiled in. Wrapping `sink`'s launch with `chrt -f <priority>`
to give it real-time scheduling priority is the standard fix for
exactly this class of problem -- lets the audio thread preempt
normal-priority work whenever it needs to run, without any source
changes or rebuild. Worth trying as a low-risk test independent of
getting the sched-trace confirmation first, since if it resolves the
choppiness that's itself strong evidence for the mechanism (and a
usable fix), and if it doesn't, that's a real negative result ruling
out "scheduling priority alone" as the complete explanation.

### Refined further: not video decode itself -- software AES decryption of the video stream (2026-07-28, same day)

User correctly pushed back on the "video decode competes for CPU"
framing: actual H.264 decode is hardware-offloaded to `hx170dec`
(confirmed extensively elsewhere in this project -- the fasync/SIGIO
fix, the direct decoder test tool), not something the CPU does math
for. Re-examined what's actually CPU-bound in the video pipeline
instead of assuming decode itself was the cost.

**Confirmed: this SoC has zero hardware crypto acceleration.**
`arch/arm/boot/dts/ark1668.dtsi` has no crypto/AES node; the kernel
config has no `CONFIG_CRYPTO_DEV_*` driver at all.

**Confirmed: the Android Auto session is TLS-encrypted, and the
decryption is genuinely done in software.** The connection log shows
a real TLS 1.2 handshake (`SSL version=TLSv1.2 Cipher
name=ECDHE-RSA-AES128-GCM-SHA256`). `libAndroidAuto.so` (the library
`sink` actually delegates `GalReceiver`'s implementation to --
`GalReceiver::init/start/shutdown` are undefined/imported symbols in
`sink` itself) has **OpenSSL statically linked in**: real
`AES_encrypt`/`AES_decrypt`/`AES_cbc_encrypt`/`EVP_aes_128_cbc`
symbols, not stubs, and critically **no separate `libssl.so`/
`libcrypto.so` dependency** -- confirming OpenSSL is compiled directly
into this library, not delegated to some external accelerated path.

**Refined mechanism**: decode itself is hardware-offloaded and cheap
for the CPU, but the AES decryption that has to happen *before*
`hx170dec` can even start on a frame is 100% software, on the single
core, with no hardware assist anywhere in this SoC. Audio and video
are multiplexed over the same encrypted AA session -- video's data
volume is far higher than audio's, so decryption cost scales heavily
with video bitrate/frame size, not audio. If both are demultiplexed
from the same decrypted stream in a serial receive pipeline, a large
video frame's decrypt work directly delays whatever audio data is
queued immediately behind it. This fits "recurs throughout the
session" (the user's correction to the earlier one-time-startup
theory) far better than video decode ever could, since decrypt cost
tracks ongoing video content complexity continuously, not a one-time
pipeline-setup burst.

**Still not confirmed with live evidence** -- this is now a precise,
well-evidenced *candidate* mechanism (single-core + no crypto hardware
+ real statically-linked software AES + audio thread with zero
priority protection), not a proven root cause. The `SCHED_TRACER`
capture and/or the `chrt -f` no-rebuild experiment proposed above are
still the needed next steps -- if a sched trace shows the CPU spending
time in `libAndroidAuto.so`'s AES routines (or generally in `sink`'s
receive/demux code) exactly when the audio thread should be running
but isn't, that would confirm this mechanism directly rather than by
inference.

### Confirmed content detail + third candidate mechanism + live test plan for next hardware session (2026-07-28, same day)

User confirmed a useful detail: the chopped-up audio is the *correct*
content in tiny pieces, not corrupted/garbled data. This matches a
genuine ALSA XRUN exactly (the data that does get through is real,
just interrupted at buffer-refill boundaries) -- confirms the
mechanism already identified rather than suggesting a new one.

**Third concrete candidate found, alongside AES-decrypt and RTW's
periodic watchdogs**: this system's MCU UART runs in **PIO mode, not
DMA** -- confirmed directly in the boot logs ("no TX/RX DMA channel
available (no platform data) -- using PIO/interrupt-driven TX/RX
instead"). That means every single byte of MCU communication requires
a real hardware interrupt and immediate CPU service, not a bulk DMA
transfer. If the MCU is chatty (status/CAN/GPIO reporting), this is a
genuine, driver-level, traffic-proportional interrupt-load source --
structurally different from AES (CPU-bound, data-driven by video
bitrate) and RTW's watchdogs (CPU-bound, purely time-driven), and a
direct answer to "is there a driver interrupting the stream."

**Test plan for the next hardware session, cheapest check first (not
yet run)**:

1. **`/proc/interrupts`, diffed across a reproduced choppy episode --
   no ftrace setup needed, fastest possible check:**
   ```sh
   cat /proc/interrupts > /tmp/irq_before.txt
   # reproduce the choppy audio
   cat /proc/interrupts > /tmp/irq_after.txt
   diff /tmp/irq_before.txt /tmp/irq_after.txt
   ```
   Whichever IRQ line jumped disproportionately during the episode
   directly points at the offending driver (MCU UART vs. WiFi vs.
   something else) without needing to interpret a full trace.

2. **If #1 isn't conclusive, the already-staged ftrace tracers:**
   ```sh
   mount -t debugfs none /sys/kernel/debug 2>/dev/null
   echo sched_switch > /sys/kernel/debug/tracing/current_tracer
   # or: echo irqsoff > /sys/kernel/debug/tracing/current_tracer
   echo 1 > /sys/kernel/debug/tracing/tracing_on
   # reproduce choppy audio
   echo 0 > /sys/kernel/debug/tracing/tracing_on
   cat /sys/kernel/debug/tracing/trace > /data/sched_trace.txt
   ```
   `sched_switch` shows exactly what task runs instead of the audio
   thread each time it's off-CPU; `irqsoff` catches the longest
   interrupts-disabled window if the cause turns out to be kernel-side
   lock/IRQ contention rather than plain scheduling loss.

User is testing at the device tomorrow (2026-07-29). This is the plan
to run then -- three concrete candidate mechanisms now identified
(software AES decrypt of the video-heavy AA stream, RTW's periodic
watchdog/calibration timers, PIO-mode MCU UART interrupt load), none
confirmed yet, `/proc/interrupts` diffing is the fastest way to start
distinguishing between them.

### The WiFi-to-buffer data pipeline, traced through the binary (2026-07-28, same day)

User asked what the actual pipeline is from a WiFi frame arriving to
landing in the ALSA buffer. Traced this via disassembly rather than
inferring from architecture docs.

**Pipeline, as it actually exists in `sink`**:
1. Kernel: `rtl8811cu` receives 802.11 frames -> TCP/IP stack -> the
   AA session's TCP socket receive buffer (over the `carplay_fc9f` AP
   link).
2. **`PipeTransport::read(void*, unsigned int)`** -- the generic
   transport reader, used for both wired and wireless. Confirmed via
   `typeinfo for` symbols that only three concrete `Transport`
   subclasses exist in the whole binary: `Transport` (abstract),
   `PipeTransport`, `RfcommTransport` -- no separate WiFi/TCP-specific
   class. Caps each read at **4096 bytes**.
3. Delegates to **`Accessory::readBytes(unsigned char*, int, int*,
   int)`**, which branches by connection type: `libusb_bulk_transfer()`
   for USB, or **`select()` then a plain `read()`** on the socket fd
   for WLAN -- confirmed directly in the disassembly.
4. Decrypted (OpenSSL AES, statically linked in `libAndroidAuto.so`)
   and demultiplexed by **`GalReceiver::messageRouter()`** into
   logical channels (audio/video/control).
5. Audio payloads -> `LinuxAudioSink` -> `AlsaHandle::play()` ->
   `snd_pcm_writei()`. Video payloads -> `VideoDecoder` -> `hx170dec`.

**Structural implication, initially framed as a strong lead**: audio
and video are multiplexed over the **same single TCP connection**,
read through the same 4096-byte-at-a-time call -- only one `Transport`
instance per session (matches "Transport type is wlan!" logging once,
not per-channel). A large video frame (easily tens of KB) requires
many successive reads; whatever thread loops on `PipeTransport::read()`
has to work through the whole video message before reaching audio
data queued right behind it in the same byte stream -- a potential
head-of-line blocking mechanism, structurally distinct from CPU-load
theories.

### Checked against real stock -- and found something bigger: the deployed AA binaries don't match stock at all

Asked to check this finding against stock before trusting it --
this surfaced a much larger, independent problem.

**The architecture itself is confirmed identical in stock.**
Disassembled the genuine Prado dump's `sink`
(`firmware_dumps/Prado firmware dump/mtd6_rootfs/usr/bin/sink`):
same `Transport` class layout, `PipeTransport::read()` caps at 4096
bytes identically, `Accessory::readBytes()` has the identical
`select()`+`read()` (WLAN) / `libusb_bulk_transfer()` (USB) branch.
**Since stock has this exact same single-reader multiplexed-transport
design and does not stutter, this weakens the head-of-line-blocking
theory as a standalone, sufficient explanation** -- the architecture
alone evidently doesn't cause the problem on genuine stock hardware,
so if it contributes here, something else about our system must be
what turns a normally-harmless design into a real bottleneck (most
plausibly the single-core + zero-crypto-hardware + unprioritized-
audio-thread combination already documented above, making this
pipeline more failure-prone than it would otherwise be).

**But comparing byte-for-byte turned up a real, independent bug**: the
deployed `firmware_source/mtd6_rootfs/usr/bin/sink`,
`usr/lib/libAndroidAuto.so`, and `usr/lib/libAutoDongle.so` **did not
match the genuine Prado dump's copies at all** (different md5sums).
`git log` shows `sink` was introduced as a fresh 0->532304-byte
addition in commit `d2b2dbc` ("fix: correct corrupted paths in build
scripts from directory restructure", 2026-07-21) rather than ever
being copied from a verified-against-Prado source -- stock's real
`sink` is 527448 bytes, and the deployed one has at least one extra
exported symbol (`RfcommConnection::reSendVersionRequestMessage`) not
present in the Prado dump's copy.

**Provenance confirmed, not a mystery**: user identified the source --
these are genuinely from the **Holden** firmware. Verified directly:
extracted `firmware_dumps/Holden firmware update/rootfs.img` (a UBI
image, via `ubireader_extract_files`, same technique already used
once before in this project for the same dump) and both `sink` and
`libAndroidAuto.so` came back **byte-identical** (md5-verified) to
what `firmware_source` had. So this wasn't corruption or an unknown
build -- at some point (most likely during the July 21 directory
restructure) the wrong sibling dump's copy got pulled in for this
Prado-specific rootfs, probably because Holden and Prado share close
enough lineage that a Holden binary runs fine here without any
obvious error, masking the mismatch. `firmware_overlay` has no copies
of these three files to override this, so `firmware_source`'s
Holden-sourced copies are exactly what has been getting deployed and
tested this whole time on a Prado unit.

**Fixed**: restored all three files from the real Prado dump,
verified byte-identical via md5 afterward. `firmware_source/
mtd6_rootfs` is the base directly used by `build_bootable_sdcard.sh`
(confirmed in the script), and nothing in `firmware_overlay` shadows
these paths, so this takes effect automatically on the next build --
no script changes needed. Committed (`74ec4c9`), pushed. **Not yet
hardware-tested.**

**Why this matters for the investigation**: this was a real,
previously-unnoticed divergence from stock sitting directly in the
exact code path (`Transport`/`PipeTransport`/`GalReceiver`) this
session has been tracing for the audio-stutter investigation. Not
confirmed as the cause of the choppy-audio symptom -- but it's a
genuine bug independent of that question (the deployed AA SDK build
should match stock unless there's a documented reason it shouldn't,
and there wasn't one here), and different AA SDK build versions can
have real behavioral differences in exactly the kind of
buffering/threading/timing details this investigation cares about.
**This should be retested from a clean slate**: a lot of static
analysis this session (the pipeline trace, the read-chunk-size
finding) was done against the *wrong* binary -- worth confirming next
time whether the genuine stock binary's behavior differs in any way
now that it's what's actually deployed.

**CORRECTION, same day: the sink/libAndroidAuto.so/libAutoDongle.so
swap above was reverted per explicit instruction.** User identified
the deployed copies are genuinely sourced from the Holden firmware
(confirmed: extracted `firmware_dumps/Holden firmware update/
rootfs.img` via `ubireader_extract_files`, `sink`/`libAndroidAuto.so`
came back md5-identical to what was deployed) -- not corruption or an
unknown build, the wrong sibling dump's copy got pulled in during the
2026-07-21 directory restructure, most likely unnoticed because
Holden and Prado share close enough lineage that the Holden binary
runs without any obvious error on a Prado image. **Explicitly told not
to change the deployed file** -- reverted commit `74ec4c9` (revert
commit `289c750`), `firmware_source`'s copies are back to the
Holden-sourced binaries. Leave as-is; this is not currently being
treated as part of the active investigation.

### `pingThread`: a real, confirmed fixed-interval (1s) mechanism touching the same shared connection as audio/video (2026-07-28, same day)

User reported observing "a repeatable fixed interval interrupt to the
stream" -- a specific, valuable clue: fixed-interval (not data-driven)
rules out the AES-decrypt theory (which scales with video content, not
time) and points at a genuine timer-driven mechanism instead. Traced
a `pingThread` symbol spotted earlier in `sink` rather than guessing
further.

**`pingThread::run()` (`0x1f510`), disassembled directly**:
```
loop:
    if (stop-requested) break
    GalReceiver::sendPingRequest(timestamp, false)
    sleep(1)                          <- exactly 1 second, hard sleep(1) libc call
    if (missed_ping_count > 5) { log error, Transport::requestStop(); break }
```
**`sink` sends a ping request over the same `GalReceiver`/`Transport`
connection every single second, unconditionally, for the entire
session.** This is a confirmed, disassembly-verified fixed-interval
mechanism, not inferred from timing alone.

**Traced further into the write path**: `GalReceiver::sendPingRequest`
delegates to `Controller::sendPingRequest` (`libAndroidAuto.so`,
`0x12e6c4`), which builds a `PingRequest` protobuf message, marshals
it via `MessageRouter::marshallProto`, then calls
**`ProtocolEndpointBase::queueOutgoing(void*, unsigned int)`** -- it
does not write to the socket directly. This confirms a producer/
consumer design: a **separate writer thread** (matches the
`Transport::getWriterThread()` symbol found earlier, distinct from
the reader thread that pulls in audio/video data) dequeues and
actually sends the ping.

**The resulting candidate mechanism**: every second, the writer thread
sends the ping over the *exact same* TLS/socket connection the reader
thread is using to decrypt incoming audio and video data. If that
shared `SSL` object (or the underlying socket) needs synchronization
between concurrent read and write access -- a common real constraint
with OpenSSL, whose `SSL` object is not inherently safe for
simultaneous multi-directional I/O from two threads without explicit
locking -- a ping write on the writer thread could briefly stall
whatever the reader thread is doing at that exact moment. That is a
genuinely periodic, fixed-interval (~1s, matching "repeatable fixed
interval" directly) interruption to the stream, driven by a real,
confirmed mechanism, not a guess.

**This is now the strongest lead in the investigation** -- more
directly matching the reported symptom character than the AES-decrypt,
RTW-watchdog, or PIO-UART theories, all of which were either
data-driven or had confirmed intervals (2s for RTW) that didn't
obviously fit "repeatable fixed interval" as tightly as a confirmed
1-second `sleep(1)` loop touching the shared connection does.

**Not yet confirmed with live evidence.** Real next steps: (1) confirm
the actual observed interval matches ~1s (or a clean divisor/multiple
of it) during a live reproduction; (2) trace whether the writer
thread's dequeue-and-send actually contends with the reader thread on
a shared lock (would need `libAndroidAuto.so`'s `Transport`/writer-
thread-loop and `ProtocolEndpointBase` internals, not yet done); (3)
the already-planned `/proc/interrupts` diff and `sched_switch`/
`irqsoff` ftrace captures for tomorrow's hardware session would also
directly show this if reproduced -- watch specifically for periodicity
close to 1 second, not just "what's running."

### Comprehensive re-sweep per explicit instruction: "must be the kernel" (2026-07-28, same day)

User correction/constraint: `firmware_source`'s userspace binaries are
confirmed to be what's actually on the device's NAND right now (the
Holden-sourced `sink`/`libAndroidAuto.so`/`libAutoDongle.so` --
matches the revert above), so whatever is different from stock has to
be in the kernel. Also flagged a units slip in the earlier write-up:
the `play:225` interval is **375 milliseconds (0.375s)**, not
0.375ms -- 28 calls over ~10.5s. Asked for a genuinely thorough sweep
of every angle, not just the strongest one found so far.

**Reconciling `pingThread` with "must be the kernel"**: the finding
isn't invalidated by this constraint, it's sharpened. `pingThread`'s
`write()` (via `queueOutgoing()` and the writer thread) is the
*trigger*, issued from userspace once a second -- but any actual stall
has to physically happen in the kernel's TCP/IP stack or WiFi driver
servicing that write, not in `sink` itself. Two concrete kernel-level
mechanisms checked for how that trigger could turn into a real stall:

**1. No `TCP_NODELAY` anywhere -- Nagle's algorithm is active by
default. Confirmed, not inferred.** Swept both `sink` and
`libAndroidAuto.so` for any `setsockopt` call at all -- zero matches
in either binary. `TCP_NODELAY` is never explicitly set, meaning
Nagle's algorithm runs at the kernel's default behavior on this
connection. Nagle's algorithm interacting with the receiver's
delayed-ACK timer is a classic, extremely well-documented source of
periodic ~40-200ms stalls on connections carrying small, frequent
writes -- and a once-a-second tiny ping message is exactly the kind of
write that triggers it. This is purely kernel/TCP-stack behavior,
directly satisfying "must be the kernel," and unlike the half-duplex
theory below, could also explain stalls happening *more often* than
once a second if other small protocol/control messages hit the same
connection (not just the ping).

**2. WiFi chip is confirmed USB-attached** (`usbcore: registered new
interface driver rtl8821cu` in the boot log) -- consumer 802.11 USB
chips are essentially always single-radio/half-duplex at the PHY
level, meaning a TX operation (sending the ping frame) may require
briefly pausing RX on the same radio chain, a real, physically-driven
stall mechanism entirely in kernel/firmware territory. **Less
concretely confirmed than the Nagle finding**: swept the `rtl8811cu`
driver for a literal "pause RX to TX" primitive and found only
`rtw_xmit_ac_blocked`/`rtw_is_xmit_blocked`/`rtw_set_xmit_block` --
these look like WMM/QoS access-category TX flow control, not a direct
RX-pause-during-TX mechanism. The actual half-duplex behavior (if it
exists) most likely lives inside the RTL88xx chip's own closed
firmware, not visible in kernel driver symbol names -- can't confirm
or rule this out further via static analysis alone.

**Full inventory of every kernel-level angle considered this
investigation, for completeness**:

| Angle | Status |
|---|---|
| IRQ-disable window in `ARKFB_SET/GET_FB_ADDR` (~30x/sec, unrelated to audio content but shares the CPU) | Fixed, hw-tested, **no change** to stuttering -- ruled out |
| `SND_PCM_XRUN_DEBUG` (does ALSA's own buffer ever starve?) | Hw-tested clean in one prior session -- inconclusive re: this specific choppy window, not yet retested with current logging |
| Generic `dmaengine_pcm` framework vs. stock's custom `ark_pcm_*` platform driver | Real architecture difference, but traced to functionally-equivalent DMA calls -- weak candidate |
| `CONFIG_CPU_FREQ` / cpufreq governor stalls | Not present at all (fixed clock) -- ruled out |
| RT scheduling priority anywhere in `sink` or the kernel | None set anywhere -- confirmed absent, but this alone doesn't explain a *fixed interval* |
| `HZ`/`PREEMPT` model vs. stock | Confirmed identical (`HZ=100`, `PREEMPT_NONE` both) -- ruled out as a differentiator |
| Hardware AES/crypto engine | Confirmed absent in both stock and ours -- not a stock-vs-ours divergence, though software AES cost itself remains a live (if now lower-priority, since data-driven not fixed-interval) candidate |
| RTW driver's periodic watchdog/DM timers | Main one confirmed exactly 2s via disassembly -- doesn't match a sub-1s "repeatable fixed interval" report as tightly as the 1s ping does; sub-watchdogs' intervals not individually resolved |
| MCU UART in PIO mode (no DMA) | Confirmed -- traffic-driven, not obviously fixed-interval unless the MCU itself polls/reports on a fixed schedule (not checked) |
| DMA burst size / `dma_slave_config` exact values vs. stock | Still not resolved -- flagged as open several times, never completed |
| GIC interrupt-priority configuration (audio DMA IRQ vs. WiFi/USB/video IRQs) | Never compared against stock at all -- genuinely open |
| **`TCP_NODELAY` / Nagle's algorithm** | **Confirmed absent -- new strongest concrete candidate, see above** |
| WiFi USB chip TX/RX half-duplex serialization | Plausible, physically well-motivated, not confirmable via kernel driver symbols alone -- needs live/hardware evidence or vendor documentation |
| Head-of-line blocking in the shared audio/video `PipeTransport`/reader thread | Confirmed same architecture in stock (doesn't stutter) -- weakens this as a standalone explanation |

**Recommended priority for tomorrow's hardware session, given this
sweep**: (1) confirm the observed choppy interval's exact value against
1s (the ping period) -- if it's actually much less than 1s and
genuinely fixed, that would argue against the ping/Nagle theory and
toward something else entirely (RTW sub-watchdog timers, or a GIC/DMA
angle not yet checked); (2) the `/proc/interrupts` diff and `sched_switch`
capture already planned remain the fastest way to get real data rather
than continuing to add candidates from static analysis; (3) if a
kernel-side fix is wanted to test the Nagle theory specifically without
waiting for a full trace, forcing `TCP_NODELAY` on this connection
would need either a kernel-side `sysctl net.ipv4.tcp_*` workaround
(imprecise, affects all sockets) or an `LD_PRELOAD` shim intercepting
`connect()`/`socket()` to call `setsockopt(..., TCP_NODELAY, ...)` on
the AA session's socket specifically (same complexity tradeoff as the
stdbuf shim discussed and deferred earlier -- not proposing to build
this now, just noting it's the concrete next step if this theory is
confirmed).

### DMA burst-size and interrupt-controller deep dive, requested to fill the wait before hardware testing (2026-07-29)

**DMA burst size**: ours is `maxburst=16` in `ark1668_i2s.c`
(`snd_dmaengine_dai_dma_data.maxburst`), which `ark-dma.c`'s
`convert_burst()` turns into DW_DMAC's `MSIZE=3` encoding (`fls(16)-2`)
-- a standard, unremarkable burst length. Traced stock's `ark_pcm_open`
(`vmlinux.elf`) far enough to confirm it goes through the **same
generic `__dma_request_channel()` DMA-engine core API** our driver
uses -- not some entirely bespoke mechanism -- but couldn't pin down
stock's exact burst constant from static disassembly (most likely
passed via a filter/private-data struct to the channel request, not
visible as a separate `dma_slave_config`-style call at the site
traced). **Inconclusive, no evidence of a difference, not fully
resolved.**

**Interrupt controller -- correction and a real finding**: this SoC
uses **ARM PL192 VIC** (`compatible = "arm,pl192-vic"` in the DTS),
not GIC. Checked the mainline Linux driver (`drivers/irqchip/
irq-vic.c`) for any priority-configuration code -- **none exists at
all**. This VIC driver has no software-configurable per-IRQ priority
scheme; dispatch order is fixed by hardware vector wiring. So "GIC/VIC
priority misconfiguration" isn't a real axis of difference from
stock here -- there's nothing to compare or misconfigure.

**Checked IRQ line sharing between audio DMA and USB/WiFi -- ruled
out.** DTS confirms distinct IRQ numbers: audio DMA controller = IRQ
16, `usb0` (musb) = IRQs 14/13, `usb1` = IRQs 8/7. No physical line
sharing, despite both drivers requesting `IRQF_SHARED` (permissive,
not actually shared with anything in practice per the DTS).

**But this surfaced a third real, concrete kernel-level mechanism**:
both `ark-dma.c`'s DMA-controller IRQ handler and `musb_ark.c`'s USB
IRQ handler (`ark_musb_interrupt` -- the controller carrying *all*
WiFi traffic, since the WiFi chip is USB-attached per the earlier
finding) are registered via plain **`request_irq()`, not
`request_threaded_irq()`** -- confirmed via grep, neither is threaded.
Disassembled/read `ark_musb_interrupt`: it does real, synchronous
register reads (`MUSB_INTRRX`/`MUSB_INTRTX`/`MUSB_INTRUSB`) and (via
the generic MUSB core it calls into) FIFO processing entirely inline,
in hard-IRQ context -- a well-known characteristic of the mainline
MUSB driver in PIO mode, not something specific to our port.

**On a confirmed single-core system, this matters independent of IRQ
line sharing**: any hard-IRQ handler running -- on any line -- masks
the CPU from servicing other pending interrupts for as long as it
runs, simply because there is exactly one core to run interrupt
handlers on at all. WiFi USB traffic (which is also what carries the
once-a-second `pingThread` write and the ongoing AA audio/video data)
being serviced in `ark_musb_interrupt`'s hard-IRQ context can
genuinely, measurably delay the audio DMA controller's own
period-complete interrupt (IRQ 16) from being serviced -- a third,
independent candidate mechanism alongside the `TCP_NODELAY`/Nagle
theory and the WiFi-chip half-duplex theory, and one that doesn't
require either of those to also be true.

**Where this leaves the investigation, going into hardware testing**:
three real, non-mutually-exclusive kernel-level candidate mechanisms
now on the table, all consistent with "must be the kernel" and none
requiring the userspace binaries to be anything other than what's
already deployed:
1. No `TCP_NODELAY` -- Nagle's algorithm stalls on `pingThread`'s
   small once-a-second write (or other small writes).
2. WiFi USB chip TX/RX half-duplex serialization (plausible, not
   confirmable via kernel driver symbols alone).
3. Non-threaded hard-IRQ USB handling delaying audio DMA's own IRQ on
   a single-core system, independent of (1) and (2).

All three would be directly visible in the already-planned
`sched_switch`/`irqsoff` ftrace capture -- (1) and (2) would show up
as the audio thread blocked/waiting around WiFi TX activity; (3) would
show up directly as `irqsoff`'s longest-interrupts-disabled window
landing inside `ark_musb_interrupt` or its callees. The `/proc/
interrupts` diff remains the fastest first check (a spike in the USB0/
USB1 IRQ counts during a choppy episode, disproportionate to DMA's own
count, would support (3) specifically).

### First live hardware evidence for this whole thread (2026-07-29)

User began live testing with the fully-instrumented kernel. Several
real, concrete data points collected so far, none individually
conclusive but all pointing the same direction.

**`/proc/asound/card0/pcm0p/sub0/status` polling**: `state: RUNNING`
never once caught `XRUN`, expected and not contradictory -- the
recovery in `AlsaHandle::play()` calls `snd_pcm_prepare()` immediately
after detecting `-EPIPE`, so the state flips back to `RUNNING` within
microseconds, far faster than a manual poll can catch. Also noted:
`appl_ptr` read as a constant `0` across every poll while `hw_ptr`
climbed steadily -- most likely a reporting quirk in this proc-status
path rather than a real timing problem, not pursued further.

**Interleaved with those polls, `play:225` (the XRUN/EPIPE recovery
path) was firing roughly once per second, continuously, for the whole
~7+ second window shown** -- not rare, an ongoing condition throughout
normal playback. A burst of ~13 consecutive `play:225` lines appeared
right at stream teardown (`playbackStopCallback`) -- most likely
normal end-of-stream buffer draining, not part of the ongoing
mechanism. The ~1/sec cadence remains a close match for `pingThread`'s
confirmed 1-second cycle.

**Two real `pcm_dmaengine: period jitter` lines** (our own
instrumentation, first time it's fired on real hardware):
```
actual=27105864ns expected=21333333ns (+27%)   <- period 5.77ms late
actual=15551074ns expected=21333333ns (-27%)   <- next period 5.78ms early
```
A genuine delay-then-catch-up pattern (the two nearly cancel out over
two periods). Real, measurable DMA-side jitter, but ~5.8ms is smaller
than would be needed to drain a 341ms-deep buffer to XRUN by itself --
either a separate, smaller artifact, or one instance of something that
occasionally compounds into a larger stall not yet captured.

**`irqsoff` trace (twice)**: first run showed the single longest
interrupts-disabled window (~305-353us) occurring in the context of
**`PipeWrit`-238** -- the transport writer thread (matches
`Transport::getWriterThread()`, the same thread that sends
`pingThread`'s ping and other outgoing messages). Second run (tracer
apparently not reset between tests) caught a boring, expected
idle-wake cycle (`<idle>-0` -> `arch_cpu_idle` -> `__irq_svc`) instead
-- not meaningful, normal ARM idle behavior. ~300-350us is real but
too short to explain a full audible dropout alone.

**`wakeup` latency tracer**: caught a 479us scheduling-latency event
for a task named `msgfunc1`-168, running with genuine real-time
priority (`policy:2`=`SCHED_RR`, `rt_prio:50`). Identified via `strings`
match: this is `carplay`'s `MsgQueue::SendMsgFunc` thread (symbol
`_ZN8MsgQueue11SendMsgFuncEPv`) -- **`carplay` runs its own
RT-priority message-queue thread continuously, even with no CarPlay
device connected** (this session is entirely `sink`/Android Auto).
The trace shows `PipeWrit`-238 running immediately before `msgfunc1`
was woken but had to wait ~479us to actually be scheduled. Real,
independent corroboration that the writer thread's activity causes
measurable scheduling perturbation on this system -- if it can delay
a genuine RT-priority thread by ~479us, `sink`'s own audio-writing
thread (confirmed zero priority protection) is at least as vulnerable.
Still too small in magnitude to be the sole explanation on its own.

**Pattern across all of the above**: three independent small
perturbations (300us, 305-353us, 479us) all coincide with `PipeWrit`
(the AA transport writer thread) being active, consistent with -- but
not yet proof of -- the `TCP_NODELAY`/Nagle and/or non-threaded-USB-IRQ
theories. None individually explains the full-scale audible dropout;
either they compound, or a larger event (matching the ~5.8ms jitter
or bigger) is still uncaptured.

**Not yet obtained**: a proper `sched_switch` event-based trace over a
full, multi-second window (attempted twice; first attempt likely
never actually left `irqsoff` set, second produced the `wakeup`
summary above instead of the full context-switch log requested) --
this remains the key missing piece to see the complete picture rather
than isolated worst-case snapshots. Also still wanted: a `dmesg` dump
from the *same* window as a sched trace, to line up jitter/XRUN
timestamps against exactly what the CPU was doing at each one.

### The full `sched_switch` trace obtained, and a major reframing (2026-07-29, same day)

Got the real, full `sched_switch` event trace (22628 entries,
kernel-uptime `1069.7s`-`1088s`, ~18 seconds) plus the terminal
transcript of the app-level output from the exact same test run.
User confirmed stuttering genuinely happened during this window.

**Analyzed the trace programmatically**: extracted all 901
`dmaengine_pcm_dma_complete` (period-jitter) events -- worst deviation
found was only +-19.3% (~4ms), far too small to explain a real XRUN.
Checked the biggest gap between *any* two consecutive trace entries
across the whole 18-second window -- **~15 milliseconds, the largest
gap anywhere in the entire trace.** No scheduling stalls, no
IRQ-disabled windows, nothing anomalous at the CPU/DMA level for the
whole session.

**This is a genuinely decisive negative result for the CPU-scheduling
theories.** If real stuttering happened during this window (confirmed)
and the CPU/DMA-side trace shows completely healthy, normal activity
throughout, that's strong evidence against every CPU-scheduling-based
mechanism examined so far -- the `PipeWrit` writer-thread IRQ-disable
window, the non-threaded USB hard-IRQ theory, general CPU starvation.
If the CPU were the bottleneck, this trace would show it. It doesn't.

**But cross-referencing the terminal transcript from the same test run
revealed the real pattern**: the ~50 `play:225` (XRUN-recovery) events
are **concentrated in a dense burst immediately after
`playbackStartCallback:63 status=39` fires** (media playback starting)
at `1071.892s` -- not spread evenly across the whole ~18-second window.
After that initial burst, the rest of the session (until
`playbackStopCallback` at `1089.858s`) produced no further `play:225`
lines at all. **This is a startup ramp-up problem, not an ongoing
steady-state scheduling contention problem** -- which is exactly why
the CPU/DMA trace over the same window came back clean: after the
initial burst, playback genuinely stabilized and ran normally at the
CPU/DMA level for the rest of the session. This also retroactively
explains the very first `v20` log finding (choppy audio confined to
the first ~10s of playback, then paused) and the manual `/proc/asound/
.../status`-polling test, which likely also happened to sample during
a similar startup window rather than true steady-state playback.

**New leading theory: TCP slow start.** The "rough for the first few
seconds, then smooth" pattern is the textbook signature of a freshly-
active TCP connection's congestion window ramping up from a small
initial value rather than delivering full throughput immediately. If
AA's audio+video data starts flowing while this connection is still
in its slow-start ramp, throughput would genuinely be constrained and
bursty for the first several seconds -- producing exactly this
pattern, with nothing visible in CPU/DMA-side tracing since the
bottleneck is the TCP stack's congestion-control state, not anything
the CPU is doing. Still purely kernel-level (`tcp_slow_start_after_idle`,
initial congestion window, TCP stack internals all live in the
kernel), and it cleanly explains "why not stock" too: if stock's
kernel has a different initial-cwnd default, or its WiFi driver
reaches full link rate faster, the same slow-start ramp would simply
finish before becoming audible.

**Not yet checked, real next steps**: compare our kernel's TCP
slow-start/initial-congestion-window defaults and
`tcp_slow_start_after_idle` sysctl against stock's (if determinable);
check `iw`/WiFi driver link-rate ramp-up timing during the first few
seconds after a fresh AA connection (does the radio negotiate/settle
into full link speed quickly, or does it also ramp up slowly, which
would compound with TCP's own slow start); a live packet-level
capture (if `tcpdump`-equivalent tooling can be gotten onto the
device) during a stream-start event would be the most direct way to
confirm or rule this out definitively.

**Status of the earlier three CPU-scheduling candidates (Nagle/
`TCP_NODELAY`, WiFi USB half-duplex, non-threaded USB hard-IRQ)**:
not disproven outright (they could still matter for a different
class of stutter, e.g. if choppiness genuinely recurs deep into a
long-running session as the user separately reported), but
de-prioritized below TCP slow start for explaining *this* specific,
now well-characterized startup-burst pattern.

### Mixer/mute RE-mistake check: I2C path cleared, but the audible mute mechanism confirmed (2026-07-29, same day)

User asked directly: could our own RE of the drivers/audio stack have
broken the mixer or mute controls, and separately, is audio being
continuously muted and unmuted?

**BD37033 external-amp userspace I2C path -- checked, cleared as an RE
mistake.** `Sound_BD37033::resetSpeakerAtts`/`muteSpeakerAtts` in
`libMsnSound.so` call `I2COperator::writeData()`, fully implemented in
`libMsnCommons.so`. Disassembled `I2COperator::I2COperator(uchar,uchar)`
and the underlying `arki2c_open`/`arki2c_write` helpers directly:
they do a completely standard kernel `i2c-dev` sequence --
`snprintf("/dev/i2c-N")` -> `open()` -> `ioctl(fd, I2C_SLAVE=0x0703,
addr>>1)` -> `write(fd, buf, len)`. This is not custom register-level
GPIO bit-banging in userspace (which would have been the worst case
for this single-core system -- a busy-loop with zero opportunity to
yield). It's a normal blocking syscall through whatever kernel i2c
driver backs `/dev/i2c-N` (i2c-gpio in our case). The chip's confirmed
always-fails-with-timeout behavior (documented earlier in this file)
is real, but it fails through the kernel driver's own retry/timeout
logic, not through anything `I2COperator` itself does wrong. **Ruled
out as an RE mistake and de-prioritized as a stutter mechanism.**

**Mute/unmute: confirmed happening repeatedly, and now tied directly
to the `play:225` XRUN burst.** Grepped the kernel-side `digital_mute`
and `elem_write` logging (added earlier this session) against
`new uboot new kernel baseline v21_260729.txt`. In a ~1.1-second
window at stream start (kernel-uptime `14.815s`-`15.9s`),
`ark1668-sddac: digital_mute` toggles **24 times**, alternating
mute=1/mute=0 at 20-45ms intervals, interleaved with `softmaster*`
`elem_write` volume-ramp writes. After `15.9s` it goes completely
silent -- no further mute activity until `41.36s`.

Cross-checked against `ark1668-i2s: trigger cmd=1/cmd=0 stream=0
(playback)` logging in the same file: the START(cmd=1)/STOP(cmd=0)
trigger pairs fire at the **identical timestamps and cadence** as the
`digital_mute` toggles in that same window. They are the same event,
not two separate mechanisms -- `ark1668-sddac`'s codec driver mutes
the DAC on `trigger(STOP)` and unmutes on `trigger(START)` (standard
pop/click suppression), so every trigger flap produces one audible
mute blip.

**This connects directly to the already-identified `play:225` XRUN
burst and explains the actual audible mechanism**: ALSA's
`snd_pcm_prepare()` (called by `AlsaHandle::play()`'s XRUN-recovery
path on every `play:225`) issues an implicit `trigger(STOP)` on a
still-running stream; once the refilled buffer crosses the
auto-start threshold, the next successful write auto-triggers
`trigger(START)`. If the following write underruns again within
milliseconds -- which it does, repeatedly, during the startup burst
-- the cycle repeats. So: **`play:225` (XRUN) -> implicit
`trigger(STOP)` -> `digital_mute(1)` -> buffer refills ->
`trigger(START)` -> `digital_mute(0)` -> repeat.** ~24 of these
cycles happen in just over a second at stream start, which is
plainly audible as choppy/segmented audio -- this is not a separate
bug, it's the literal mechanism connecting the already-confirmed
XRUN storm to what the user actually hears.

**Answers the user's two questions directly**: (1) no RE mistake
found in the mixer/mute/I2C code paths examined -- the codec driver's
mute-on-stop behavior is correct, standard behavior, just exercised
far more than it should be; (2) yes, audio is genuinely being muted
and unmuted repeatedly, dozens of times, but only during the
already-known ~1s startup window, not continuously through playback.

**Still open**: this confirms the downstream *symptom* mechanism, not
the upstream *cause* of the XRUN storm itself -- the TCP-slow-start
theory (or another network/buffer-starvation cause) is still the
leading candidate for why the XRUNs happen in the first place. A
useful next diagnostic: if TCP slow start is the real cause, a local
`aplay` test of a WAV file (no network involved at all) should play
back cleanly with none of this mute-flapping pattern -- which is
exactly the test the user is already running.

### TCP slow start deprioritized -- video is unaffected, so it isn't a shared-pipe bandwidth problem (2026-07-29, same day)

User pointed out the flaw directly: video isn't delayed or glitchy at
all during this same window, only audio chops. Audio and video are
multiplexed over the **same single TCP `Transport`** (confirmed
earlier this session -- only one `Transport` instance exists per AA
session; `GalReceiver::messageRouter` demuxes both channels from it).
A TCP-level cause -- slow start, Nagle, retransmission from packet
loss -- throttles the connection as a whole; it can't selectively
delay only the audio channel while leaving video completely clean.
**This rules out every TCP/network-bandwidth-level theory examined so
far** (slow start, Nagle/`TCP_NODELAY`, WiFi USB half-duplex,
retransmission) as the cause of the startup XRUN burst specifically.

**New theory, better supported by the actual binary architecture:
single-threaded reader/demux contention between audio and video, not
a network problem.** Symbol-table evidence from `sink`/
`libAndroidAuto.so`: there is exactly **one `GalReaderThread`** per
session, pulling all bytes off the shared `Transport` serially. Data
passes through a single decrypt stage (`SslWrapper::
decryptionPipelineEnqueue/Dequeue`), then `MessageRouter::
routeMessage()` demuxes by channel ID to per-channel handlers
(`AudioSource`, `MediaSinkBase`/`VideoDecoder`), with a `WorkQueue::
queueWork()` dispatch mechanism in between. This is architecturally
**one serial pipe feeding two very differently-buffered consumers**:
video has large frame buffers and a hardware decode pipeline that
easily absorbs a few extra milliseconds of reader-thread delay;
audio's ALSA buffer is tiny (~21ms periods, already established
earlier in this file) and has essentially zero tolerance for the
same delay. If the single reader thread spends time receiving/
decrypting/dispatching a large video frame (an I-frame is much
bigger than a P-frame or an audio chunk) right when an audio chunk
is due, the *audio* consumer starves while the *video* consumer
doesn't even notice -- exactly the asymmetric symptom observed.
This also fits the startup-only timing better than TCP slow start
did: the first ~1s of an AA session is precisely when an initial
video I-frame and audio stream setup both need to go out close
together, maximizing contention on the single reader thread; once
the session settles into steady-state P-frames the contention
disappears, matching the observed silence after ~15.9s.

**Not yet confirmed, concrete next steps**: (1) check whether
`AudioSource`'s frame delivery to `AlsaHandle::play()` is synchronous
on the `GalReaderThread` itself or dispatched onto a separate
consumer thread via `WorkQueue` -- if synchronous, the reader thread
literally cannot start the next audio read until the video frame
ahead of it in the demux/decrypt pipeline is fully processed, which
would be a smoking gun; (2) in a fresh capture, check whether video
I-frame arrival (visible via frame-size/type if logged, or via
`hx170dec` decode-start timing) lines up with the `play:225`/
`digital_mute` burst timestamps; (3) the already-pending `aplay`
local-WAV test remains useful even under this theory, since it
removes the network+demux pipeline entirely -- clean playback there
would confirm the ALSA/DMA/codec path itself is fine and the problem
really is upstream in the shared reader/demux stage, not ruled out
by this reframing.

### This-session reader-thread theory ALSO doesn't fit: user confirms recurrence at any point, even with static video (2026-07-29, same day)

User corrected the reader-thread/video-data-volume theory above almost
immediately: the audio stutter is **not confined to startup** -- it
recurs at any point during normal operation, including while video
content is largely static. A "large video I-frame contends with audio
on the shared reader thread" mechanism can't explain this: static
video content means little/no large frame data is arriving to cause
contention, yet the audio glitch still happens. **This deprioritizes
the reader-thread-contention theory** (still plausible as a
contributor to the specific startup burst already captured in logs,
but not the general/recurring mechanism).

User's own framing: "we must be doing something different to stock in
our kernel decoding path." This redirects the search specifically to
kernel-side video/display code that (a) fires continuously regardless
of point in the session, and (b) fires regardless of whether video
content is actually changing -- i.e. driven by fixed hardware timing,
not by data volume or network delivery.

**Found a strong structural match**: `ark1668_lcdfb_interrupt()`
(`drivers/video/fbdev/arkmicro/ark1668_lcdfb.c`) is the LCDC's own
vsync/frame IRQ -- fires at the **physical panel refresh rate**, a
fixed hardware timing signal, completely independent of whether AA
video content is static or changing, for as long as the display is
powered on (i.e. always, for the whole time the unit is running).
This handler is a plain, non-threaded `request_irq()` handler (same
class of mechanism already flagged this session for the non-threaded
USB IRQ theory), and on every single frame it unconditionally calls
`schedule_work(&sinfo->task)`. That work item calls
`ark_itu656_display_int_handler()` in
`drivers/soc/arkmicro/itu656/ark1668_vin.c` -- the **backup-camera**
video-input handler, part of `CONFIG_ARK1668_ITU656`, which this
project re-enabled in the defconfig on 2026-07-26 specifically to fix
the reverse-camera feature (see
[[project_stock_kernel_boot_backcar_investigation]]). When the camera
isn't active it returns quickly after checking `dvr_dev->work_status`,
but the ISR still pays for a full `schedule_work()` -> kworker wake ->
schedule round-trip every single frame, unconditionally, on this
single-core system, for the entire time the display is on.

This fits both of the user's corrections directly: **recurs at any
point in operation** (driven by continuous panel-refresh timing, not
a one-time startup event) and **independent of video content**
(hardware vsync timing, not data volume). It's also a strong fit for
"different from stock": ITU656 backup-camera support is something
this project reconstructed/re-enabled from scratch (no real stock
driver source available for it), so a difference in how often/how
this work gets scheduled compared to whatever stock's actual
(closed) driver does is very plausible.

**Not yet proven sufficient to explain the audible magnitude of the
mute-flap pattern by itself** -- the per-call overhead when the
camera is inactive is small (a few flag/pointer checks), and this
needs live correlation, not just architectural plausibility. **Added
logging (commit f83033fac, `linux-arkmicro`, not yet hw-tested)**: a
per-frame `trace_printk` in the vsync ISR itself (ftrace buffer,
correlatable directly against the `pcm_dmaengine` period-jitter
trace_printk already in place), plus a `ktime`-measured runtime of
`ark_itu656_display_int_handler()` with a rate-limited `printk` if any
single call exceeds 1ms. Test: reproduce the stutter mid-session with
static video, then check `cat /sys/kernel/debug/tracing/trace` for
`lcdfb vsync irq`/`itu656 display task` lines lined up in time against
the `pcm period`/`digital_mute`/`play:225` lines already logged
elsewhere -- if vsync IRQ timing or task runtime spikes coincide with
audio events even when video is static, that's the confirmation this
theory needs.

### Broader RE audit: "look at any RE that would create issues like this" (2026-07-29, same day)

User asked for a systematic sweep, not just the one lcdfb finding.
Grepped every `request_irq`/`request_threaded_irq` call site across
`drivers/video/fbdev/arkmicro/`, `drivers/soc/arkmicro/`, and
`sound/soc/arkmicro/`: **not a single one uses
`request_threaded_irq`** -- every video/camera/DMA/I2S interrupt
handler in this whole tree is a plain non-threaded hard-IRQ handler.
This is a systemic characteristic of the tree, not an isolated
mistake (and top-half-does-register-work-then-defers-to-workqueue is
a normal Linux idiom in itself; the concern is specifically about how
much work happens in the hard-IRQ top half, and how often it fires).

**Also found: two alternate ITU656 driver implementations exist in
the source tree** (`ark1668_vin.c` and `ark1668_itu656.c`, both
exporting the same `ark_itu656_display_int_handler` symbol), gated by
different Kconfig options (`CONFIG_VIDEO_ARK1668_VIN` vs
`CONFIG_ARK1668_ITU656`). Only `CONFIG_ARK1668_ITU656=y` is set in
this project's defconfig, so `ark1668_itu656.c` is the file actually
linked -- the earlier analysis of `ark1668_vin.c`'s version of this
function (in the section above) was describing **dead code that
isn't even compiled in**. Corrected: the real active
`ark_itu656_display_int_handler()` (in `ark1668_itu656.c`) also
exits early on `!dvr_dev->work_status`, so it's cheap when the camera
isn't active -- weaker than initially thought as a standalone cause.

**Found something stronger while correcting that: the ITU656
hardware capture IRQ itself.** `ark_itu656_int_handler()` in
`ark1668_itu656.c` (registered as `"dvr_itu656"`, `IRQF_SHARED`,
non-threaded) does a register read (`ARK1668_ITU656_ISR`), a register
write (clear `ARK1668_ITU656_ICR`), and takes
`dvr_dev->spin_lock_irqsave` -- **all before** the `work_status`
check. This overhead happens on *every single interrupt from this
hardware line*, regardless of whether the backup camera is actually
being displayed. No `clk_prepare_enable`/power-gating for the ITU656
peripheral was found anywhere in this driver -- the clock/interrupt
source isn't obviously toggled based on `work_status` at all. If the
camera sensor free-runs (common for reverse cameras, wired always-on
so the picture appears instantly on gear change rather than after a
sensor warm-up), this non-threaded hard-IRQ could be firing
continuously at the camera's field rate (~50-60Hz) the entire time
the unit is powered, **completely independent of AA video content or
session state** -- a much better structural match for "recurs at any
point in operation, even with static video" than the vsync/lcdfb
theory above.

**Also found, documented but lower-priority (gated behind
`work_status`, so only matters when the camera is actively
displaying, e.g. actual reverse gear)**: a real busy-wait spin loop
in this same hard-IRQ handler --
`while(dvr_dev->deinter_status && timeout--);` (10000 iterations,
`ark1668_itu656.c` line 1333) -- executing with interrupts disabled
via the held `spin_lock_irqsave`. Genuinely bad practice in hard-IRQ
context regardless of relevance to the general stutter; worth fixing
on its own merits, separately from this investigation.

**Added logging (commit 05ecc4c56, `linux-arkmicro`, not yet
hw-tested)**: `trace_printk` at the very top of
`ark_itu656_int_handler()`, before the `work_status` gate, logging
`intr_stat` and `work_status` on every single firing. **Cheapest
possible test, no ftrace even required**: `cat /proc/interrupts |
grep -i itu656` at two points during normal driving with no reverse
gear engaged -- if the `dvr_itu656`/`dvr_deinterlace` counts are
climbing, this confirms the hardware IRQ fires continuously
regardless of camera state, which would make this the strongest
candidate found so far for a content-independent, whole-session,
single-core-CPU-contending mechanism. If confirmed, the ftrace
capture (correlating `itu656 hw irq` lines against
`pcm period`/`digital_mute`/`play:225`) is the next step to prove
actual audio impact, not just IRQ presence.

### SoundAdapter `ICType` experiment: negative result, and a strong new lead from a real captured stutter (2026-07-29, same day)

User found a genuine stock log showing `SoundAdapter Create Failed,
Not Support ICType: 0` and, since this project's own `.ini` was
already set to `ICType`/`SoundType=3` (the confirmed-correct BD37033
value, see the "Background: how this started" section above), set it
to `0` to test whether matching stock's apparent value changed
anything. **Result: no crash** (the documented `SettingWindow::
onFirstInit()` NULL-deref crash path wasn't hit this run), **and the
audio still stuttered.** Expected and consistent: `SoundType`/`ICType`
only selects MsnCoreApp's userspace `SoundAdapter` class for talking
to the external BD37033 amp over I2C -- a completely separate layer
from the kernel ALSA/PCM data path the stutter mechanism lives in.
Ruled out as a stutter-relevant setting.

**The same test run (`docs/logs/new uboot new kernel baseline
v22_260729.txt`) also contained the richest live capture of a real
XRUN burst yet, correlated against the newly-added `hw_params`/
`digital_mute`/`trigger` logging plus `switchToAppAudioChannel`/
`setAppVolumeMute` app-level log lines.**

**First, a genuinely new pattern found and then ruled out as the
general mechanism**: at boot, t=20.4s-21.2s (well before `MsnCoreApp
version` even prints, i.e. before the app's main init), there's a
storm of ~18 `hw_params` calls, each paired with `digital_mute`/
`trigger cmd=1`/`cmd=0`, cycling through all **6** `asound.conf`
`softvol`/`softmaster` dmix slots in turn (`softmaster`,
`softmaster1`-`softmaster5`, distinct `numid`s 3-8) -- one open/mute/
trigger/close cycle per slot. This looks like MsnCoreApp priming/
self-testing all 6 of its named audio-focus slots once at launch (the
DirectFB startup banner appears immediately before it in the log,
confirming the process has already started). **Checked whether this
recurs later and it doesn't** -- `hw_params` appears exactly twice in
the whole ~929-line log: this one boot-time storm, and a single
ordinary call at t=58.8s when AA's own audio sink is created
(`Creating new Audio sink...`, `plug:softvol2`) -- a normal one-time
setup, not a repeat of the multi-slot storm. **Ruled out** as the
general "recurs at any point" mechanism -- it's boot-only.

**But the actual XRUN burst at t=69.68s-76.29s (the same `play:225`
mechanism already root-caused as genuine ALSA XRUN recovery) has a
striking, tight correlation right at its onset:**

```
[69.679]  switchToAppAudioChannel, ModeApp: "MsnCarAuto"  AudioApp: "MsnCarAuto"  Channel: 0 Request: false
[69.680]  setAppVolumeMute "MsnCarAuto" false
[69.681]  ======== refreshAppAudioChannel 8388611 0
[69.682]  switchToAppAudioChannel, ModeApp: "MsnCarAuto"  AudioApp: "MsnCarAuto"  Channel: 3 Request: false
...
play:225  (x18)
```

`switchToAppAudioChannel`/`setAppVolumeMute` fire twice in immediate
succession right before the 18x `play:225` burst begins -- not a loose
same-window correlation like earlier candidates, but the literal
preceding action. This is the exact mechanism traced by disassembly
much earlier in this whole investigation (see the very first "Real
mechanism, confirmed via Ghidra decompile" entry in the project
memory / early sections of this doc): `switchToAppAudioChannel()` ->
`SoftVolCtrl::setMute()`/`setVolume()` -> `SoftVolCtrl::amixer_cset()`,
which does a **full `snd_ctl_open()`->`elem_info()`->`elem_read()`->
`elem_write()`->`snd_ctl_close()` cycle from scratch on every single
call**. That lead was explored early on and deprioritized because it
seemed tied only to a ~10-12s voice-session cycle that didn't match
the reported symptom frequency -- this new evidence is a direct timing
hit against an actual captured XRUN onset, not a loose frequency
argument, and revives it as a strong candidate.

**A second, earlier `switchToAppAudioChannel` call in the same log
(t=50.825s, when the UI switches into CarAuto mode, before AA's WiFi
link is even up) produces NO `play:225`** -- consistent with, not
contradicting, the theory: at t=50.8s there's no active PCM stream
being written yet, so nothing is available to glitch. The mechanism
plausibly fires at any audio-focus/channel-switch event (BT connect,
source change, voice prompt, phone call) regardless of video content
or session stage, but only produces an audible/XRUN-visible symptom
when it happens to land during active playback -- exactly the "recurs
at any point, even with static video" pattern already established,
and a much better fit than the USB/camera-IRQ leads chased over the
last several turns.

**Also explains the `ICType` test result cleanly**: `amixer_cset()`
operates directly on ALSA mixer controls via `snd_ctl_*`, entirely
independent of which `SoundAdapter` backend class is selected --
consistent with changing `ICType` having no effect on the stutter.

**A plausible cross-process mechanism, tying to earlier evidence**:
`MsnCoreApp` (running `switchToAppAudioChannel`) and `sink` (writing
AA's PCM audio) are separate processes, both routed through the same
shared `dmix`/`softvol` chain in `asound.conf`. ALSA's dmix
implementation coordinates concurrent clients via shared memory +
System V semaphores. This connects directly to the `Semop lock
failure Invalid argument` / `Semop unlock failure Invalid argument`
lines seen in the earlier MsnCoreApp-restart dmesg capture (this same
session, a few turns back) -- independent evidence of real contention
in exactly this shared IPC mechanism. Two separate captures now point
at the same architectural weak point: `MsnCoreApp`'s control-plane
`amixer_cset()` calls contending with `sink`'s active PCM writes
through the shared dmix/softvol layer.

**Not yet confirmed, concrete next steps**: (1) check whether
`switchToAppAudioChannel`/`setAppVolumeMute` (or the underlying
`amixer_cset()`) fires at other points during a longer session --
voice prompts, phone calls, source switches -- and whether those
correlate with reported stutter away from the AA-connect window; (2)
if reachable, time `SoftVolCtrl::amixer_cset()`'s actual duration on
this hardware (a slow `snd_ctl_open()`/`close()` round-trip against a
contended dmix semaphore could plausibly block for tens of
milliseconds, comparable to the ALSA buffer's ~340ms XRUN margin
established earlier); (3) a live semaphore-contention check (`ipcs -s`
during a reproduced stutter, or strace on both processes around a
`switchToAppAudioChannel` call) would directly confirm or rule out
the cross-process blocking theory.

### Critical correction: the stutter is NOT a startup-only burst that self-resolves -- it is continuous for the whole session, and doesn't recover on its own (2026-07-29, same day)

User clarified two things that materially change this investigation's
framing. First: every session captured so far shows the media stream
stopping (`playbackStopCallback`) a few seconds after starting -- this
was being read as the app/AA SDK giving up automatically due to poor
quality. **It isn't. The user manually presses pause to end these
test sessions.** Session-length numbers (v20's ~10.5s, v22's ~6.6s)
only reflect how long each test happened to run, not anything about
the underlying condition resolving. Second, and decisive: **the
stutter never stops or improves on its own -- "keeps stuttering,
never catches up," for as long as playback continues.**

This retroactively undermines the earlier "it's a startup ramp-up
burst, not steady-state contention" reframing (the section above,
built on a clean `sched_switch` trace over an 18s window) -- that
trace's window may simply have been a session the user paused
similarly early, not evidence the condition self-resolves. It also
weakens (without fully ruling out) the `switchToAppAudioChannel`/
`amixer_cset` theory as the *sole* explanation: that call fires
exactly once per session, at stream start, so on its own it can
plausibly explain the *first* XRUN but not why the storm continues,
unrecovering, for the entire rest of playback with no further trigger
event visible in the log. Whatever the real mechanism is, it must be
a *persistent* condition present for the whole duration of playback,
not a one-time disruption with lingering effects.

**DMA/buffering driver code audited directly against mainline, found
solid, not the cause.** In response to "are our buffering routines
correct," did a line-by-line comparison of `ark-dma.c`'s cyclic DMA
implementation (period-elapsed callback, residue/pointer tracking)
against the in-tree mainline reference (`drivers/dma/dw/core.c`).
Initially suspected `dwc_get_sent()` had an inverted read of the
hardware `BLOCK_TS` counter (counts-remaining vs counts-sent) -- would
have been a very plausible chronic, structural bug -- but it's a
byte-for-byte match with mainline's own identical function, same
comment, same logic. Not a bug. The two real DMA bugs this project
already found and fixed earlier (inverted residue formula in the
cyclic `tx_status` path; missing `BLOCK` IRQ unmask needed for cyclic
period-boundary detection) remain the only confirmed issues in this
layer, and both are already fixed. No new DMA/buffering-layer bug
found despite a careful, direct audit.

**DMA "is our USB slower than stock" check: inconclusive, one
real-but-cosmetic finding.** Compared boot logs: stock's driver
explicitly logs `"musb-hdrc musb-hdrc.0: ... using DMA, IRQ 14"`;
ours has no equivalent line (different driver entirely for the 4.19
port, not proof of a functional gap by itself). `is_dma_capable()`
compiles to `1` in our build (`CONFIG_USB_INVENTRA_DMA=y`,
`CONFIG_MUSB_PIO_ONLY` not set), same DMA-capable status as stock.
Found a real code smell in `musb_ark.c` -- `dma_off` is a single
`static int` shared across *both* USB controller instances rather
than per-instance -- but traced both branches of the code that reads
it and confirmed neither touches any hardware register, only a log
message; functionally inert, not a performance bug. Whether DMA is
actually *engaged* for real bulk WiFi transfers at runtime (vs. just
compile-time capable) remains genuinely unresolved -- would need a
live check (MUSB debugfs, or IRQ-count-vs-data-volume comparison), not
answerable from static code alone.

**Renamed `ark-dma.c`'s driver/log strings to match stock**
(`linux-arkmicro` commit `e8b9bb9fe`) purely for easier side-by-side
log comparison going forward: `"dw_dmac"`/`"DesignWare DMA
Controller"` -> `"ark_dw_dmac"`/`"Arkmicro DMA Controller"`, matching
stock's real 3.4 kernel dmesg exactly. Cosmetic only, DT probing is
unaffected (matches on the `"arkmicro,ark-dma"` compatible string).

### Major mixer-control-surface gap found via `audio log stock_260715.txt`, checked and ruled out as the stutter's cause (2026-07-29, same day)

Compared stock's real `amixer controls` output (`docs/logs/archived/
audio log stock_260715.txt`) against this project's own established
control list. **Stock has 39 mixer controls; our build has 8** (`Left/
Right Playback Volume` + the 6 `softmasterN` dmix controls). Stock has
all of that plus **22 real BD37033 "PA" hardware controls** (`PA
Mute`, `PA Volume`, `PA Fader-FL/FR/RL/RR/Sub1/Sub2`, `PA Input
Select`, `PA Loudness`, `PA Mixing-CH1/CH2`, `PA Sub-Input/LPF-FC/
LPF-Parse/Output Select`, `PA Reset`, etc.) **and 9 EQ controls** (`EQ
Bass/Middle/Treble`, each with gain/F0/Q) -- all entirely absent from
our card. `CONFIG_SND_SOC_BD37033 is not set` in the current
defconfig (briefly enabled 2026-07-19, disabled again) -- consistent
with this project's long, never-resolved BD37033 I2C timeout history
(see `docs/BD37033.md`).

**Promising theory raised, then checked and walked back with direct
evidence**: hypothesized that stock's `switchToAppAudioChannel`
mechanism might normally hit the real, fast `PA Mute`/`PA Volume`
hardware controls directly, while our build -- missing those entirely
-- falls through to the slower, IPC-heavy `softmasterN` path instead,
which would nicely explain the whole investigation. **Checked via
`nm -CD` on `MsnCoreApp`'s own binary strings: it only ever references
`"softmaster"`-prefixed control names, never `"PA Mute"` or similar.**
So `switchToAppAudioChannel` uses the same software `dmix`-routed
controls on stock too, regardless of whether BD37033 is present --
this theory doesn't hold up. **Also checked**: `Sound_BD37033::
resetSpeakerAtts`/`muteSpeakerAtts` (flagged in an earlier session's
log as firing right before a burst) does not appear anywhere in the
v22 log's runtime output at all, yet the identical XRUN burst still
happens at the same point in the sequence -- ruling this specific
mechanism out for at least this capture.

**Net result**: the missing 31 mixer controls are a real, separate,
worth-fixing functional gap (no EQ, no fader balance, no working
external amp control at all currently) but not demonstrably connected
to the audio stutter based on everything traceable in the available
logs. Static log analysis of this specific angle is exhausted; further
progress needs the pending `aplay` local-file test or a live `ipcs`/
`strace` capture during a reproduced stutter.
