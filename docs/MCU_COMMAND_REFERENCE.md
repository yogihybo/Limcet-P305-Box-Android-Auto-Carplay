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

**Bitfield/sub-code tables**: where a command's payload has more than one sub-field or bit
worth naming, the main table cell gives a one-line summary and points at a dedicated table
in [Per-command bitfield / sub-code tables](#per-command-bitfield--sub-code-tables) below —
same pattern already used for `CMD 0x84`/`CMD 0xA0 id=0x11`'s shared relay dispatcher.

---

## SoC → MCU (commands the SoC sends)

The real inbound dispatch table (`0x0800B9E4` in `can_app.bin`) has exactly **9 entries** —
this set is closed and double-checked (cross-referenced between two independent disassembly
passes, 9/9 exact address matches). No command outside this list does anything on the real
MCU firmware.

| Cmd | Meaning | Payload / bitfield layout | Status | Note |
|---|---|---|---|---|
| `0x81` | Init handshake / keepalive, resets internal state slots | `payload[0] = 0x01` fixed, no further structure | ✅ CONFIRMED | handler `0x080088B5` |
| `0x82` | App foreground/mode change | see the [dedicated table](#cmd-0x82-app-mode-change) below | ✅ CONFIRMED both directions (2026-08-31) — real send function and real MCU handler now both traced | GPIOB Pin0/Pin6 toggle still has no disassembly citation |
| `0x84` | Audio route select | see [`CMD 0x84`/`0xA0 id=0x11` table](#cmd-0x84--cmd-0xa0-id0x11-shared-relay-dispatcher) | ✅ CONFIRMED (mask/ignore/dispatch logic, `0x08008808`) / ⚠️ payload offset conflict | this command and `0xA0 id=0x11` drive the exact same dispatcher |
| `0x85` | App-protocol ACK | `payload[0..2]` — 3 raw bytes stored into an internal queue slot; no further bit-level structure resolved | ✅ CONFIRMED (command byte, store, queue mechanism) / ❌ the clean-room reply content is a self-admitted "reasonable approximation," not byte-exact |
| `0x87` | Bluetooth AT-command relay | `payload[0..n]` = raw ASCII AT-command bytes, verbatim passthrough to USART3 — not a structured bitfield | ✅ CONFIRMED — handler `0x080087A1`, real USART3 base `0x40004800` (PB10/PB11) | baud rate (9600) is an unconfirmed best-guess. Real bug found: the `AT+PIN=0000` template's digit-substitution bytes have no writer anywhere in the firmware — the PIN is very likely always sent malformed |
| `0x88` | TEA-cipher anti-clone challenge | `payload[0..7]` = 8-byte TEA-encrypted block (one 64-bit cipher round-trip), no sub-fields | ✅ CONFIRMED, fully — real cipher `0x080050A0`, genuine 32-round TEA, key recovered from `.data` `0x0800BCAC`, shared across the whole DCn32 product line, and the decrypted reply is read by nothing on the SoC side (inert as shipped) | see `0x60` below — the clean-room reply is tagged `0x88`, but the real MCU→SoC reply opcode is `0x60`, not `0x88` |
| `0xA0` | UI settings sync | `payload[0]` = settings `id` (`0x00`–`0x11`), `payload[1]` = value — see the [dedicated sub-table](#cmd-0xa0-sub-table-settings-sync-by-id) below | ✅ CONFIRMED, see the dedicated table below | |
| `0xE1` | Enter bootloader for update | `payload = [0x00, 0x00]` — the trigger is the command byte alone; the MCU-side effect (writing magic `0x5555AAAA` to SRAM `0x20004004`) is internal, not driven by any payload field | ✅ CONFIRMED end-to-end, both sides | **real risk, not just a documentation note**: the resident bootloader isn't in this repo, and the clean-room bootloader erases flash *before* waiting for the first byte — sending this on real hardware with no follow-up could wipe application flash with no recovery. `tools/mcu-probe --reboot-probe` is gated behind `--confirm-erase-risk` |
| `0xFF` | System state reset | `payload[0]` = sub-command id (`0x00`–`0x09` all confirmed no-ops; only `0x7F` acts). Separately confirmed (while tracing `CMD 0x87`'s shared SRAM struct) that this handler only *reads* struct offsets `0-2`, never writes them | ✅ CONFIRMED dispatch shape and gate, `0x080088E8` | **Partially traced, not fully**: which sub-id acts and that it reads (not writes) the shared struct is real disassembly. The actual real effect once sub-id `0x7F` fires was never deep-traced past that gate — the clean-room's "resets CAN RX ring buffer" is an explicit approximation, not verified against the real handler's own instructions |

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

### `CMD 0x82` app-mode change

**Send side, real, traced (2026-08-31)**: `MCUAdapter_BoxP300::onModeAppChanged(unsigned int appId, unsigned int mode)` (`0x00035D50` in `libMcuCenter.so`) builds and sends the frame via the same real `getPackageCheckSum()`/`ProtocolUtils::writeDatas()` pipeline this project already independently reverse-engineered. After a fixed 4-byte prefix, it branches on `mode`:

| `mode` value | Extra payload byte appended |
|---|---|
| `2`, `4`, `5`, `7`, `13` | `8` |
| `23` | `0x0A` |
| anything else | none |

This directly **refutes** the doc's earlier oversimplified framing (`payload[0]: 0x01=Media, 0x02=Navi/AA — plain enum`) — the real function branches on a much wider set of `mode` values, not a clean 2-way enum. `mcu-handshake.c`'s own hardcoded startup send uses `mode=4`, which falls in the `{2,4,5,7,13}` group.

**Not yet closed**: which byte of this multi-field frame ends up at the MCU's `payload[0]` wasn't fully traced — the function keeps appending more fields after the conditional byte before calling `writeDatas()`.

**Receive side, real, traced (2026-08-31)**: the real MCU-firmware handler (`0x08008BD4`, from the confirmed 9-entry dispatch table) reads `payload[0]` and branches:

| `payload[0]` | MCU writes to state struct (`0x20000282`) |
|---|---|
| `== 1` | `offset[0]=1`, `offset[1]=4` |
| anything else | `offset[0]=2`, `offset[1]=1` |

Both branches then call the same already-confirmed generic internal event/message-queue function (`0x08006228`, a 40-slot circular queue also used by `CMD 0x81`'s init handshake) with identical arguments (`event type=0`, data `1,0`) — so `CMD 0x82` itself never touches a GPIO register directly. Whatever real hardware effect follows depends on how that internal event or the written state bytes get consumed elsewhere. The state struct (`0x20000282`) is heavily shared — **89 separate real load sites** found across the firmware via a full literal-pool scan — too broad to fully trace in this pass. The clean-room's `handle_app_state()` GPIOB Pin0/Pin6 toggle guess remains **unconfirmed**, neither proven nor disproven by this trace.

### `CMD 0x84` / `CMD 0xA0 id=0x11`'s shared relay dispatcher — real 4-state truth table

Both commands funnel into the same real dispatcher function (`0x080058A4`), confirmed by
instruction-by-instruction trace of `0x080058F8`/`0x0800591C`:

| Dispatcher state | GPIOC13 | GPIOC2 |
|---|---|---|
| `0` | LOW | LOW |
| `1` | LOW | HIGH |
| `2` | LOW | LOW *(same physical result as state 0)* |
| `3` | HIGH | LOW |

**`CMD 0x84` value → dispatcher state** (value masked to 4 bits, `&0xF`, ignored if `≥6`):

| `CMD 0x84` value | Action |
|---|---|
| `0` | sends real `"AT+AUDROUTE=1\r\n"` + dispatcher state `0` |
| `1`, `2` | state field updated only, no relay action |
| `3` | sends real `"AT+AUDROUTE=2\r\n"` + dispatcher state `1` |
| `4`, `5` | state field updated only, no relay action |
| `≥6` | ignored entirely |

**Payload offset itself is disputed**: this session's own MCU-side disassembly reads the
`CMD 0x84` value from frame `+3` (`payload[1]`), but `custom_ui`'s `handle_audio_route()`
reads `payload[0]` — not reconciled.

**Real, still-unresolved finding**: `CMD 0x84`'s own gate ("proceed if struct offset `0x5e`
== 0") is the *opposite* polarity of `CMD 0xA0 id=0x11`'s gate ("proceed if its own offset
`0x5e` == 1") — and the two handlers read that offset from different SRAM struct bases, so
whether these are the same flag at overlapping offsets or two genuinely independent flags is
not resolved.

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

Bit/sub-code breakdowns are shown as indented `↳` rows directly under their command, with
the specific bit or field named in the **Bit / Field** column — no separate sub-tables to
jump to.

| Cmd | Bit / Field | Meaning | Status |
|---|---|---|---|
| `0x00` | — | Default / ignored (no-op) | ✅ CONFIRMED, `0x37348` |
| **`0x01`** | — | **Headlights / illumination status** (main use), plus 2 more real sub-fields `custom_ui` doesn't use | ✅ upgraded 🟡→✅ (2026-08-31) — see bits below |
| ↳ | `payload[0]` bit `1` | Headlights ON/OFF → real `MsnEvent(0x5004)`/`MsnEvent(0x5005)`. Live-hardware confirmed (updates in real time on the stalk); corroborated by a checksum-verified stock-app frame (`2E 01 06 13...`, bit `0x02` set). Wired end-to-end into a real feature: `mcu_input.cpp`→`night_mode_`→`nightModeClient.sendNightMode()`→AA's real `SENSOR_NIGHT_MODE` — user-confirmed on real hardware | ✅ CONFIRMED, disassembly + live hardware + working feature |
| ↳ | `payload[0]` bit `2` | Reverse-camera override → real `MsnEvent(type=0x5026, param=1)`, dispatched via `MsnApplication::dispatchMsnEvent()` | ✅ CONFIRMED (2026-08-31), `0x0003DC88`/`0x0003E844` — not used by `custom_ui` |
| ↳ | later payload byte, bit `7` | Legacy key-matrix: bit `7`=press/release, code = byte `\| 0x4000` → real `MsnEvent(type=0x1013)` | ✅ CONFIRMED (2026-08-31), `0x0003DD28` — exact payload index not independently re-derived (a separate report's "`payload[2]`" label is off by one against this doc's own `byte[2+n]` convention; treat as "a later payload byte" until re-checked). Not used by `custom_ui` |
| **`0x02`** | — | **Knob/button event** | ✅ Live-capture confirmed, what `custom_ui` runs on |
| ↳ | `b3=3`, `b4`=press/release | Next Track | ✅ confirmed (matches `kBtnNextTrack`, live-tested on real hardware — `3`/`4` confirmed NOT volume) |
| ↳ | `b3=4` | Prev Track | ✅ confirmed (matches `kBtnPrevTrack`, live-tested) |
| ↳ | `b3=8` | Answer | 🟡 live-capture |
| ↳ | `b3=9` | Hangup | 🟡 live-capture |
| ↳ | `b3=12` | Home | 🟡 live-capture |
| ↳ | `b3=13` | Knob push (press/release) | 🟡 live-capture |
| ↳ | `b3=36` | "Mode / Source" (label unconfirmed) | ⚠️ single observation, never seen elsewhere in this project |
| ↳ | `b3=64` | Knob CCW | 🟡 live-capture |
| ↳ | `b3=65` | Knob CW | 🟡 live-capture |
| ↳ | *(pattern)* | `8`/`9`, `12`/`13`, `64`/`65` each share every bit except bit `0` (upper bits = control group, bit `0` = state/direction); `3`/`4` don't fit | inferred from the value list, not disassembly-confirmed |
| **`0x03`** | — | **Vehicle status / HVAC bitfield broadcast**. Real dispatch (`0x0003BB44`→`0x0003DF98`), real `AirConditionDlg` setters confirmed. Not handled by `custom_ui`, no live capture | 🟡 — real layout spans more of the frame than 2 bytes (also pulls `payload[4]`, `payload[7]`); full map incomplete |
| ↳ | `payload[0]` bit `5` | `CirculationMode` (also gated by `payload[4]` bits `2`/`5`) | ✅ confirmed by direct trace |
| ↳ | `payload[0]` bit `6` | `ACEnable` | ✅ confirmed by direct trace |
| ↳ | `payload[0]` bit `7` | `AirConditionEnable` | ✅ confirmed by direct trace |
| ↳ | `payload[0]` bits `0`–`4` | unconfirmed door/trunk/handbrake guess (a separate report) | ❌ plausible, not traced |
| ↳ | `payload[1]` | unconfirmed fan-speed/AC/recirc/defrost guess (a separate report) | ❌ **directly contradicted** — the confirmed HVAC bits above are in `payload[0]`, not `payload[1]` |
| `0x04` | not resolved at byte level — presumably a distance/level value | ✅ **Parking radar / distance level** (`transRadarLevel`), 🟡 **empirically correlates with reverse gear engaging** | "High"-confidence disassembly, named real function call. A fresh live capture confirms it fires when reverse gear engages. See [the reverse-gear conflict](#the-reverse-gear-command-conflict--the-one-that-actually-matters-right-now) below |
| **`0x05`** | — | two competing theories, unreconciled — structurally near-identical to `0x04`'s handler | ⚠️ CONFLICT — Qt event type `0x5018` (vs. `0x04`'s `0x5019`); zero live captures either way |
| ↳ | Theory A: HVAC/climate | same `MsnEvent` dispatch mechanism as `CMD 0x03` (real `AirConditionDlg` elsewhere) — but no direct call from `0x05`'s own handler into any `AirConditionDlg` setter | ❌ unconfirmed |
| ↳ | Theory B: radar-adjacent | `payload[1]` feeds directly into the same real `transRadarLevel(unsigned char)` function `0x04` uses (`0x04`→`payload[0]`, `0x05`→`payload[1]`) — real, structural | 🟡 real function-call evidence, not conclusive (function name alone doesn't prove semantic reuse) |
| ↳ | *(context)* | `MCUAdapter_BoxP300` is confirmed the *active* adapter for this hardware — not dead code for the wrong vehicle — but zero captures of `0x05` exist; may simply never fire on real Prado hardware | — |
| **`0x06`** | — | **Vehicle dynamics/safety bitfield** — bit *positions* confirmed, bit *meanings* not. `payload[0]` via `QBitArray`, bits combined into one packed state value (not 4 independent booleans), passed as one `MsnEvent` param | 🟡 real dispatch (`0x0003BB24`→`0x0003DAD8`) + real event (`MsnEvent(0x501A)` to app `0x190`) re-verified |
| ↳ | bit `4` | Parking Brake / Handbrake (best-current guess) | ❌ unconfirmed — same weight as `custom_ui`'s own unverified `MCU_CMD_REVERSE_GEAR` guess |
| ↳ | bit `5` | Footbrake (guess) | ❌ unconfirmed |
| ↳ | bit `6` | Turn Signals / Hazard (guess) | ❌ unconfirmed |
| ↳ | bit `7` | Reverse / Transmission (guess) | ❌ unconfirmed |
| `0x07` | not resolved | UNCONFIRMED | Single, never-cross-checked source; no disassembly evidence |
| **`0x0A`** | — | **Steering angle / reverse trajectory**. `"recv track:"` string cited, matches `custom_ui`'s `MCU_CMD_STEERING_ANGLE` | ✅ "High"-confidence disassembly |
| ↳ | `byte[3]` bit `0` | Direction | ✅ confirmed |
| ↳ | `byte[4..5]` | 16-bit magnitude, scaled to a signed angle | ✅ position confirmed; **scale factor still not cracked** |
| **`0x12`** | — | 3-byte payload; real checksum-verified capture: `2E 12 03 01 04 00 E5` → `payload=[0x01,0x04,0x00]`. See [the reverse-gear conflict](#the-reverse-gear-command-conflict--the-one-that-actually-matters-right-now) below | ⚠️ CONFLICT — two real but unreconciled findings |
| ↳ | `payload[0]` = `0x01` | not resolved | UNCONFIRMED |
| ↳ | `payload[1]` = `0x04` | SoC-side handler (`0x0003D7EC`) gates on `payload[1]==0x11` exactly — only that value posts a real `MsnEvent(type=0x5026)`; any other value hits the same shared no-op epilogue as `CMD 0x30`. **This captured frame's value (`0x04`) fails the gate** — a real no-op in the vendor stack | ✅ gate confirmed (2026-08-31); the "Toyota Prado 150 CAN Matrix Mode" reading a separate report attached here is **contradicted** — names a value the code doesn't branch on |
| ↳ | `payload[2]` = `0x00` | reserved / not resolved | UNCONFIRMED |
| **`0x20`** | — | **Touch coordinate report**. Live-capture confirmed via a corner-touch test, exact pixel-width match — the most rigorously verified finding in the corpus. ~~Static disassembly only knew it as a "reply builder"~~, superseded | ✅ CONFIRMED |
| ↳ | X | `(payload[1]<<8)\|payload[0]` | ✅ confirmed, native `0`–`800` px |
| ↳ | Y | `(payload[3]<<8)\|payload[2]` | ✅ confirmed, native `0`–`480` px |
| ↳ | all-zero payload | release | ✅ confirmed |
| ↳ | `payload[4]` (`b7`) | values `1`/`2`, possibly touch-down vs. move — `custom_ui` never branches on it (only used in the all-zero release test), so this stays open even though down/move/release all work correctly on real hardware via the `touch_pressed_`+coordinate-change state machine | ❌ **not confirmed** |
| `0x21`/`0x22` | not resolved | UNCONFIRMED — hypothesis: multi-touch/gesture event dispatcher | SoC-side handler exists, posts an `MsnEvent`, meaning never resolved. **MCU side (2026-08-31)**: scanned all 5 known firmware images for every literal load of `0x21`/`0x22` — no hit resembles an outbound frame's `cmd` byte. Consistent with unused listener hooks on hardware with no multi-touch digitizer |
| `0x30` | `payload[0]` is a sub-type selector; only value `12` (`0x0C`) is handled, everything else a silent no-op | ✅ **Arkdata display-profile selector** | **Resolved (2026-08-31)**: `0x0003BB54: cmp r3,#0x30`/`beq 0x3CC7C`; handler checks `payload[0]==0x0C`, then real `QDir`/`QString`/`QFile` calls (strings confirmed: `"/msnprofile/"`, `"arkdata/"`, `"arkdata.ini"`) — Linux-side `/msnprofile/arkdata.ini`, unrelated to U-Boot's own copy. This project's "battery voltage" capture had `payload[0]=12` — exactly the trigger value, so likely coincidental. `custom_ui`'s battery-voltage feature itself unchanged |
| `0x40` | `len=1`, fires once during the startup telemetry burst, payload value never inspected | 🟡 **SoC-side no-op, confirmed** | Walked the entire real dispatch chain (`2,32,4,5,6,10,18,1,3,127,48,226,228` are the only tested values) — no case for `0x40`, falls to the same shared no-op epilogue as `0x30`/`0x12`. MCU-side purpose still unconfirmed |
| `0x60` | not resolved (TEA-decrypted reply content) | ✅ **`CMD 0x88` TEA-challenge reply opcode** | Confirmed separately in `MCU_FIRMWARE_VERIFIED_FINDINGS.md`'s `0x88` trace; not handled by `custom_ui` |
| `0x7F` | `payload[0..27]` = 28-byte ASCII version string | ✅ **MCU version report** | "High"-confidence disassembly, `0x38e00`. `/tmp/mcu_version` is separately confirmed hardcoded, not sourced live |
| `0xE2`/`0xE4` | not resolved at byte level; confirmed only by cited strings (`"End Update Mcu!"`, `"recv update packageid:"`) | ✅ **Firmware-update handshake** | Both "high"-confidence disassembly; not implemented anywhere in this project |

---

## The reverse-gear command conflict — the one that actually matters right now

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
