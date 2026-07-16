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
