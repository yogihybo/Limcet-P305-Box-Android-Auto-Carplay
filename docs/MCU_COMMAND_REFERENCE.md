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
| `0xFF` | System state reset | `payload[0]` = sub-command id (`0x00`–`0x09` all confirmed no-ops; only `0x7F` acts). Separately confirmed (while tracing `CMD 0x87`'s shared SRAM struct) that this handler only *reads* struct offsets `0-2`, never writes them | ✅ CONFIRMED dispatch shape and gate, `0x080088E8` | **Partially traced, not fully**: which sub-id acts and that it reads (not writes) the shared struct is real disassembly. The actual real effect once sub-id `0x7F` fires was never deep-traced past that gate — the clean-room's "resets CAN RX ring buffer" is an explicit approximation, not verified against the real handler's own instructions |

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
| `0x01` | AfterMarket/Factory Camera, AfterMarket/Factory 360 | — | shares the *same dead no-op handler* (`0x08008B88`) as `0x02`–`0x06`/`0x0E` — **not `0x00`'s own handler** (a separate, real one at `0x080089F8`); "shares `0x00`'s handler" in earlier project notes was loose phrasing, corrected here after re-deriving the real TBB dispatch table byte-for-byte. **Confirmed genuine no-op on real MCU firmware** | ✅ "OEM Factory Camera" toggle — see the `id=0x11` note below for why it's actually wired to `0x11` instead |
| `0x02`–`0x06` | shares `0x08008B88` (same dead handler as `0x01`/`0x0E`) | — | confirmed shared/no-op | not wired |
| `0x07` | shares `0x00`'s handler | "Radar" | write-only, no consumer — **exhaustively re-confirmed**: a full raw-byte scan of every possible Thumb2 `ldrb.w [Rn,#0x3A]` encoding in the firmware found zero reads of this struct offset anywhere outside the handler's own two writes | not wired |
| `0x08` | Off/On | "Trajectory" | write-only, no consumer — same exhaustive scan against this id's real target offset (`0x39`) also found zero reads anywhere in the firmware. ~~A separate claim framed this as "enables/disables calculation of steering track curve"~~ — that's `id=0x07`'s own historical characterization misapplied here; `id=0x08` writes a *different* offset (`0x39`, not `0x3A`) and has no confirmed consumer of any kind | not wired |
| `0x09` | Off/On | "Reversing mode" *(vendor-code mismatch)* | mic/audio input mux, GPIOB Pin 6. **Cross-firmware confirmed (2026-08-31)**: byte-for-byte identical handler machine code in all 5 known firmware images (this project's `hardware/MCU/can_app.bin` plus all 3 archived `DCn32-VOLVO` dumps and the differently-sized `DCn32-ACURA` dump) — same struct offset `0x38`, same value(1/2)/else-0 branching, confirmed via direct disassembly comparison, not just table-address matching. This is genuinely shared, portable vendor logic, not an artifact of this one firmware build | ✅ "OEM Factory Microphone" |
| `0x0A` | CAN Active/12V Active/P Key Active | "360 camera" | write-only, no consumer | not wired |
| `0x0B` | empty/dynamic | "Front camera" | coordinated 3-pin enable (PA15/PB8/PB9) when cleared to 0 | not wired |
| `0x0C` | Off/Radar Active/5s/10s/15s | "Front camera time" | real reader, thresholds the `0x0B` 3-pin group | not wired |
| `0x0D` | 5s/10s/15s | "Speech button" | write-only, no consumer | not wired |
| `0x0E` | Off/On | "DVR" | confirmed genuine no-op | not wired |
| `0x0F` | Off/On | "Right Camera" | **CORRECTED (2026-08-31)** — earlier "confirmed no GPIO effect" was wrong. Struct offset `0x43` (this id's real target) IS read, at `0x08005D30` in `hardware/MCU/can_app.bin` — gated by a second flag (a different struct's offset `9`), and when both are true it drives the exact same GPIOA Pin 15 / GPIOB Pin 8 / GPIOB Pin 9 relay trio as `id=0x0B`'s "Front Camera Enable" (confirmed by resolving the GPIO port-base literals: `0x40010800`=GPIOA, `0x40010C00`=GPIOB, masks `0x8000`/`0x200`/`0x100`). Byte-identical consumer code confirmed present in the `DCn32-ACURA` dump too (not ACURA-exclusive, as a separate claim suggested — it's in every checked firmware including this project's own) | not wired |
| `0x10` | Off/On/12V Active | "Left Camera" | **CORRECTED (2026-08-31)**, same finding as `0x0F` immediately above: struct offset `0x44` is read at `0x08005D80` (adjacent code, gated by a different struct's offset `0xA`), driving the identical PA15/PB8/PB9 trio via the same three helper calls | not wired |
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
never-cross-checked guesses in the now-archived docs. Several codes had **three or four
different claimed meanings across this project's own history** that were never reconciled
against each other until now — each row below states the single best-supported meaning,
with any rejected/unconfirmed alternative struck through in Resolution rather than shown as
its own competing claim.

| Cmd | Payload / bitfield layout | Meaning | Resolution |
|---|---|---|---|
| `0x00` | — | Default / ignored (no-op) | ✅ CONFIRMED, `0x37348` |
| `0x01` | `payload[0]` bit `0x02` = on/off — the ONE bit `custom_ui` actually reads; no other bits decoded | ✅ **Headlights / illumination status** | Live-hardware confirmed via direct behavioral test: **updates in real time when headlights are toggled on the stalk** — a much stronger form of evidence than a byte-parse guess. Independently corroborated by a real captured frame from the *stock* MSN app's own log, `2E 01 06 13 00 00 00 00 00 E5` — `len=6`, `payload[0]=0x13` (`0b00010011`, bit `0x02` set) — matches "Lights ON" exactly (checksum-verified real capture). **Upgraded from 🟡 to ✅ (2026-08-31)**: this bit is now wired end-to-end into a real, working feature — `mcu_input.cpp` feeds it into `night_mode_`, `main.cpp`'s main loop calls `nightModeClient.sendNightMode()` on every change, which drives Android Auto's real `SENSOR_NIGHT_MODE` channel — and the user has confirmed on real hardware that toggling headlights now genuinely triggers AA's night mode. A reading that drives a live, user-verified downstream feature is as confirmed as this project's evidence gets. |
| `0x02` | `payload[0]`(`b3`)=key code, `payload[1]`(`b4`)=1 press/0 release. **Resolved**: a fuller checksum-verified capture shows real press+release pairs for every code (e.g. `2E 02 02 04 01 F6` then `2E 02 02 04 00 F7`) — the earlier-observed "some codes only show `b4=0`" was an artifact of an abbreviated example listing, not a real per-key difference; `b4` is uniformly press(`1`)/release(`0`). Live-captured codes: `3`=Next Track,`4`=Prev Track,`8`=Answer,`9`=Hangup,`12`=Home,`13`=Knob push,`64`=Knob CCW,`65`=Knob CW. **Real, likely non-coincidental sub-bit pattern in `b3`** (inferred from the value list, not itself disassembly-confirmed): `8`/`9`, `12`/`13`, and `64`/`65` each share every bit except bit `0` — consistent with upper bits = control group, bit `0` = state/direction. `3`/`4` don't fit that pattern | ✅ **Knob/button event** | Live-capture confirmed, what `custom_ui` runs on and matches its `kBtnNextTrack=3`/`kBtnPrevTrack=4` constants. Confirmed via live testing on real hardware: `3`/`4` are track-skip buttons, not volume. No code change needed |
| `0x03` | `payload[0]`/`payload[1]` are each unpacked into an 8-bit `QBitArray` (generic bit-unpack loop). **Confirmed by direct trace (2026-08-31)**: `payload[0]` bit `7`→`AirConditionEnable`, bit `6`→`ACEnable`, bit `5`→`CirculationMode` (also gated by `payload[4]` bits `2`/`5`) — all real, verified against the actual disassembly, not inferred. **To be confirmed**: a separate report proposed a clean split (byte 0 = door/trunk/handbrake bits, byte 1 = fan speed/AC/recirc/defrost bits) — the byte-0-bits-6/7="Handbrake" and byte-1="all HVAC" parts of that split are **directly contradicted** by the trace above (HVAC bits are in byte 0, not byte 1), so that specific table is not adopted. The door-bits-0–4-of-byte-0 portion is unverified either way — plausible given the confirmed HVAC-adjacent bits 5-7 leave room for door/status bits below them, but not itself traced. Full per-bit map needs the rest of the function traced (it continues into `setWindDirectEx`/`setAirVolume`/`setTemperature`/`setDefrostingMode`, pulling from `payload[1]`, `payload[4]`, and `payload[7]` too — the real layout spans more of the frame than a clean 2-byte model) | 🟡 **Vehicle status / HVAC bitfield broadcast** | Real dispatch (`0x0003BB44`→`0x0003DF98`) and real `AirConditionDlg` setter calls (`setAirConditionEnable`/`setACEnable`/`setCirculationMode`/`setWindDirectEx`/`setAirVolume`/`setTemperature`/`setDefrostingMode`, all confirmed exported symbols) independently verified in `libMcuCenter.so`. Not handled by `custom_ui`, no live capture exists to cross-check against real vehicle state |
| `0x04` | not resolved at byte level — presumably a distance/level value, exact encoding never traced. `len=6` in a fresh live capture | ✅ **Parking radar / distance level** (`transRadarLevel`), 🟡 **empirically correlates with reverse gear engaging** | "High"-confidence disassembly, named real function call, for the radar meaning. A fresh live capture confirms this command really does fire when reverse gear engages — real, reproducible correlation, not just a theory. Doesn't settle which is the "true" meaning: parking sensors very plausibly auto-activate exactly when reversing, so a radar-telemetry command would correlate with reverse gear for a real physical reason, not by coincidence. `custom_ui`'s "Reverse gear: ENGAGED" reading has no disassembly support but is now empirically well-supported as a *practical* trigger, if not necessarily the byte's true meaning. See the dedicated reverse-gear note below |
| `0x05` | not resolved | UNCONFIRMED | Real disassembly explicitly leaves this deliberately open (Qt event type `0x5018`); not handled by `custom_ui` |
| `0x06` | `byte[3]` via `QBitArray`; **no individual bit meaning resolved** | UNCONFIRMED | Real disassembly leaves this deliberately open (Qt event type `0x501A`). ~~Clean-room firmware guessed `MCU_CMD_REVERSE_GEAR`~~ — never verified, unreconciled with the disassembly's own "left open" finding |
| `0x07` | not resolved | UNCONFIRMED | Single, never-cross-checked source; no disassembly evidence |
| `0x0A` | `byte[3]` bit `0` = direction, `byte[4..5]` = 16-bit magnitude, scaled to a signed angle. **Real bit-packing of the magnitude/scale factor still not fully cracked** despite the command's purpose being high-confidence | ✅ **Steering angle / reverse trajectory** | "High"-confidence disassembly, `"recv track:"` string cited, matches `custom_ui`'s `MCU_CMD_STEERING_ANGLE` |
| `0x12` | `byte[4]` (`payload[1]`) read; **no individual bit meaning resolved**. `len=3`, real checksum-verified payload captured: `2E 12 03 01 04 00 E5` → `payload = [0x01, 0x04, 0x00]`. **New (2026-08-31), independently verified against `libMcuCenter.so`**: the SoC-side handler (`0x0003D7EC`) reads `payload[1]` and gates on `payload[1] == 0x11` (17 decimal) exactly — only that value proceeds to post a real `MsnEvent(type=0x5026)` to app id `0x191` via genuine `MsnEvent`/`MsnApplication::dispatchMsnEvent()` calls; any other `payload[1]` hits the same shared no-op-return epilogue confirmed for `CMD 0x30`. **This project's own captured frame has `payload[1]=0x04`, which fails that gate** — so the real vendor stack would have treated this specific capture as a no-op, contradicting the "`payload[1]=0x04` → Toyota Prado 150 CAN Matrix Mode" reading a separate report attached to it (that reading isn't just unconfirmed, it names a value the code doesn't even branch on) | ⚠️ **CONFLICT — two real but unreconciled findings** | Real disassembly now shows a *specific* gate (`payload[1]==0x11`) rather than leaving the byte fully open, but no capture of a `payload[1]=0x11` frame exists to know what actually happens when it fires. Two separate live-capture sessions give this command two different real jobs regardless: (1) this session's fresh init-handshake capture shows `CMD 0x12` firing **once, as part of the startup telemetry burst** immediately after the `0x81/0x82/0x84/0x85` init sequence — the `payload[1]=0x04` "Vehicle Profile / DIP Switches" reading tied to this capture is now known to be a value the SoC-side handler doesn't even act on; (2) an earlier live test in this same conversation found `CMD 0x12` correlating with reverse gear *disengaging*. Both are real captures, not reconciled — possibilities include this being a general status-broadcast command that fires on multiple real triggers (startup AND certain vehicle-state changes), or the reverse-gear correlation being coincidental timing in that one test. `custom_ui`'s "Reverse gear: DISENGAGED" reading has real empirical support but is not the only behavior this command has been observed to have. See the dedicated reverse-gear note below |
| `0x20` | `X=(payload[1]<<8)\|payload[0]`, `Y=(payload[3]<<8)\|payload[2]`, all-zero payload = release. `payload[4]` (`b7`) values `1`/`2`: possibly touch-down vs. move, **not confirmed** — `custom_ui`'s own code never actually branches on `b7`'s value (only checks it's part of the all-zero release test), so this byte's specific meaning stays open at the wire level even though touch down/move/release all work correctly on real hardware through the existing `touch_pressed_`+coordinate-change state machine, independent of `b7` | ✅ **Touch coordinate report** | Live-capture confirmed via a corner-touch test, exact pixel-width match — the most rigorously verified single finding in the whole corpus. ~~Static disassembly only knew it as a "reply builder"~~, superseded |
| `0x21`/`0x22` | not resolved | UNCONFIRMED — hypothesis: multi-touch/gesture event dispatcher | SoC-side handler exists in `libMcuCenter.so` (`MCUAdapter_BoxP300::onRecvMcuProtocol`), posts an `MsnEvent`, but the meaning was never resolved — single, never-cross-checked source. **MCU side (2026-08-31)**: scanned all 5 known firmware images (this project's own `can_app.bin` plus 3 `DCn32-VOLVO` + `DCn32-ACURA` dumps) for every literal load of `0x21`/`0x22` — the few hits found don't resemble an outbound frame's `cmd` byte (no `0x2E` sync-byte write or checksum-call pattern nearby, unlike every confirmed real outbound command). Consistent with these being unused SoC-side listener hooks the physical Prado MCU (single-touch 4-wire resistive digitizer, no multi-touch hardware) never actually triggers — not itself exhaustive proof, but well-supported |
| `0x30` | `payload[0]` is read as a sub-type selector; only value `12` (`0x0C`) is handled — everything else is a silent no-op. `custom_ui`'s own `payload[0] + payload[1]/100.0f` decode is a plausible-looking but unconfirmed reuse of the same bytes | ✅ **Arkdata display-profile selector** (`MCUAdapter_BoxP300::onRecvMcuProtocol`, `libMcuCenter.so`) | **Resolved (2026-08-31), independently re-verified by direct disassembly of the real `.so`** (not just trusting a report): `0x0003BB54: cmp r3,#0x30` / `beq 0x3CC7C` inside the confirmed-active `MCUAdapter_BoxP300` adapter; the `0x3CC7C` handler reads `payload[0]`, checks `== 0x0C` (12 decimal), and — **only on that exact value** — proceeds through real `QDir`/`QString`/`QFile` calls; any other `payload[0]` hits the function's plain return-with-no-op epilogue. Real strings confirmed present in the binary: `"/msnprofile/"`, `"arkdata/"`, `"arkdata.ini"`, `"Recv change arkdata name:"`, plus real exported symbols `GetArkdataChangeName()`/`ChangeArkdata(QString)`. This is the **Linux-side** `/msnprofile/arkdata.ini` (rewritten live by `libMcuCenter.so`'s MCU-adapter handlers per `docs/1.10_SETTINGS_REFERENCE.md`) — unrelated to U-Boot's own separate `sd_bootable/arkdata.ini` copy, which Linux never touches. **The decisive point**: this project's own live capture that motivated the "battery voltage" reading had `payload[0]=12` — exactly the one value that triggers this real code path. The "12.35 V"-looking capture is very likely this same trigger value, not independent evidence of a voltage reading; `custom_ui`'s battery-voltage feature is not itself changed by this doc update, just no longer the better-evidenced interpretation of the wire bytes |
| `0x40` | raw traffic captured (`len=1`, fires once during the startup telemetry burst), not decoded — payload value itself never inspected | 🟡 **SoC-side no-op, confirmed** | **Confirmed (2026-08-31)** by walking the real `libMcuCenter.so` dispatch chain byte-for-byte: `MCUAdapter_BoxP300::onRecvMcuProtocol`'s full `cmp r3,#N`/`beq` sequence tests exactly `2,32(0x20),4,5,6,10(0x0A),18(0x12),1,3,127(0x7F),48(0x30),226(0xE2),228(0xE4)` — no case for `0x40` anywhere in it, so it falls straight to the same shared no-op-return epilogue (`0x3BD64`) already confirmed for `CMD 0x30`/`0x12`'s failed-gate paths. The MCU-side purpose (heartbeat/power-state beacon vs. something else) is still unconfirmed — only the SoC-side "ignored, no error" half is now solid |
| `0x60` | not resolved (real TEA-decrypted reply content, plaintext structure not documented) | ✅ **`CMD 0x88` TEA-challenge reply opcode** | Confirmed separately in `MCU_FIRMWARE_VERIFIED_FINDINGS.md`'s `0x88` trace; not handled by `custom_ui` |
| `0x7F` | `payload[0..27]` = 28-byte ASCII version string | ✅ **MCU version report** | "High"-confidence disassembly, `0x38e00`, matches what `custom_ui` runs on. The SoC's own `/tmp/mcu_version` file is separately confirmed hardcoded, not sourced live from this command — any UI display reading that file isn't reading this command's live payload |
| `0xE2`/`0xE4` | not resolved at byte level; confirmed only by cited strings (`"End Update Mcu!"`, `"recv update packageid:"`) | ✅ **Firmware-update handshake** | Both "high"-confidence disassembly; not implemented anywhere in this project |

### The reverse-gear command conflict — the one that actually matters right now

`custom_ui/src/hal/mcu_input.cpp` currently treats `CMD 0x04` as "reverse gear ENGAGED" and
`CMD 0x12` as "reverse gear DISENGAGED," with **no payload check on either** — just the
command byte's presence. A fresh live capture (2026-08-31) confirms both really do fire at
the right moment (`0x04` on engage, `0x12` on disengage) — this is real, reproducible,
empirical support for using them as *practical triggers*. It does not, however, settle
either byte's *true* meaning:

- `CMD 0x04` is the one "high"-confidence disassembly finding in the whole outbound table
  that **isn't** about reverse gear at all — it's real, named parking radar/distance
  telemetry (`transRadarLevel`). Parking sensors very plausibly auto-activate exactly when
  reversing, which would make this command correlate with reverse gear for a real physical
  reason, not by coincidence and not because the byte itself encodes gear state. The new
  capture confirms the *correlation*, not which explanation is right.
- `CMD 0x12`'s meaning has **three different unreconciled guesses** across this project's
  own history (unresolved status event / "DIP switch profile" / "reverse disengaged") and
  zero disassembly support for any of them. It now also has a real, checksum-verified
  capture showing it firing **once at app startup**, as part of the same telemetry burst
  the init handshake (`0x81/0x82/0x84/0x85`) triggers — unrelated to reverse gear. This
  makes it a weaker signal than `0x04`, not a stronger one: the same command byte has now
  been directly observed doing two different real things, so its mere presence isn't
  reliably specific to a reverse-gear disengage event even where the correlation holds.

`CMD 0x04`'s engage correlation is real and reproducible, but `CMD 0x12` now has a real,
demonstrated reason to fire for something else entirely — the practical case for keeping
these as-is is weaker for `0x12` than it looked before this capture, even though `0x04`'s
case is genuinely stronger. Neither byte's meaning is disassembly-confirmed either way, and
`CMD 0x04` specifically has a real, named, competing meaning.
This project has a real, independently-sourced, dedicated hardware signal for reverse gear
that doesn't depend on any of this UART guessing either way: `/dev/carback`, a real
SoC-level GPIO IRQ driver (`linux-arkmicro/linux/drivers/soc/arkmicro/ark-carback.c`),
completely unrelated to the MCU UART protocol. `main.cpp`'s dual-redundant detection
currently checks the MCU-UART signal *first* and only falls back to `/dev/carback` if the
UART side doesn't fire — worth reconsidering that priority order, or at minimum keeping
`/dev/carback` as the tie-breaker if the two ever disagree. **Not yet implemented as of this
doc** — recorded here as the concrete next decision, not a settled fix.

---

## What to do when you find another gap

If you trace a new command and its meaning conflicts with a row above, **don't silently
overwrite the old claim** — add it as a new conflict row with both claims shown, exactly like
the table above. The value of this doc is that every claim carries its evidence and its
counter-claims in one place; a doc that only ever shows the most-recent guess loses that.
