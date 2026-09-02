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

| Cmd | Bit / Field | Meaning | Status | Note |
|---|---|---|---|---|
| `0x81` | — | Init handshake / keepalive, resets internal state slots. `payload[0] = 0x01` fixed, no further structure | ✅ CONFIRMED | handler `0x080088B5` |
| **`0x82`** | — | **App foreground/mode change**. Real `MCUAdapter_BoxP300::onModeAppChanged(appId, mode)` (`0x00035D50`, `libMcuCenter.so`) builds the frame via the same `getPackageCheckSum()`/`writeDatas()` pipeline already reverse-engineered elsewhere | ✅ CONFIRMED both directions (2026-08-31) — real send function and real MCU handler now both traced | refutes the doc's earlier oversimplified "`0x01`=Media/`0x02`=Navi/AA" 2-way enum guess |
| ↳ | `mode` (send arg) `2`,`4`,`5`,`7`,`13` | Extra payload byte `8` appended | ✅ confirmed — `mcu-handshake.c`'s hardcoded startup send uses `mode=4`, in this group |
| ↳ | `mode` (send arg) `23` | Extra payload byte `0x0A` appended | ✅ confirmed |
| ↳ | `mode` (send arg), anything else | no extra byte appended | ✅ confirmed |
| ↳ | *(gap)* | which byte of the full frame lands at the MCU's `payload[0]` wasn't fully traced — the function appends more fields after this conditional byte before sending | not yet closed |
| ↳ | MCU `payload[0] == 1` | MCU writes state struct `0x20000282`: `offset[0]=1, offset[1]=4`; calls the shared internal event-queue function `0x08006228` | ✅ confirmed, `0x08008BD4` |
| ↳ | MCU `payload[0]`, anything else | MCU writes `offset[0]=2, offset[1]=1`; same event-queue call | ✅ confirmed, `0x08008BD4` |
| ↳ | *(hypothesis, not confirmed)* | possibly the **input-focus switcher between factory/OEM mode and app mode** — which subsystem the MCU routes knob ticks (`CMD 0x02`) toward. Plausible given the real 2-way state change, but no consumer of that state was found to actually branch input routing on it (the struct has 89 real load sites, too broad to fully trace). A separate report asserted this as already-proven ("routes rotary knob event target and CAN arbitration priority") — checked and rejected, that fabricates a conclusion the trace doesn't support | ❌ unconfirmed, GPIOB Pin0/Pin6 toggle also still has no disassembly citation |
| **`0x84`** | — | **Audio route select**. Value masked to 4 bits (`&0xF`), ignored if `≥6`. Same dispatcher as `CMD 0xA0 id=0x11` — see the [4-state GPIO table](#cmd-0x84--cmd-0xa0-id0x11-shared-relay-dispatcher) below | ✅ CONFIRMED (mask/ignore/dispatch logic, `0x08008808`) / ⚠️ payload offset conflict | this command and `0xA0 id=0x11` drive the exact same GPIOC13/PC2 dispatcher |
| ↳ | value `0` | sends real `"AT+AUDROUTE=1\r\n"` + dispatcher state `0` | ✅ confirmed |
| ↳ | value `1`, `2` | state field updated only, no relay action | ✅ confirmed |
| ↳ | value `3` | sends real `"AT+AUDROUTE=2\r\n"` + dispatcher state `1` | ✅ confirmed |
| ↳ | value `4`, `5` | state field updated only, no relay action | ✅ confirmed |
| ↳ | value `≥6` | ignored entirely | ✅ confirmed |
| ↳ | *(offset conflict)* | this session's own MCU-side disassembly reads the value from frame `+3` (`payload[1]`), but `custom_ui`'s `handle_audio_route()` reads `payload[0]` | ⚠️ not reconciled |
| ↳ | *(polarity conflict)* | `CMD 0x84`'s own gate ("proceed if struct offset `0x5e`==0") is the *opposite* polarity of `CMD 0xA0 id=0x11`'s gate ("proceed if its own offset `0x5e`==1") — read from different SRAM struct bases, so whether these are the same flag or two independent ones is unresolved | ⚠️ not reconciled |
| `0x85` | — | App-protocol ACK. `payload[0..2]` — 3 raw bytes stored into an internal queue slot; no further bit-level structure resolved | ✅ CONFIRMED (command byte, store, queue mechanism) / ❌ the clean-room reply content is a self-admitted "reasonable approximation," not byte-exact |
| `0x87` | — | Bluetooth AT-command relay. `payload[0..n]` = raw ASCII AT-command bytes, verbatim passthrough to USART3 — not a structured bitfield | ✅ CONFIRMED — handler `0x080087A1`, real USART3 base `0x40004800` (PB10/PB11) | baud rate (9600) is an unconfirmed best-guess. Real bug found: the `AT+PIN=0000` template's digit-substitution bytes have no writer anywhere in the firmware — the PIN is very likely always sent malformed |
| `0x88` | — | TEA-cipher anti-clone challenge. `payload[0..7]` = 8-byte TEA-encrypted block (one 64-bit cipher round-trip), no sub-fields | ✅ CONFIRMED, fully — real cipher `0x080050A0`, genuine 32-round TEA, key recovered from `.data` `0x0800BCAC`, shared across the whole DCn32 product line, and the decrypted reply is read by nothing on the SoC side (inert as shipped) | see `0x60` below — the clean-room reply is tagged `0x88`, but the real MCU→SoC reply opcode is `0x60`, not `0x88` |
| `0xA0` | — | UI settings sync. `payload[0]` = settings `id` (`0x00`–`0x11`), `payload[1]` = value — see the [dedicated sub-table](#cmd-0xa0-sub-table-settings-sync-by-id) below | ✅ CONFIRMED, see the dedicated table below | |
| `0xE1` | — | Enter bootloader for update. `payload = [0x00, 0x00]` — the trigger is the command byte alone; the MCU-side effect (writing magic `0x5555AAAA` to SRAM `0x20004004`) is internal, not driven by any payload field | ✅ CONFIRMED end-to-end, both sides | **real risk, not just a documentation note**: the resident bootloader isn't in this repo, and the clean-room bootloader erases flash *before* waiting for the first byte — sending this on real hardware with no follow-up could wipe application flash with no recovery. `tools/mcu-probe --reboot-probe` is gated behind `--confirm-erase-risk` |
| `0xFF` | — | System state reset. `payload[0]` = sub-command id (`0x00`–`0x09` all confirmed no-ops; only `0x7F` acts). Separately confirmed (while tracing `CMD 0x87`'s shared SRAM struct) that this handler only *reads* struct offsets `0-2`, never writes them | ✅ CONFIRMED dispatch shape and gate, `0x080088E8` | **Partially traced, not fully**: which sub-id acts and that it reads (not writes) the shared struct is real disassembly. The actual real effect once sub-id `0x7F` fires was never deep-traced past that gate — the clean-room's "resets CAN RX ring buffer" is an explicit approximation, not verified against the real handler's own instructions |

### `CMD 0xA0` sub-table (settings sync, by `id`)

| id | Real value texts | Real display label (vendor code, often mismatched — see note) | Real MCU-side effect | `custom_ui` wiring |
|---|---|---|---|---|
| `0x00` | **CORRECTED (2026-09-02)**: `AfterMarket Camera` / `Factory Camera` / `AfterMarket 360` / `Factory 360` — the earlier "OEM Microphone / AfterMarket Microphone" claim had `id=0x00`/`id=0x01`'s real value-texts swapped; re-derived directly from `getSetItemValueTexts(0)` (`0x00032460`), byte-for-byte, not guessed | not independently re-checked this pass — see the `id=0x01`/vendor-inconsistency notes below for why the old "Reversing camera" framing no longer needs a mismatch theory | GPIOB Pin 1, 4-way branch; value `2` sends real `AT+UPGRADE`. **HARDWARE-CONFIRMED (2026-09-02)**: real, methodical test (toggled repeatedly, tested with real reverse gear each time, `id=0x11` held fixed to rule out interaction) — `id=0x00` alone reliably controls the OEM camera relay | ✅ now sent automatically by `sync_video_relay()` alongside `id=0x11`/`CMD 0x84` (`custom_ui` commit `11128ca`) — the old standalone "Microphone Source" toggle was removed, its real function folded into "OEM Factory Camera" |
| `0x01` | **CORRECTED (2026-09-02)**: empty list — `getSetItemValueTexts(1)` returns no value-texts at all, the same generic no-values fallback shared with `id` `0x02`–`0x06`. The earlier "AfterMarket/Factory Camera" claim for this id was `id=0x00`'s real value-texts misattributed here | — | shares the *same dead no-op handler* (`0x08008B88`) as `0x02`–`0x06`/`0x0E` — **not `0x00`'s own handler** (a separate, real one at `0x080089F8`); "shares `0x00`'s handler" in earlier project notes was loose phrasing, corrected here after re-deriving the real TBB dispatch table byte-for-byte. **Confirmed genuine no-op on real MCU firmware** | not wired — never had a real display label to be wired to in the first place, see the correction above |
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
| `0x11` | `0`=SoC/LVGL, `1`=OEM Camera | **Video Source / Camera Relay Select** | GPIOC13/PC2 relay pair, same dispatcher table as `CMD 0x84` above, that switches what the LCD panel is actually fed: the SoC's own LCDC output (running `custom_ui`'s LVGL) vs. the OEM factory camera's direct analog feed. **Two independent trigger paths into the same dispatcher, both confirmed by direct disassembly (2026-08-31, `0x08008B5E`)**: (1) this handler *always* stores the sent value to struct offset `0x45`, then **immediately** fires the dispatcher too — but *only if `flag_5e` already reads `1` at that exact moment* (`ldrb [+0x5e]; cmp #1; bne <skip>`); (2) independently, the `flag_5e` edge-poller (below) fires the same dispatcher later, using whatever value is *currently stored*, whenever its own hardware edge occurs. So under normal driving (`flag_5e==0`), sending `id=0x11` only arms the value — but the "send alone never switches anything" framing from the previous version of this row was imprecise; it *can* switch immediately if the edge condition already holds | ✅ this is what `custom_ui`'s camera toggle actually sends |
| ↳ | — | — | **`flag_5e` gate, fully traced — arm-then-trigger, not send-triggered**: a real polling function reads 5 GPIO input pins (change-detected); one of them, `GPIOB Pin 2`, is separately tracked (inverted) as `flag_5e`. **Only when `GPIOB Pin 2` transitions LOW** does `flag_5e` become `1`, and *at that exact moment* the firmware branches on whatever `id=0x11` value was last sent to pick the relay state — so `id=0x11`'s send just arms the preference; the actual switch happens later, on that GPIO edge. On the *disengage* direction (`GPIOB Pin 2` back HIGH), the poller unconditionally forces dispatcher state `0` (back to LVGL) regardless of what's stored — this path needs no software involvement at all | ✅ fully traced (`0x080084A4`/`0x08005582`/`0x080058A4`) — what physically drives `GPIOB Pin 2` (a strap, a connector signal, a camera-power-detect line, possibly the reverse-gear signal itself) is **not independently confirmed**; real next step is checking the schematic or probing it directly |
| ↳ | — | — | **A separate report proposed** `GPIOB Pin 2` is literally the vehicle's reverse-gear wire / camera power line, framing the whole mechanism as "the firmware handles reverse-camera switching entirely autonomously once armed, no `custom_ui` involvement needed." **HARDWARE-CONFIRMED (2026-09-01)**: user ran `killall custom_ui` on real hardware (leaving `custom_ui` dead, no process to send/poll anything) and reverse gear still switched to the OEM factory camera feed correctly. This directly confirms the autonomy claim — the MCU's own `GPIOB Pin 2` edge-poller drives the relay entirely independently of `custom_ui` once the preference is armed, exactly as the disassembly predicted. Still not independently confirmed by schematic/probe that `GPIOB Pin 2` specifically *is* the reverse-gear wire (vs. some other signal that happens to correlate with it), but the "no software involvement needed" half of the claim is now real, hardware-observed fact, not just a plausible story | ✅ hardware-confirmed autonomous switching; wire identity still unconfirmed |
| ↳ | — | — | **RESOLVED (2026-08-31), HARDWARE-CONFIRMED (2026-09-01):** the reverse-gear-triggered resend was removed entirely, not narrowed. The disengage `id=0x11=0` send unconditionally de-armed the factory-camera preference back to `0` on *every* disengage, regardless of the user's real setting — meaning OEM Factory Camera mode only ever worked for the first reverse-gear engagement after boot/a settings change, then silently broke on every subsequent engagement, since nothing re-armed it except that same code's own engage branch (which was itself re-sending the already-armed value every time — real, self-inflicted churn, not defense-in-depth). **User reported, from direct real-world experience, that reverse-camera switching worked better before these sends existed at all** — stronger evidence than the earlier "don't touch a hardware-confirmed fix" caution, which was itself based on a smaller, less complete picture of the mechanism. `main.cpp`'s reverse-gear handler no longer sends `id=0x11` at all; the preference is armed only at boot (`main()` ~line 528) and immediately on every settings change (`settings_screen.cpp`'s `OriginalCarCamera` toggle → `hal::send_mcu_video_relay()`) — both untouched. **Real hardware retest (2026-09-01, boot log)**: multiple full reverse-gear engage/disengage cycles all correctly hid/showed the GUI layer and re-synced the camera-type MCU setting each time, with zero regression — the simplification holds up on real hardware, not just at build time | ✅ code changed (`custom_ui` commit `a4211c2`), hardware-confirmed working |

**CORRECTED (2026-09-02) — `id=0x00` half retracted**: this section previously claimed a
vendor label/value-text mismatch for `id=0x00` ("values are mic-related but display name is
'Reversing camera'"). That was built on the same `idx=0`/`idx=1` value-texts swap corrected
above — `id=0x00`'s real value-texts are the camera strings, not mic-related at all, so
there's no mismatch left to explain there. **`id=0x11`'s half of this finding stands,
independently**: its display name is "Microphone" but its values and real hardware effect
are the camera relay — a real, separate, still-confirmed vendor inconsistency, unaffected by
this correction (it was never based on the `id=0x00`/`id=0x01` value-texts trace).

**`id=0x01` vs `id=0x11` for the camera toggle — corrected**: `custom_ui`'s
`sync_video_relay()` sends `id=0x11` (and, as of today, `id=0x00` and `CMD 0x84` too), not
`id=0x01`. The earlier reasoning here ("`id=0x01`'s own display label would suggest
sending it") no longer applies — `id=0x01` never had a real camera-sounding display label to
begin with; that was `id=0x00`'s real value-texts misattributed to it (see the correction
above). `id=0x01` is simply, confirmedly, a dead no-op on the real MCU firmware, full stop,
no "looked promising but wasn't" story needed.

**Also fully separate and worth remembering**: the real *stock* OEM/aftermarket camera
switch mechanism is a U-Boot env var (`fw_setenv carback_camera_mode`) plus a kernel `rn6752`
I2C sysfs write — **zero MCU involvement**. This superseded an earlier belief that `id=0x11`
was how stock itself does the toggle; `id=0x11` is real and has a real hardware effect, but
stock apparently doesn't use it for this purpose.

### `CMD 0x84` / `CMD 0xA0 id=0x11`'s shared relay dispatcher — real 4-state truth table

Both commands funnel into the same real dispatcher function (`0x080058A4`), confirmed by
instruction-by-instruction trace of `0x080058F8`/`0x0800591C`. Referenced from both `0x84`'s
and `0xA0 id=0x11`'s rows above/below — kept as its own table since it's genuinely shared
physical-pin state, not specific to either command alone:

| Dispatcher state | GPIOC13 | GPIOC2 |
|---|---|---|
| `0` | LOW | LOW |
| `1` | LOW | HIGH |
| `2` | LOW | LOW *(same physical result as state 0)* |
| `3` | HIGH | LOW |

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
| **`0x20`** | — | **Touch coordinate report**. Live-capture confirmed via a corner-touch test, exact pixel-width match — the most rigorously verified finding in the corpus | ✅ CONFIRMED |
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
command byte's presence. Four real candidate signals exist for reverse-gear detection; none
is both fully confirmed *and* currently used as the primary source:

**SUPERSEDED (2026-09-01) — resolved, see below.** The `CMD 0x04`(engage)/`CMD 0x12`(disengage)
split this table originally debated turned out to be the wrong model entirely: `CMD 0x12`
carries BOTH directions in its own payload, and `CMD 0x04` isn't reverse-gear-related at all.
The history is kept below for context, since it's what led to the real fix.

| Candidate | Mechanism | Evidence for | Evidence against / open gaps | Current status in `custom_ui` |
|---|---|---|---|---|
| `CMD 0x04` presence | any frame with this cmd byte, no payload check | Live capture (2026-08-31) confirmed it *correlated* with reverse-gear engage | Real, named, *competing* meaning: parking radar/distance telemetry (`transRadarLevel`), the one "high"-confidence disassembly finding in the whole outbound table. **Confirmed wrong (2026-09-01)**, see below — never actually meant reverse gear at all, just correlated because parking sensors activate around the same time | ❌ demoted to a no-op, no longer touches reverse-gear state |
| `CMD 0x12` presence, direction-blind | any frame with this cmd byte, no payload check, always treated as disengage | Live capture (2026-08-31) confirmed it fires on what looked like disengage | **Three different unreconciled guesses** across this project's own history, zero disassembly support for any. Also fires once at app startup, unrelated to reverse gear. **Real hardware evidence (2026-09-01)**: user reported a consistent ~5s lag between physically leaving reverse and `custom_ui` switching back; a same-day boot log showed the pattern `0x04`(engage)→`0x12`(assumed disengage, 5.76s later)→`0x04`(engage again, 4ms later) — a 4ms flip isn't real gear-shifting. **RESOLVED**: user confirmed the two `0x12` events captured that session were a real entering/exiting *pair*, not two disengages — `payload=[01 04 00]` was the enter, `payload=[02 01 00]` was the exit. `CMD 0x12` fires on both edges; the old "any `0x12` == disengage" mapping just happened to relabel every enter-event as a disengage too, which is what produced the apparent 4ms flip and the apparent 5s "lag" (it was really `0x04`'s spurious engage racing against `0x12`'s real, correctly-timed enter push) | ❌ superseded by the payload-direction mapping below |
| `CMD 0x12` **`payload[0]` direction** (`0x01`=entering, `0x02`=exiting) | real payload field, not just command-byte presence | **Hardware-confirmed 2026-09-01**: user directly identified which of the two captured `0x12` events was the real physical enter vs. exit, matching `payload[0]` cleanly (`0x01`→enter, `0x02`→exit) | Only one real enter/exit pair confirmed so far — wired in and **pending a second real-world retest** to fully settle the mapping before treating it as closed. **Real regression hit on first retest (2026-09-02)**: exit showed "LVGL flashes up momentarily, then blank again" while engage kept working. User confirmed the hardware relay itself is independently reliable/autonomous (switches correctly even with `custom_ui` killed), ruling out a relay explanation — pointed instead at a spurious/duplicate `payload[0]==0x01` frame arriving shortly after the real exit frame, re-triggering `hide_display()` right after `show_display()` ran. Fixed with a 300ms debounce on direction flips (`custom_ui` commit `fbdcc7b`). **Second real regression, same retest session**: LVGL only showed momentarily at fresh boot with reverse gear never engaged at all — AA's own video (fb1) became visible through the now-hidden LVGL layer (fb0) shortly after. Root cause: this doc already documents `CMD 0x12` firing once during the MCU's own startup telemetry burst, unrelated to reverse gear — harmless under the old direction-blind mapping (a same-value no-op), but a real false "ENGAGED" under the new payload-direction mapping if that startup frame happens to carry `payload[0]==0x01`, and since it's the very first commit the flip-debounce (which only guards flips *after* a prior commit) never catches it. Fixed with a fixed 5s startup grace window that ignores `CMD 0x12` direction entirely regardless of debounce state (`custom_ui` commit `4b7f230`). Both fixes build-verified and hardware-retested successfully for the specific bugs they targeted. **But see the CRITICAL subsection right below this table — a third, more fundamental issue was found on the same retest: `CMD 0x12 payload[0]==0x01` fires from headlights alone, with zero gear change** | ⚠️ **primary signal, debounced + startup-grace-windowed, but confirmed NOT exclusive to reverse gear** — `mcu_input.cpp`'s `CMD 0x12` handler branches on `payload[0]`, ignores direction for the first 5s of the MCU input thread's life, and debounces flips within 300ms of the last commit thereafter; `CMD 0x04` no longer touches reverse-gear state at all (`custom_ui` commits `72220bf`, `fbdcc7b`, `4b7f230`) — mitigation for the false-trigger issue not yet decided, see below |
| `CMD 0x01` bit `2` + `CMD 0x12`'s `payload[1]==0x11` gate | both post the exact same real Qt event, `MsnEvent(0x5026)`, to app id `0x191` | Real, disassembly-confirmed convergence between two independently-traced commands — a named, specific shared event | What actually *consumes* event `0x5026` was traced as far as `MsnCoreApp`'s own main dispatcher, which doesn't test for it — no confirmed downstream behavior. Confirmed unrelated to the reverse-gear-direction question above (neither real captured disengage payload had `payload[1]==0x11`) | ❌ not wired in — a real, separate lead, unrelated to reverse-gear detection |
| `/dev/carback` | dedicated SoC-level GPIO IRQ, real kernel driver (`linux-arkmicro/linux/drivers/soc/arkmicro/ark-carback.c`) | Completely independent of the MCU UART protocol — a real, purpose-built reverse-gear hardware signal | **Currently unavailable at runtime** on this exact build/boot (`[HAL:REVCAM] WARN: /dev/carback unavailable`, 2026-09-01 boot log) — not a live fallback option right now, see `project_carback_probe_order_bug.md` memory (IRQ deliberately disabled 2026-08-03) | ✅ present in code as `main.cpp`'s secondary/tie-breaker source, not usable on this particular boot |

### CRITICAL (2026-09-02): `CMD 0x12` confirmed unreliable on real hardware -- fires from headlights alone, no gear change

The retest called for above happened, and surfaced a real, confirmed false trigger, not
just an edge case. Real captures, `custom_ui`'s own new console-wide frame logging
(`hal/mcu_input.cpp`'s `log_frame()`, printing every frame's raw cmd+payload to the
console, not just the on-screen MCU Live Log ring buffer):

```
[299.896686] Frame cmd=0x01 payload=[13 00 00 00 00 00]   <- Headlights ON
[299.925791] Frame cmd=0x12 payload=[01 04 00]              <- 29ms later: "ENGAGED"
...
[320.092828] Frame cmd=0x01 payload=[11 00 00 00 00 00]   <- Headlights OFF
[320.120708] Frame cmd=0x12 payload=[01 04 00]              <- 28ms later: same payload again
```

**User confirmed directly: the vehicle was in Park, not running, the entire time -- the
only action taken was turning the headlights on and off.** No real gear change occurred.
This is a hard, confirmed false positive, not a theory -- `CMD 0x12 payload=[01 04 00]`,
the exact byte pattern this doc had just confirmed for genuine reverse-gear engagement,
fired from headlights alone. This got the user stuck on the reverse-camera screen once
already (LVGL hidden, no real gear event to trigger the disengage side).

**A second capture deepens the mystery rather than resolving it.** The same headlights-OFF
action produced the *opposite* payload (`[02 01 00]`, the "exit" pattern) on a different
occasion -- but that specific capture turned out to be during a genuine real reverse-gear
in/out test (user-confirmed), so it doesn't stand as a second false positive. It does show
something else worth recording: `CMD 0x01` (headlights) frames kept appearing closely
bracketing *real* `CMD 0x12` gear transitions too, in a pattern (`0x01` -> `0x04` -> `0x04`
-> `0x01` -> `0x12`) that doesn't look like a driver actually toggling headlights twice
mid-reverse-test. This suggests `CMD 0x01` and `CMD 0x12` may both be symptoms of a shared
internal MCU event/state-machine tick (consistent with the already-documented `MsnEvent
0x5026` convergence between these two commands) rather than a simple one-directional
"headlights cause false reverse triggers" rule.

**Leading candidate mechanism, not yet confirmed**: `CMD 0x01` genuinely and correctly
reports real headlight state (hardware-confirmed separately -- toggling real headlights
reliably drives AA's night mode via this exact command, a different, solid finding). The
vehicle's own headlight relay/switch (`ILL+`/switched-12V circuit, real automotive
electrical hardware entirely outside this project's own boards) is a classic real source of
switching transients. If that circuit shares any harness routing or grounding with whatever
physically drives `GPIOB Pin 2` (the still-unconfirmed real signal behind the `flag_5e`
reverse-camera-relay mechanism documented earlier in this project), a transient there could
produce a spurious edge read as a real gear-state change -- independent of anything wrong
in `custom_ui`, the UART protocol, or this head unit's own LCD backlight circuit (already
confirmed smooth PWM, not relay-based, ruling out *that* specific noise source). **Not yet
verified** -- would need checking what `GPIOB Pin 2` actually traces to on the schematic/PCB,
same as the earlier CAN-transceiver pin tracing.

**Mitigation: an open, safety-relevant decision, not yet made.** Real options on the table,
each with a real tradeoff, recorded here rather than picked unilaterally:
1. Revive `/dev/carback` (currently unavailable at runtime) as the sole/primary source --
   cleanest fix if it can be safely re-enabled, sidesteps this whole UART ambiguity with a
   dedicated hardware signal.
2. Add a suppression window around unrelated MCU traffic (headlights, buttons) -- real risk
   of suppressing a *genuine* reverse-gear trigger if a driver touches headlights/buttons
   right as they actually shift into reverse.
3. Disable the `CMD 0x12` auto-trigger entirely until a reliable signal exists -- loses
   automatic camera switching, but eliminates both the false-positive and false-negative risk.

**Current model, still standing but now known-unreliable**: `CMD 0x12`'s `payload[0]`
(`0x01`=entering, `0x02`=exiting) remains wired in and does correctly track real reverse-gear
transitions most of the time (confirmed working across multiple real engage/disengage
cycles) -- but it is now confirmed **not exclusive** to reverse gear, and can fire from at
least one unrelated real vehicle action. `CMD 0x04` still never meant reverse gear at all.

Given all four candidates, `/dev/carback` is the one with **zero open evidentiary gaps** --
worth reconsidering `main.cpp`'s current priority order (MCU-UART checked first, `/dev/carback`
only as fallback), or at minimum keeping `/dev/carback` as the tie-breaker if the two ever
disagree. **Not yet implemented as of this doc** — recorded here as the concrete next
decision, not a settled fix.

---

## What to do when you find another gap

If you trace a new command and its meaning conflicts with a row above, **don't silently
overwrite the old claim** — add it as a new conflict row with both claims shown, exactly like
the table above. The value of this doc is that every claim carries its evidence and its
counter-claims in one place; a doc that only ever shows the most-recent guess loses that.

## Real, byte-level decode of `CMD 0x04`'s payload (2026-09-02) -- and why this strengthens the case for using it as corroboration

Prompted directly by the `CMD 0x12` false-trigger investigation above: is `CMD 0x04` a
real, properly-scoped signal we could use to corroborate reverse-gear transitions, or is
it just as loosely-defined as `CMD 0x12` turned out to be? Traced
`MCUAdapter_BoxP300::onRecvMcuProtocol`'s real `CMD 0x04` handler directly (not the MCU
firmware -- this command is MCU->SoC, so the byte-unpacking logic lives on the *consumer*
side, in `libMcuCenter.so`). Real symbols intact in this specific copy
(`firmware_dumps/Prado firmware dump/mtd6_rootfs/usr/lib/libMcuCenter.so`) -- confirmed via
`arm-linux-gnueabihf-objdump -d`, not guessed; other copies of this same library across the
various `firmware_dumps/*` trees are stripped and don't carry these symbols, so use this
exact file for any follow-up tracing.

Real decoded structure, handler at `0x37790`:

```
payload[0] -> transRadarLevel(payload[0]) -> output byte 0
payload[1] -> transRadarLevel(payload[1]) -> output byte 1
payload[3] -> range-classified (0-15 -> path A; 16-45 -> level 2; beyond -> a third
              branch not yet traced) -> output byte 2
payload[2] -> transRadarLevel(payload[2]) -> output byte 3
-> packed into MsnEvent(type=0x5019, app=0x191) via setByteArrayParams + dispatchMsnEvent
```

(`payload[4]`/`payload[5]` not yet traced -- always `0x00` in every real capture so far, so
low priority.) `transRadarLevel(unsigned char)` itself is an **imported** symbol (resolved
via PLT, not defined in this library) -- almost certainly a raw-sensor-byte -> discrete
zone/level lookup, consistent with a real parking-sensor cluster (3 independently-converted
channels plus a 4th classified value, matching a typical 3-4-sensor rear array).

**Why this matters for the reverse-gear reliability problem**: this is a genuinely
well-structured, radar-specific event -- three independently-converted sensor channels
packed into their own distinct `MsnEvent` type (`0x5019`), nothing like `CMD 0x12`'s
apparent shared/multi-purpose nature (see the CRITICAL section above). And the real
capture evidence backs this up: `CMD 0x04` **never appeared at all** in the one capture
confirmed to be a false positive (vehicle parked, headlights only, no radar activity --
exactly what you'd expect if this command is genuinely tied to real parking sensors), and
it *did* appear in both real reverse-gear test captures. That makes "require `CMD 0x04`
radar telemetry to also be present before trusting a `CMD 0x12` engage transition" a real,
evidence-backed candidate mitigation for the false-trigger problem above -- not a guess.
**Not yet implemented** -- a real next decision, not a settled fix.

## CRITICAL, full trace (2026-09-02): `CMD 0x12` is a hard no-op in the real vendor app for every payload we've ever captured

Full trace of `MCUAdapter_BoxP300::onRecvMcuProtocol`'s real `CMD 0x12` handler (`0x38144`,
same `libMcuCenter.so` copy as the `CMD 0x04` trace above), prompted by the false-trigger
investigation -- what does the real stock vendor app actually do with this command?

```
r3 = payload[1]                    ; frame byte[4]
cmp r3, #0x11
bne 0x37348                        ; <- the exact address this file's own dispatch table
                                    ;    already calls "0x00 | default / ignored"
; only reached if payload[1] == 0x11:
MsnEvent(type=0x5026, app=0x191)   ; zero payload data attached (setParams(0,0)) -- a bare
                                    ; ping, not "entered/exited reverse" with any state
```

**If `payload[1] != 0x11`, this command is a hard no-op in the real stock software** --
same code path as an unrecognized command byte. Confirmed by direct comparison of branch
targets, not inference.

**Every real capture this project has ever gotten has `payload[1]` of `0x04` or `0x01`,
never `0x11`** -- both the confirmed false positive (headlights, parked, no gear change)
and the confirmed genuine reverse-gear transitions (`[01 04 00]` entering, `[02 01 00]`
exiting -- `payload[1]` is `0x04` and `0x01` respectively, neither is `0x11`). Per the real
vendor app's own dispatch logic, **every single `CMD 0x12` frame captured on this project
so far is semantically meaningless to the system this protocol actually belongs to.**

**Real reframing this forces**: `custom_ui`'s `payload[0]==0x01/0x02` "entering/exiting"
interpretation was never backed by any real vendor-software semantics -- it was built
entirely on timing correlation with real gear changes across a handful of tests, which held
up every time it was deliberately tested... and has now also been shown to fire from
headlights alone with zero gear involvement. Given the stock software treats this exact
traffic as meaningless, `CMD 0x12` with these payload values may not be a dedicated
reverse-gear signal by intended design at all -- it may be some other internal MCU
behavior/telemetry that happens to correlate with gear changes for a reason still not
understood, the same way `CMD 0x04` (parking radar) was found to correlate with engage
without meaning it directly.

**This meaningfully strengthens the case, argued earlier in this doc, for depending on
`/dev/carback` and/or `CMD 0x04` radar corroboration rather than trusting `CMD 0x12` alone**
-- not because `CMD 0x12` has been unreliable in testing (it hasn't, for genuine gear
changes), but because we now know its apparent meaning was never actually confirmed by the
one authority that would know for certain: the real vendor software that owns this
protocol, which discards every frame we've captured as a no-op.

## Consolidated command-structure understanding (2026-09-02) -- what's now real, complete, and what remains genuinely open

After the day's deep tracing (`CMD 0x03`, `CMD 0x04`, `CMD 0x06`, `CMD 0x0A`, `CMD 0x12`,
`CarSignalsWatch`, `/dev/carback`, and every producer of `MsnEvent 0x5026` across the whole
binary), this is the real, now-complete picture of `MCUAdapter_BoxP300`'s command surface --
the confirmed-active adapter for this exact product (`McuType=6`).

**Fully resolved this session, real vendor-confirmed meanings**:
- `CMD 0x03` -- pure AC/climate-control status bitfield (`AirConditionDlg::*`), unrelated to
  reverse gear.
- `CMD 0x04` -- real, well-structured rear parking-radar telemetry (3 independently-converted
  sensor channels via `transRadarLevel()`), `MsnEvent 0x5019`.
- `CMD 0x0A` -- pure steering-angle/trajectory data (`MsnEvent 0x501C`, a signed angle),
  assumes reverse mode is already active by some other mechanism -- carries no
  engage/disengage state of its own.
- `CMD 0x12` -- only meaningful with `payload[1]==0x11` (posts a bare, dataless
  `MsnEvent 0x5026`); every other payload value is a hard no-op. **`MsnEvent 0x5026` itself
  has zero confirmed consumers anywhere in `MsnCoreApp`**, confirmed by checking every one
  of its 5 producer sites across the whole binary (including two *other* vehicle-adapter
  variants in the same library, `MCUAdapter_BoxC2` and `MCUAdapter_MsnDecoder`/Holden, which
  also happen to reuse this event type for their own unrelated hardware) -- a genuine dead
  end, not an unexplored one.
- `CarSignalsWatch` -- a real, previously-undocumented SoC-GPIO watcher (GPIO 30/31,
  independent of the MCU-UART link entirely), traced through to `addAppStates`/
  `removeAppStates`'s real bit meanings: audio volume and Bluetooth-related, not reverse gear.
- `/dev/carback` -- confirmed dead by deliberate 2026-07-17 design decision already
  documented in this product's own devicetree, not a bug or a race to fix.

**Still genuinely unresolved**: the specific "backcar enable/disable" command this
project's own DTS comment names as the real mechanism has not been identified, despite
checking the entire closed 9-entry SoC->MCU dispatch table, the full ~14-entry MCU->SoC
dispatch table for `BoxP300`, the `CarSignalsWatch` GPIO side-channel, and every producer of
the one plausible event-type candidate. Two honest possibilities, not resolved either way:
(a) it's encoded in a payload value/field of one of the already-traced commands that wasn't
individually checked closely enough (most commands' full byte ranges were checked, but not
exhaustively re-verified against every possible value), or (b) the DTS comment's own
"backcar enable/disable command" description, while correctly identifying that reverse
detection is UART-based rather than SoC-GPIO-based, may itself be imprecise about which
specific command implements it.

**Practical implication for the false-trigger problem this was chasing**: no cleaner
signal than what's already been found and documented (`CMD 0x04` radar corroboration) was
uncovered by this pass. The mitigation decision recorded in the CRITICAL section above
still stands as the real open next step.

## RESOLVED (2026-09-02): stepped back from chasing a reliable reverse-gear signal -- removed the OEM camera path's dependency on one entirely

After the day's exhaustive `CMD 0x12`/`CMD 0x04`/`/dev/carback`/command-structure
investigation above, the user reframed the actual problem: rather than continuing to hunt
for a fully reliable software reverse-gear signal, is one even needed? The answer for the
OEM Factory Camera path turned out to be no -- the MCU's own `flag_5e`/`GPIOB-Pin-2` relay
switching was already hardware-confirmed fully autonomous earlier this session (OEM camera
engaged correctly with `custom_ui` completely killed, meaning zero software involvement).

**Real architectural fix, not just documentation**: removed `hal::hide_display()`/
`show_display()`/`lv_obj_invalidate()` entirely from both the engage and disengage branches
of `main.cpp`'s OEM Factory Camera handling (`custom_ui` commit `de1b75d`). Every real bug
this project hit today -- the stuck-in-reverse false trigger, the exit flash-then-blank, the
boot-time false engage -- came from reactively driving this GUI-hide/show behavior off a
live reverse-gear detection signal that turned out to be shared/unreliable (see the CRITICAL
sections above). Removing the reactive dependency removes the entire bug class at its root,
rather than continuing to patch around an inherently noisy signal.

**Deliberately scoped, not a wholesale removal**: volume cut during reverse, the AA
video auto-resume nudge on disengage, and the aftermarket camera screen navigation path
all still use the same `reverseChanged`/`CMD 0x12` detection and were left untouched --
none of them were the source of today's bugs, and a stray false trigger there is a much
lower-consequence outcome (a redundant volume dip or resume request) than a visually broken
or stuck display. If `CMD 0x04` radar corroboration or a `GPIOB Pin 2` hardware trace is
ever pursued further, it would now matter only for those remaining consumers and for
aftermarket-camera-mode users, not for the OEM path most of this session's real hardware
testing has been on.

**Not yet hardware-retested** -- the real next step is confirming reverse-camera switching
(both directions, multiple cycles, OEM Factory Camera mode) still works correctly now that
`custom_ui` does nothing reactive at all for it.

**Real hardware retest (2026-09-02): confirmed working every time.** User confirmed OEM
Factory Camera reverse-gear switching now works correctly on every cycle after the
`de1b75d` fix above -- closes out the day's entire `CMD 0x12` investigation for the OEM
path. The architectural call (stop reacting to a signal proven unreliable, rather than
continuing to try to make it reliable) was correct.

**Aftermarket camera toggle confirmed NOT working**, same test session -- consistent with
the real gap identified in the same conversation: `custom_ui` never invokes the actual
stock OEM/aftermarket camera switch mechanism (`fw_setenv carback_camera_mode` + kernel
`rn6752` I2C sysfs write, a separate real hardware decoder chip, unrelated to the MCU relay
just fixed). Real next step, not yet implemented: wire that mechanism up.

---

## Reverse camera system architecture -- full, consolidated picture (2026-09-02)

After a full day tracing this from multiple directions, this is the complete, real
architecture as currently understood -- five genuinely separate mechanisms, not one.

### 1. Two independent physical SoC<->MCU UART channels exist, not one

- **`/dev/ttyHS0`** (MMIO `0xE4F00000`, the "HS UART") -- the main channel. `[0x2E][cmd]
  [len][payload][checksum]` framing. Everything this doc otherwise covers (touch, knob,
  buttons, settings sync, `CMD 0x12`, etc.) goes over this link. This is the only channel
  `custom_ui` ever opens (`hal::McuInputHal`'s default `/dev/ttyHS0`).
- **`ttyS2`** (MMIO `0xE8000000`, U-Boot calls it "UART2") -- a real, *separate* channel,
  `[0x0d][cmd=0x24][len][payload][checksum]` framing (completely different sync byte and
  structure). **`custom_ui`/Linux never opens or touches this device at all.** Only used by
  U-Boot's own `ark_mcu_notify_backcar()` (`u-boot/board/arkmicro/ark1668_limcet_p305/
  ark1668_display_cfg.c`), ported from real stock `mtd1_uboot.bin` disassembly
  (`FUN_0006ede4`). Real, honest uncertainty carried over from that original trace: no
  direct caller of the original stock function was ever found (this project's own fork
  explicitly wired up a caller, `ark_carback_camera_check()`, that stock itself may never
  have exercised this way), and the on/off polarity is an *inferred* convention, not
  disassembly-proven. Whether the current `can_app.bin` MCU firmware's own receiver even
  understands this framing at all is **unverified** -- not confirmed either way.

### 2. A real SoC GPIO reads reverse-gear state directly -- but only during early boot

`ARK_BACKCAR_GPIO` = SoC GPIO pin 5, active-low, read directly by U-Boot
(`do_backcarcheck`/`ark_carback_camera_check`) for two real, working things: instant
boot-time camera preview (`ark_itu656_camera_bypass_enable()`) and triggering the
`ark_mcu_notify_backcar()` UART2 notification above. **This is the exact same physical pin
already documented elsewhere in this doc set as removed from Linux's `ark-carback` driver**
(`ark1668_limcet_p305.dts`, 2026-07-17) -- because once the Linux display driver
initializes, this same pin gets reclaimed for LCD `r3`. So real, working GPIO-based reverse
detection genuinely exists on this hardware, but only in the narrow early-boot window
before the LCD pinmux takes it over -- not usable by `custom_ui`/Linux at runtime, which is
exactly why `/dev/carback` is correctly absent (see the dedicated section above).

### 3. The MCU's own relay switching is autonomous and hardware-confirmed reliable

Separately from all of the above: the MCU's own `flag_5e`/`GPIOB Pin 2` mechanism
(traced in full in `docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md`) switches the OEM Factory
Camera video relay entirely on its own, in firmware, with zero SoC-side involvement --
**hardware-confirmed twice**: once by killing `custom_ui` entirely and observing OEM camera
still engage correctly, and again after removing `custom_ui`'s reactive `hide_display()`/
`show_display()` calls, where the user confirmed OEM camera now switches correctly on
*every* cycle. This is the one mechanism in this whole picture with zero open reliability
questions.

### 4. A separate real chip (RN6752) selects which physical camera SOURCE is decoded

The `RN6752` AHD camera decoder (real I2C device on this board, `dvr_rn6752` in the DTS) is
what actually selects *which physical camera feed* gets decoded when not using the OEM
passthrough -- controlled via `fw_setenv carback_camera_mode` + a kernel sysfs write, **zero
MCU involvement**. This is a completely different axis from mechanism #3 above (#3 switches
*whether the SoC's own video reaches the panel at all*; #4 selects *which camera the SoC's
own video pipeline is actually looking at*). **Real, confirmed gap**: `custom_ui` never
invokes this mechanism anywhere in its source -- the "Aftermarket Camera" setting/screen
does not currently select a real aftermarket camera source at the hardware level. User
confirmed on real hardware (2026-09-02) that the aftermarket toggle does not work,
consistent with this gap.

### 5. What `custom_ui` actually does today, post-2026-09-02 simplification

- **OEM Factory Camera**: nothing reactive. Logs the (still-received, still-tracked)
  `CMD 0x12`-derived state for diagnostics only -- the actual relay switch is 100%
  autonomous MCU hardware (mechanism #3). This is the fix that closed out today's whole
  false-trigger bug class.
- **Aftermarket Camera**: still navigates to its own LVGL camera screen on `CMD 0x12`
  engage (the same signal already confirmed unreliable -- not yet revisited for this path),
  and does **not** invoke mechanism #4 (RN6752 source selection) at all -- a real,
  unimplemented gap, not yet fixed.
- **Volume cut + AA-resume-video nudge**: both still react to the same `CMD 0x12` signal,
  independent of camera mode. Deliberately left as-is (low consequence if occasionally
  wrong -- a redundant volume dip or resume request, not a broken/stuck display).

### Open items, stated plainly

- The "backcar enable/disable" *MCU-to-SoC* command this doc spent most of the day
  searching for was never found in that direction, despite exhausting the entire
  `MCUAdapter_BoxP300` dispatch table. Given mechanism #1 above, the real "backcar
  notification" concept turns out to run the *other* direction (SoC-to-MCU, via `ttyS2`,
  not `ttyHS0`) -- a genuinely different channel this project's software has never used.
- Whether `ttyS2`'s protocol is actually understood by the current MCU firmware is
  unverified either way.
- Wiring up the RN6752 mechanism for aftermarket camera mode is the concrete, real next
  step if that mode is worth fixing.

## RESOLVED (2026-09-02): OEM camera stuck after settings change, reboot doesn't fix it -- CMD 0x84 was the missing half

Real hardware bug report following the OEM-path simplification above: OEM camera got stuck
(not showing) after a settings change, persisted across a SoC reboot, but was fixed by
booting into stock `MsnCoreApp` instead. The reboot-doesn't-fix-it detail was the key clue --
the MCU is a separate, continuously-powered chip, confirmed unaffected by anything on the
SoC side, so this couldn't be a `custom_ui`-side state issue; it had to be that `custom_ui`'s
sync was genuinely incomplete compared to what stock does.

**Root cause**: `CMD 0xA0 id=0x11` (`custom_ui`'s only mechanism for this until now) is gated
on `flag_5e` already reading `1` -- per the arm-then-trigger design already traced in this
doc, it only *immediately* forces the `GPIOC13`/`PC2` relay while the MCU currently thinks
it's in reverse. Outside that window (the common case -- including right after boot, or a
settings change made while not reversing) it only updates the stored preference; the relay
itself doesn't physically move until the MCU's own next real `GPIOB Pin 2` edge. A stale
relay state from before a settings change can therefore persist indefinitely with nothing to
correct it.

**The fix**: `CMD 0x84` drives the exact same relay dispatcher (already documented above,
`0`=state0/LVGL/Aftermarket, `3`=state1/OEM) with its own gate polarity -- the opposite of
`id=0x11`'s. Stock `MsnCoreApp` very plausibly sends this unconditionally as part of its own
settings sync, which is the most likely reason booting into it fixed the stuck state.
`custom_ui` already had a real HAL wrapper for this exact command (`sync_audio_route()`/
`send_mcu_audio_route()`) sitting completely unused -- now wired into `sync_video_relay()`
so it fires alongside `id=0x11` on every sync (boot and settings-toggle both route through
this one function now). Build-verified (`custom_ui` commit `f7f0442`), **pending hardware
retest**.

## CONFIRMED (2026-09-02): `CMD 0xA0 id=0x00` independently controls the OEM camera relay -- the "Microphone Source" label was always wrong, exactly as flagged

This settles what `settings_screen.cpp`'s own `id=0x00` toggle comment had already flagged as
a real, untested possibility (added earlier this session, see that comment's own "Kept as a
plain two-state toggle deliberately, exactly so both real values can be tested directly on
hardware" note). **Superseded explanation, corrected later the same day** (see the
`id=0x00`/`id=0x01` correction further up this doc and `MCU_FIRMWARE_VERIFIED_FINDINGS.md`'s
own dedicated correction section): the reasoning below this point originally framed this as
a vendor label-vs-value-text mismatch ("Reversing camera" label vs. mic-sounding
value-texts). That framing was itself built on an indexing error in this project's own
earlier analysis, not a real vendor bug — `id=0x00`'s real value-texts, independently
re-derived, are the camera strings all along, not mic-sounding at all. The *hardware test
result* below is unaffected by this correction and remains the real, decisive evidence;
only the "why the UI label was wrong" explanation needed fixing.

**Real, methodical hardware test**: user toggled `id=0x00` (the "Microphone Source
(OEM/AfterMarket)" row) repeatedly, engaging real reverse gear after each toggle to observe
the effect, with `id=0x11` (the "OEM Factory Camera" toggle) held **fixed** throughout --
ruling out an interaction effect. `id=0x00` alone reliably determined whether the OEM
factory relay engaged during reverse. (Testing the Aftermarket side specifically wasn't
possible -- no physical aftermarket camera is connected on this vehicle, and `custom_ui`
doesn't yet call `start_camera_stream()` for that screen either, a separate already-
documented gap -- but the OEM-engage behavior alone is a clean, decisive result.)

This also lines up with the real MCU-side handler already documented for `id=0x00`
(`hardware/MCU/source/src/uart_protocol.c`): it drives **`GPIOB Pin 1`** -- directly
adjacent to `GPIOB Pin 2`, the pin this whole day's `flag_5e` tracing has centered on. Not
confirmed as the identical mechanism, but a real, physically plausible relationship, not a
coincidence.

**RESOLVED (2026-09-02)**: a same-methodology follow-up test (`id=0x00` held fixed this
time, `id=0x11` toggled/tested instead) found `id=0x11` "didn't seem to do anything" on its
own -- `id=0x00` is the confirmed-working lever, `id=0x11` an unconfirmed-if-load-bearing
secondary at best. `sync_video_relay()` now sends `id=0x00` alongside the existing `id=0x11`
and `CMD 0x84` sends (`custom_ui` commit `11128ca`) -- `id=0x11`'s send was kept rather than
removed (harmless per this same testing, and still the one signal with an independently
disassembly-confirmed `GPIOC13`/`PC2` effect under its own gate condition), so this adds the
confirmed-working signal rather than replacing anything not proven harmful to keep. The
standalone, misleadingly-labeled "Microphone Source" toggle was removed from Settings --
its real function is now folded into "OEM Factory Camera". Build-verified, **pending
hardware retest**.
