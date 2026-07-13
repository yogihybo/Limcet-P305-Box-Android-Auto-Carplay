# Audio Subsystem Investigation

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
stretching:** see `docs/I2C_GPIO0_LCD_PIN_CONFLICT.md`. `i2c-gpio-0`'s
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
session) — this should resolve the write-timeout failures without
needing to touch the LCD/`i2c-gpio-0` conflict at all. rn6752 stays on
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
- [ ] Fix the I2C write failures first (likely candidate: remove/adjust
      `i2c-gpio,scl-output-only` on `i2c-gpio-0` in
      `Limcet Hardware/ark1668-limcet-prado.dts` to allow clock
      stretching, if the GPIO wiring supports bidirectional SCL) and
      re-test `start_msn` — if the crash disappears or the re-entrant
      call's parameters change, that confirms the I2C-failure theory
      without needing to touch `libSetting.so` at all.
- [ ] If the crash persists even with clean I2C, the uninitialized
      `sp+68` local in `sendSoundData()` (`0x3506c` in `libSetting.so`)
      is a genuine standalone bug regardless of trigger, and a binary
      patch there becomes the fallback plan.
- [ ] Note: the device **auto-reboots after this crash**
      (`crash3.strace`'s minidump-then-reboot sequence, reconfirmed live
      — `dmesg` shows `The system is going down NOW!` / `reboot:
      Restarting system` immediately after the crash), so each test
      iteration costs a full boot cycle.
