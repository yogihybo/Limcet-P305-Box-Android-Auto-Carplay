# MCU UART Command Reference — canonical, cross-checked

**Supersedes**: `docs/historical/1.3_MCU_ADAPTERS.md` and `docs/historical/1.3.1_MCU_FIRMWARE_DECOMPILATION.md`
(both archived to `docs/historical/`, 2026-08-31) — those two docs and this project's own
`hardware/MCU/source/` (clean-room firmware) and `custom_ui/src/hal/mcu_input.cpp` had each
independently guessed at several command meanings **without cross-checking each other**,
and in multiple cases (below) they flatly disagree on what the same byte value means.

**Status legend**:
- ✅ **CONFIRMED** — real disassembly/decompile of either the real STM32 firmware
  (`hardware/MCU/can_app.bin`) or the real stock vendor app (`libMcuCenter.so`,
  `MCUAdapter_BoxP300`), with a cited real address.
- 🟡 **LIVE-CAPTURE CONFIRMED** — not from static disassembly, but from an actual captured
  UART trace on real hardware. Strong evidence, different kind of evidence than disassembly.
- ❌ **INVENTED / UNCONFIRMED** — asserted somewhere in this project with no real evidence
  behind it. Struck through below wherever a specific wrong claim was made.
- ⚠️ **CONFLICT** — two or more sources in this project assert different, unreconciled
  meanings for the same byte value. Both/all are shown; neither is assumed correct.

Full disassembly detail and reasoning for each confirmed row lives in
`docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md` — this doc is the flat lookup table, that one is
the deep trace. Frame layout throughout: `byte[0]`=`0x2E` sig, `byte[1]`=cmd, `payload[n]`
below means `byte[2+n]` unless a row says otherwise.

---

## SoC → MCU (commands the SoC sends)

The real inbound dispatch table (`0x0800B9E4` in `can_app.bin`) has exactly **9 entries** —
this set is closed and double-checked (cross-referenced between two independent disassembly
passes, 9/9 exact address matches). No command outside this list does anything on the real
MCU firmware.

| Cmd | Meaning | Payload / bitfield layout | Status | Note |
|---|---|---|---|---|
| `0x81` | Init handshake / keepalive, resets internal state slots | `payload[0] = 0x01` fixed, no further structure | ✅ CONFIRMED | handler `0x080088B5` |
| `0x82` | App foreground/mode change | `payload[0]`: `0x01`=Media, `0x02`=Navi/AA — plain enum, not a bitfield | ✅ CONFIRMED (that the SoC sends this) / ❌ what the MCU *does* with it is a clean-room guess | `handle_app_state()`'s GPIOB Pin0/Pin6 toggle has no disassembly citation |
| `0x84` | Audio route select | value masked to 4 bits (`&0xF`), ignored if `≥6`. `0`→sends `"AT+AUDROUTE=1\r\n"` + shared relay-dispatcher state `0`; `3`→`"AT+AUDROUTE=2\r\n"` + state `1`; `1/2/4/5`→state field updated only, no relay action. **Payload offset itself is disputed** — this session's own MCU-side disassembly reads the value from frame `+3` (`payload[1]`), but `custom_ui`'s `handle_audio_route()` reads `payload[0]` — not reconciled | ✅ CONFIRMED (mask/ignore/dispatch logic, `0x08008808`) / ⚠️ payload offset conflict | see the shared relay truth table below — this command and `0xA0 id=0x11` drive the exact same dispatcher |
| `0x85` | App-protocol ACK | `payload[0..2]` — 3 raw bytes stored into an internal queue slot; no further bit-level structure resolved | ✅ CONFIRMED (command byte, store, queue mechanism) / ❌ the clean-room reply content is a self-admitted "reasonable approximation," not byte-exact |
| `0x87` | Bluetooth AT-command relay | `payload[0..n]` = raw ASCII AT-command bytes, verbatim passthrough to USART3 — not a structured bitfield | ✅ CONFIRMED — handler `0x080087A1`, real USART3 base `0x40004800` (PB10/PB11) | baud rate (9600) is an unconfirmed best-guess. Real bug found: the `AT+PIN=0000` template's digit-substitution bytes have no writer anywhere in the firmware — the PIN is very likely always sent malformed |
| `0x88` | TEA-cipher anti-clone challenge | `payload[0..7]` = 8-byte TEA-encrypted block (one 64-bit cipher round-trip), no sub-fields | ✅ CONFIRMED, fully — real cipher `0x080050A0`, genuine 32-round TEA, key recovered from `.data` `0x0800BCAC`, shared across the whole DCn32 product line, and the decrypted reply is read by nothing on the SoC side (inert as shipped) | see `0x60` below — the clean-room reply is tagged `0x88`, but the real MCU→SoC reply opcode is `0x60`, not `0x88` |
| `0xA0` | UI settings sync | `payload[0]` = settings `id` (`0x00`–`0x11`), `payload[1]` = value — see the dedicated sub-table below for what each `id`'s value byte means | ✅ CONFIRMED, see the dedicated table below | |
| ~~`0x90`~~ | ~~Diagnostic flash/SRAM readback~~ | — | ❌ **DISPROVEN** | checked against 5 real firmware images (this device + 4 cross-vendor references); appears in none. Removed from `uart_protocol.h`/`.c` entirely — this was a clean-room fabrication from an early, largely-unverified handoff doc |
| `0xE1` | Enter bootloader for update | `payload = [0x00, 0x00]` — the trigger is the command byte alone; the MCU-side effect (writing magic `0x5555AAAA` to SRAM `0x20004004`) is internal, not driven by any payload field | ✅ CONFIRMED end-to-end, both sides | **real risk, not just a documentation note**: the resident bootloader isn't in this repo, and the clean-room bootloader erases flash *before* waiting for the first byte — sending this on real hardware with no follow-up could wipe application flash with no recovery. `tools/mcu-probe --reboot-probe` is gated behind `--confirm-erase-risk` |
| `0xFF` | System state reset | `payload[0]` = sub-command id (`0x00`–`0x09` all confirmed no-ops; only `0x7F` acts) | ✅ CONFIRMED dispatch shape | real effect content behind sub-id `0x7F` is approximated, not byte-verified |

### `CMD 0x84` / `CMD 0xA0 id=0x11`'s shared relay dispatcher — real 4-state truth table

Both commands funnel into the same real dispatcher function (`0x080058A4`), confirmed by
instruction-by-instruction trace of `0x080058F8`/`0x0800591C`:

| Dispatcher state | GPIOC13 | GPIOC2 |
|---|---|---|
| `0` | LOW | LOW |
| `1` | LOW | HIGH |
| `2` | LOW | LOW *(same physical result as state 0)* |
| `3` | HIGH | LOW |

**Real, still-unresolved finding**: `CMD 0x84`'s own gate ("proceed if struct offset `0x5e`
== 0") is the *opposite* polarity of `CMD 0xA0 id=0x11`'s gate ("proceed if its own offset
`0x5e` == 1") — and the two handlers read that offset from different SRAM struct bases, so
whether these are the same flag at overlapping offsets or two genuinely independent flags is
not resolved.

### `CMD 0xA0` sub-table (settings sync, by `id`)

| id | Real value texts | Real display label (vendor code, often mismatched — see note) | Real MCU-side effect | `custom_ui` wiring |
|---|---|---|---|---|
| `0x00` | OEM Microphone / AfterMarket Microphone | "Reversing camera" *(vendor-code mismatch)* | GPIOB Pin 1, 4-way branch; value `2` sends real `AT+UPGRADE` | ✅ "OEM Microphone Relay" |
| `0x01` | AfterMarket/Factory Camera, AfterMarket/Factory 360 | — | shares `0x00`'s handler — **confirmed genuine no-op on real MCU firmware** | ✅ "OEM Factory Camera" toggle — see the `id=0x11` note below for why it's actually wired to `0x11` instead |
| `0x02`–`0x06` | shares `0x00`'s handler | — | confirmed shared/no-op | not wired |
| `0x07` | shares `0x00`'s handler | "Radar" | write-only, no consumer found | not wired |
| `0x08` | Off/On | "Trajectory" | write-only, no consumer | not wired |
| `0x09` | Off/On | "Reversing mode" *(vendor-code mismatch)* | mic/audio input mux, GPIOB Pin 6 | ✅ "OEM Factory Microphone" |
| `0x0A` | CAN Active/12V Active/P Key Active | "360 camera" | write-only, no consumer | not wired |
| `0x0B` | empty/dynamic | "Front camera" | coordinated 3-pin enable (PA15/PB8/PB9) when cleared to 0 | not wired |
| `0x0C` | Off/Radar Active/5s/10s/15s | "Front camera time" | real reader, thresholds the `0x0B` 3-pin group | not wired |
| `0x0D` | 5s/10s/15s | "Speech button" | write-only, no consumer | not wired |
| `0x0E` | Off/On | "DVR" | confirmed genuine no-op | not wired |
| `0x0F` | Off/On | "Right Camera" | confirmed no GPIO effect | not wired |
| `0x10` | Off/On/12V Active | "Left Camera" | confirmed no GPIO effect | not wired |
| `0x11` | Off/On/12V Active | "Microphone" *(vendor-code mismatch)* | **GPIOC13/PC2 relay pair — same dispatcher table as `CMD 0x84` above** | ✅ this is what `custom_ui`'s camera toggle actually sends, gated by `flag_5e` (real GPIOB Pin 2 read) |

**Real vendor-code inconsistency, disassembly-confirmed on both ends**: the display-name
function and the value-texts/wire function have genuinely drifted apart in the real vendor
code. `id=0x00`'s values are mic-related but its display name is "Reversing camera";
`id=0x11`'s display name is "Microphone" but its values and real hardware effect are the
camera relay. This isn't a project mistake — it's a real bug in the vendor's own source that
both independent disassembly passes confirm.

**`id=0x01` vs `id=0x11` for the camera toggle**: `custom_ui`'s `sync_video_relay()` sends
`id=0x11`, not the `id=0x01` its own display label ("AfterMarket/Factory Camera") would
suggest — because `id=0x01` is a confirmed real no-op on the MCU firmware (shares the same
dead handler as `0x02`-`0x06`), while `id=0x11` has the actual confirmed GPIOC13/PC2 relay
effect. This is a real, working reconciliation of two separately-confirmed facts, not itself
independently hardware-verified end-to-end.

**Also fully separate and worth remembering**: the real *stock* OEM/aftermarket camera
switch mechanism is a U-Boot env var (`fw_setenv carback_camera_mode`) plus a kernel `rn6752`
I2C sysfs write — **zero MCU involvement**. This superseded an earlier belief that `id=0x11`
was how stock itself does the toggle; `id=0x11` is real and has a real hardware effect, but
stock apparently doesn't use it for this purpose.

---

## MCU → SoC (commands the MCU sends)

This direction is **not** a single closed table the way the inbound one is — it's pieced
together from the real stock `libMcuCenter.so` disassembly (`MCUAdapter_BoxP300::
onRecvMcuProtocol`), from `custom_ui`'s own live UART captures, and from earlier,
never-cross-checked guesses in the now-archived docs. Several codes have **three or four
different claimed meanings across this project's own history** that were never reconciled
against each other until now.

| Cmd | Payload / bitfield layout | Claim A (`1.3_MCU_ADAPTERS.md`, archived) | Claim C (`custom_ui`, live capture) | Resolution |
|---|---|---|---|---|
| `0x00` | — | default/ignored (✅ confirmed, `0x37348`) | — | not disputed |
| `0x01` | `payload[0]` bit `0x02` = on/off — the ONE bit `custom_ui` actually reads; no other bits decoded | ~~key/button event, type `0x1013`~~ (static disasm, "med" confidence only) | 🟡 **Headlights/illumination status** — live-hardware confirmed | ⚠️ Claim C is what `custom_ui` runs on. Claim A was never independently verified against a live capture, struck through as an unreconciled guess |
| `0x02` | `payload[0]`(`b3`)=key code, `payload[1]`(`b4`)=1 press/0 release. Live-captured codes: `3`=Next,`4`=Prev,`8`=Answer,`9`=Hangup,`12`=Home,`13`=Knob push,`64`=Knob CCW,`65`=Knob CW | ~~"handshake/reply builder"~~ (later revised *in the same doc* to knob/key event) | 🟡 **Knob/button event** — live-capture confirmed | Claim C is live-capture confirmed and is what `custom_ui` runs on |
| `0x03` | `QBitArray` bitfield confirmed to exist; **no individual bit meaning resolved** | status bitfield, no type assigned | not handled | UNCONFIRMED |
| `0x04` | not resolved at byte level in any source — presumably a distance/level value, exact encoding never traced | ✅ **Reverse radar / parking-sensor level** → `transRadarLevel` — "high" confidence, named real function call | ❌ **treated as "Reverse gear: ENGAGED"** with no payload check at all | ⚠️ **Real, unresolved conflict — see the dedicated note below** |
| `0x05` | not resolved | status, type `0x5018` — explicitly "deliberately left open" | not handled | UNCONFIRMED |
| `0x06` | `byte[3]` via `QBitArray`; **no individual bit meaning resolved** | status bitfield, type `0x501A` — explicitly "deliberately left open" | clean-room firmware: ~~`MCU_CMD_REVERSE_GEAR`~~ | ⚠️ Real disassembly leaves this deliberately open; the clean-room firmware's own guess is unreconciled with that |
| `0x07` | not resolved | not present in this table at all | not handled | UNCONFIRMED — single source |
| `0x0A` | `byte[3]` bit `0` = direction, `byte[4..5]` = 16-bit magnitude, scaled to a signed angle. **Real bit-packing of the magnitude/scale factor still not fully cracked** despite the command's purpose being high-confidence | ✅ **Steering angle / reverse trajectory** — "high" confidence, `"recv track:"` string cited | matches Claim A (`MCU_CMD_STEERING_ANGLE`) | Claim A stands |
| `0x12` | `byte[4]` read; **no individual bit meaning resolved** | status, type `0x5026` — "deliberately left open" | clean-room: ~~`MCU_CMD_DIP_PROFILE`~~ / `custom_ui`: ❌ **"Reverse gear: DISENGAGED"** | ⚠️ **Three-way unreconciled conflict** |
| `0x20` | `X=(payload[1]<<8)|payload[0]`, `Y=(payload[3]<<8)|payload[2]`, all-zero payload = release. `payload[4]` (`b7`) values `1`/`2`: possibly touch-down vs. move, **not confirmed** | "reply builder" (static reading only) — later **✅ live-capture confirmed** via corner-touch test, exact pixel-width match | 🟡 matches the live-capture reading exactly | ✅ CONFIRMED (live capture) — the most rigorously verified single finding in the whole corpus |
| `0x21`/`0x22` | not resolved | `MsnEvent`, meaning unresolved | not handled | UNCONFIRMED, single source |
| `0x30` | `custom_ui` reads `value = payload[0] + payload[1]/100.0f` (its own battery-voltage interpretation — payload *format* is real, its *meaning* is disputed) | ✅ "arkdata/display-config file I/O" — "high" confidence, real function name | ❌ **treated as battery voltage** | ⚠️ **Real, substantive conflict** — file I/O and battery telemetry are unrelated functions |
| `0x40` | raw traffic captured, not decoded | live-captured raw traffic, not decoded | not handled | UNCONFIRMED |
| `0x60` | not resolved (real TEA-decrypted reply content, plaintext structure not documented) | not present at all | not handled | `MCU_FIRMWARE_VERIFIED_FINDINGS.md` separately confirms `0x60` as the real `CMD 0x88` TEA-reply opcode |
| `0x7F` | `payload[0..27]` = 28-byte ASCII version string | ✅ MCU version report — "high" confidence, `0x38e00` | matches Claim A's command byte, stores payload as ASCII | Command byte/framing confirmed. The SoC's own `/tmp/mcu_version` is separately confirmed hardcoded, not sourced live from this command |
| `0xE2`/`0xE4` | not resolved at byte level; confirmed only by cited strings (`"End Update Mcu!"`, `"recv update packageid:"`) | ✅ firmware-update handshake (both "high") | not implemented anywhere in this project | confirmed existence/purpose; not wired into anything |

### The reverse-gear command conflict — the one that actually matters right now

`custom_ui/src/hal/mcu_input.cpp` currently treats `CMD 0x04` as "reverse gear ENGAGED" and
`CMD 0x12` as "reverse gear DISENGAGED," with **no payload check on either** — just the
command byte's presence. Neither mapping has any disassembly support:

- `CMD 0x04` is the one "high"-confidence finding in the whole outbound table that
  **isn't** about reverse gear at all — it's real, named, disassembly-confirmed parking
  radar/distance telemetry (`transRadarLevel`). It very plausibly fires repeatedly while
  reversing (parking sensors are active), which would make it *correlate* with reverse gear
  without *meaning* reverse gear — exactly the kind of thing that looks right in casual
  testing and is wrong underneath.
- `CMD 0x12`'s meaning has **four different unreconciled guesses** across this project's own
  history (unresolved status event / "Display State Sync" / "DIP switch profile" / "reverse
  disengaged") and zero disassembly support for any of them.

This project has a real, independently-sourced, dedicated hardware signal for reverse gear
that doesn't depend on any of this guessing: `/dev/carback`, a real SoC-level GPIO IRQ driver
(`linux-arkmicro/linux/drivers/soc/arkmicro/ark-carback.c`), completely unrelated to the MCU
UART protocol. `main.cpp`'s dual-redundant detection currently checks the MCU-UART guess
*first* and only falls back to `/dev/carback` if the UART side doesn't fire — the MCU UART
signal should be demoted to a fallback (or removed) and `/dev/carback` made authoritative.
**Not yet implemented as of this doc** — recorded here as the concrete next fix.

---

## What to do when you find another gap

If you trace a new command and its meaning conflicts with a row above, **don't silently
overwrite the old claim** — add it as a new conflict row with both claims shown, exactly like
the table above. The value of this doc is that every claim carries its evidence and its
counter-claims in one place; a doc that only ever shows the most-recent guess loses that.
