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

| Cmd | Meaning | Values / detail | Status |
|---|---|---|---|
| `0x81` | Init handshake / keepalive, resets internal state slots | `payload[0]=0x01` fixed | ✅ confirmed (`0x080088B5`) |
| `0x82` | App foreground/mode change (`onModeAppChanged`) | Send-side `mode` `2`/`4`/`5`/`7`/`13` append extra byte `8`; `mode=23` appends `0x0A`; else nothing appended. Receive-side, MCU `payload[0]==1` writes state struct `0x20000282` (`offset[0]=1,[1]=4`); else `offset[0]=2,[1]=1` — both call the same internal event-queue function. **Re-confirmed independently (2026-09-02)** via fresh disassembly straight from `can_app.bin`'s own real dispatch table (handler `0x08008bd4`) rather than trusting the prior doc pass — byte-for-byte the same finding. **Unconfirmed hypothesis, still open**: struct might be the input-focus switcher between factory/OEM and app mode (which subsystem gets knob ticks) — no consumer found that branches on it (89 real load sites, too broad to trace fully; not re-attempted this pass). A separate report's claim this was already proven ("routes rotary knob event target and CAN arbitration priority") was checked and rejected — overstates the trace. | ✅ confirmed both directions |
| `0x84` | Audio route select — **also drives the same `GPIOC13`/`PC2` relay dispatcher as `CMD 0xA0 id=0x11`**, see the [4-state table](#cmd-0x84--cmd-0xa0-id0x11-shared-relay-dispatcher) below | Masked to 4 bits, `≥6` ignored. Value `0` → `"AT+AUDROUTE=1\r\n"` + relay state `0`; value `3` → `"AT+AUDROUTE=2\r\n"` + relay state `1`; `1`/`2`/`4`/`5` → state-only, no relay action. **RESOLVED (2026-09-02)**, see the [full `CMD 0x82`/`0x84` gate trace](#cmd-0x82--cmd-0x84-real-mcu-side-trace-2026-09-02----the-shared-gate-conflict-is-settled) below: the "2 unreconciled conflicts" are now 1 resolved + 1 reconfirmed. Read-byte conflict (`payload[1]` vs `payload[0]`) reconfirmed as a real, consistent frame-struct convention (offset `+2`=`payload[0]`, `+3`=`payload[1]`), not an actual disagreement. **Gate-polarity "conflict" is resolved**: both `CMD 0x84` and `CMD 0xA0 id=0x11` read/write the exact same absolute SRAM byte (`0x20000236`) for their offset-`0x5e` check — genuinely the same flag, opposite required polarity by design (arm-then-trigger vs. fire-when-not-already-armed), not two independent flags. | ✅ confirmed, gate-sharing conflict resolved |
| `0x85` | App-protocol ACK | `payload[0..2]` stored into an internal queue slot | ✅ confirmed shape / ❌ clean-room reply content is an approximation |
| `0x87` | Bluetooth AT-command relay, verbatim passthrough to `USART3` (`PB10`/`PB11`) | Real bug: `AT+PIN=0000`'s digit-substitution is unwired, very likely always sent malformed | ✅ confirmed (`0x080087A1`) |
| `0x88` | TEA-cipher anti-clone challenge, 8-byte block | Real cipher, genuine key, decrypted reply read by nothing (inert as shipped) | ✅ fully confirmed |
| `0xA0` | UI settings sync — see the [dedicated sub-table](#cmd-0xa0-sub-table-settings-sync-by-id) below | — | ✅ confirmed |
| `0xE1` | Enter bootloader for update | **Real erase risk**: resident bootloader erases flash before waiting for the first byte, no recovery if nothing follows. `tools/mcu-probe --reboot-probe` gated behind `--confirm-erase-risk` | ✅ confirmed |
| `0xFF` | System state reset | Sub-id `0x00`-`0x09` all no-ops, only `0x7F` acts | ✅ dispatch confirmed / partial trace beyond the `0x7F` gate |

### `CMD 0xA0` sub-table (settings sync, by `id`)

Real, closed, disassembly-confirmed range: `id` `0x00`–`0x11` (18 entries, real TBB jump
table). Anything outside this range (e.g. a separate report's claimed `id=0x87`) falls
outside the confirmed dispatch table and is very unlikely to do anything real.

| id | Vendor label | Values | Real MCU-side effect | `custom_ui` |
|---|---|---|---|---|
| `0x00` | Camera Type | `0`=AfterMarket / `1`=Factory / `2`=AfterMarket 360 / `3`=Factory 360 | `GPIOB Pin 1`. **Hardware-confirmed: controls the OEM camera relay.** (Value `2` is a distinct `AT+UPGRADE` trigger, not a camera value) | ✅ sent by `sync_video_relay()` |
| `0x01` | — (no real label) | — (no real values) | Dead no-op — shares the same handler as `0x02`–`0x06`/`0x0E` | not wired |
| `0x02`–`0x06` | — | — | Dead no-op (same shared handler) | not wired |
| `0x07` | "Radar" | — | Write-only, no consumer | not wired |
| `0x08` | "Trajectory" | Off/On | Write-only, no consumer | not wired |
| `0x09` | "Reversing mode" *(vendor mislabel)* | Off/On | Mic/audio input mux, `GPIOB Pin 6`. Confirmed byte-identical across 5 real firmware images | ✅ "OEM Factory Microphone" |
| `0x0A` | "360 camera" | CAN Active/12V Active/P Key Active | Write-only, no consumer | not wired |
| `0x0B` | "Front camera" | empty/dynamic | `PA15`/`PB8`/`PB9` 3-pin enable when cleared to `0` | not wired |
| `0x0C` | "Front camera time" | Off/Radar Active/5s/10s/15s | Real reader, thresholds `0x0B`'s 3-pin group | not wired |
| `0x0D` | "Speech button" | 5s/10s/15s | Write-only, no consumer | not wired |
| `0x0E` | "DVR" | Off/On | Dead no-op | not wired |
| `0x0F` | "Right Camera" | Off/On | `PA15`/`PB8`/`PB9` relay trio (same as `0x0B`), gated by a second flag | not wired |
| `0x10` | "Left Camera" | Off/On/12V Active | Same `PA15`/`PB8`/`PB9` trio, adjacent gate | not wired |
| `0x11` | "Video Source" *(vendor mislabel: "Microphone")* | `0`=SoC/LVGL / `1`=OEM Camera | `GPIOC13`/`PC2` relay pair — arm-then-trigger (see below); **hardware-confirmed autonomous switching** | ✅ sent by `sync_video_relay()` |

**`id=0x11`'s arm-then-trigger mechanism, in short**: sending `id=0x11` only *arms* the
preference — the physical relay switch happens later, on the MCU's own `GPIOB Pin 2` edge
(a real, hardware-polled pin, not something `custom_ui` triggers). Hardware-confirmed to
work with zero software running at all (`custom_ui` killed entirely, OEM camera still
engaged correctly). Full trace: `docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md`.

**Real stock camera-source mechanism, separate from all of the above**: stock's own
OEM/aftermarket switch is a U-Boot env var (`fw_setenv carback_camera_mode`) plus a kernel
`rn6752` I2C sysfs write — zero MCU involvement. Unrelated to `id=0x11`'s relay.

<details>
<summary>Revision history for this table (click to expand)</summary>

- **2026-09-02**: `id=0x00`/`id=0x01`'s value-texts were found swapped in the 2026-08-29
  pass — `id=0x00` is the real Camera setting (re-derived from `getSetItemValueTexts(0)`,
  `0x00032460`, byte-for-byte); `id=0x01` has no real value-texts at all. Real hardware
  testing separately confirmed `id=0x00` controls the OEM relay and `id=0x11` "didn't seem
  to do anything" on its own — `sync_video_relay()` now sends both, plus `CMD 0x84`.
- **2026-08-31**: `id=0x0F`/`id=0x10` corrected from "no GPIO effect" to real, confirmed
  `PA15`/`PB8`/`PB9` relay control. `id=0x09` cross-confirmed identical across 5 real
  firmware images. `id=0x11`'s `flag_5e`/`GPIOB Pin 2` arm-then-trigger mechanism fully
  traced.
- **2026-09-01**: `id=0x11`'s autonomous relay switching hardware-confirmed with
  `custom_ui` killed entirely. The per-transition resend of `id=0x11` (present until this
  date) was found to actively break repeat engagements and was removed.

Full evidence trail for every row: `docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md`.
</details>

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

| Cmd | Meaning | Values / detail | Status |
|---|---|---|---|
| `0x00` | Default / ignored (no-op) | — | ✅ confirmed (`0x37348`) |
| `0x01` | Headlights / illumination status (main use), plus 2 more real sub-fields `custom_ui` doesn't use | Bit `1` = headlights ON/OFF (`MsnEvent 0x5004`/`0x5005`) — live-hardware confirmed, wired end-to-end into `custom_ui`'s AA night-mode feature. Bit `2` = reverse-camera override (`MsnEvent 0x5026`) — confirmed, not used by `custom_ui`. A later payload byte's bit `7` is a legacy key-matrix press/release, `MsnEvent 0x1013` — confirmed, not used. | ✅ confirmed |
| `0x02` | Knob/button event | `b3=3`/`4` = Next/Prev Track (confirmed, live-tested — NOT volume). `b3=8`/`9` = Answer/Hangup. `b3=12`/`13` = Home/Knob push. `b3=36` = "Mode/Source" (single observation only). `b3=64`/`65` = Knob CCW/CW. The `8`/`9`, `12`/`13`, `64`/`65` pairs each share every bit except bit `0` (upper bits = control group, bit `0` = state/direction) — `3`/`4` don't fit this pattern, inferred not confirmed. | ✅ live-capture confirmed, what `custom_ui` runs on |
| `0x03` | Dual-zone HVAC/climate status broadcast — fully traced, real named consumer, not just a bitfield | See the [full `CMD 0x03` byte-level decode](#full-cmd-0x03-byte-level-decode-2026-09-02----a-dual-zone-hvac-frame-calling-straight-into-airconditiondlg) below. Old "`payload[0]` bits `5`/`6`/`7`" claim was correct on the *bits*, wrong on the *byte* — it's `payload[1]`, not `payload[0]`. | ✅ confirmed, real `AirConditionDlg` setter calls, not an inferred event |
| `0x04` | Parking radar / distance level (`transRadarLevel`) — empirically correlates with reverse gear. See the [reverse-gear conflict](#the-reverse-gear-command-conflict--the-one-that-actually-matters-right-now) section | See the [byte-level `CMD 0x04` decode](#real-byte-level-decode-of-cmd-0x04s-payload-2026-09-02----and-why-this-strengthens-the-case-for-using-it-as-corroboration) further down for the full 4-channel `transRadarLevel()` breakdown | ✅ confirmed |
| `0x05` | Radar-family telemetry, sibling of `CMD 0x04` — theory now resolved in favor of radar over HVAC | Full re-trace of the handler (`0x38190`) settles the old conflict: it builds a 4-byte `QByteArray`, one byte per `payload[0..3]`, each range-checked against the exact same `cmp X,#15`/`cmp Y,#29` clamp pattern `CMD 0x04`'s `transRadarLevel()`-family classification uses — structurally identical to `CMD 0x04`'s handler, not anything resembling `CMD 0x03`'s `AirConditionDlg` bitfield path. Posts `MsnEvent(app=0x191, type=0x5018)` — **one less than `CMD 0x04`'s `0x5019`**, i.e. an adjacent event-type constant in the same family. Real evidence now favors "front/secondary radar channel, sibling to `CMD 0x04`'s rear radar" over the old HVAC theory; zero live captures still exist either way, so this is a disassembly-only resolution, not hardware-confirmed. | 🟡 resolved by disassembly, no live capture yet |
| `0x06` | Vehicle dynamics/safety bitfield, real structure now traced beyond "bit positions confirmed" | Handler (`0x38360`) builds a `QBitArray(8)` from `payload[1]` (byte `[3]`) — each of its 8 bits tested independently (`asr`/`tst` per bit) and packed as one `setParams()` argument, i.e. genuinely 8 independent boolean flags, not a single code. A **second, separate 4-bit field** is built from `payload[2]` (byte `[4]`) bits `4`-`7`, but re-packed in a non-obvious order (final nibble = `bit6`\|`bit7`<<1\|`bit4`<<2\|`bit5`<<3) into the other `setParams()` argument — this is a **repacked small code, not 4 independent flags**, correcting the old "bit `4`=Parking Brake / `5`=Footbrake / `6`=Turn Signals / `7`=Reverse" guess table, which assumed 4 independent booleans at those positions. Posts `MsnEvent(app=0x190, type=0x501A)`. Exact semantic labels for either field still unconfirmed — would need the real consumer in `MsnCoreApp` traced next. | 🟡 partial, structure now precise |
| `0x07` | Confirmed hard no-op, not a real command on this adapter | Ruled out by the exhaustive dispatch-chain trace above (`0x370d4`) — `0x07` is not one of the 14 values `MCUAdapter_BoxP300::onRecvMcuProtocol` branches on, so it falls straight through to the shared no-op epilogue (`0x37348`) exactly like `0x00`/`0x40`. Corrects the old "not resolved, single never-cross-checked source" status, which had left open the possibility this was a real, unexplored command. | ✅ confirmed no-op |
| `0x0A` | Steering angle / reverse trajectory | `byte[3]` bit `0` = direction (confirmed), `byte[4..5]` = 16-bit magnitude scaled to a signed angle (position confirmed, scale factor not cracked) | ✅ confirmed |
| `0x12` | Reverse-gear-adjacent, real payload structure but NOT a reliable reverse-gear signal — see the [reverse-gear conflict](#the-reverse-gear-command-conflict--the-one-that-actually-matters-right-now) section, superseded 2026-09-02 by the [CRITICAL](#critical-2026-09-02-cmd-0x12-confirmed-unreliable-on-real-hardware----fires-from-headlights-alone-no-gear-change) findings further down | `payload[0]` = `0x01` entering / `0x02` exiting; only semantically meaningful in the real vendor app when `payload[1]==0x11` (every captured frame so far has `0x04` or `0x01` instead — a hard no-op per stock software, see the full trace further down) | ⚠️ confirmed unreliable |
| `0x20` | Touch coordinate report | X = `(payload[1]<<8)\|payload[0]`, Y = `(payload[3]<<8)\|payload[2]`, native `0`-`800`/`0`-`480` px, all confirmed. All-zero payload = release, confirmed. `payload[4]` (bit `7`) values `1`/`2` possibly distinguish touch-down vs. move, but `custom_ui` never branches on it and down/move/release all already work correctly via the `touch_pressed_` + coordinate-change state machine — stays unconfirmed, not a real gap. | ✅ confirmed, most rigorously verified finding in the corpus |
| `0x21`/`0x22` | Not resolved — hypothesis: multi-touch/gesture dispatcher | Real dispatch mechanism now confirmed (`0x37514`): both values route through a genuine virtual-function call (vtable slot `0xf8`) on an internal static object pointer, guarded by a null check — a deliberate dispatch to *something*, not dead code, but which class that pointer resolves to at runtime wasn't pinned down (the slot itself is populated by relocation-free, purely-local PIC addressing, so no symbol name is attached statically; would need the setter of that static pointer traced next). Scanned all 5 known firmware images for the MCU-side counterpart, no hit resembling an outbound frame. | ❌ unconfirmed target class, ✅ confirmed real (non-dead) dispatch |
| `0x30` | Arkdata display-profile selector | `payload[0]==0x0C` is the only handled sub-type, everything else a silent no-op | ✅ confirmed (`/msnprofile/arkdata.ini`) |
| `0x40` | Fires once during the startup telemetry burst | `len=1`, payload never inspected | 🟡 SoC-side no-op confirmed, MCU-side purpose unconfirmed |
| `0x60` | `CMD 0x88` TEA-challenge reply opcode | The decrypted reply's real content — see `MCU_FIRMWARE_VERIFIED_FINDINGS.md`'s `0x88` trace | ✅ confirmed |
| `0x7F` | MCU version report | `payload[0..27]` = 28-byte ASCII string; `/tmp/mcu_version` is separately hardcoded, not sourced live | ✅ confirmed |
| `0xE2`/`0xE4` | Firmware-update handshake | Cited strings: `"End Update Mcu!"`, `"recv update packageid:"` | ✅ confirmed — not implemented anywhere in this project |

---

## The reverse-gear command conflict — the one that actually matters right now

Real, current status of every candidate reverse-gear signal this project has traced. **The
OEM camera path no longer depends on any of these** (see the "RESOLVED" architecture section
further down) — this table now matters only for the Aftermarket camera path, volume cut, and
the AA-resume nudge, which still key off `CMD 0x12`.

| Candidate | Real status | Currently used for |
|---|---|---|
| `CMD 0x04` presence | Confirmed **not** reverse-gear-related — real parking radar telemetry, correlates with reversing by coincidence (sensors activate around the same time) | Not used (demoted to a no-op) |
| `CMD 0x12` presence, direction-blind (old model) | Superseded — `CMD 0x12` carries both directions in its own payload, this model was wrong | superseded |
| `CMD 0x12` `payload[0]` direction (`0x01`=entering, `0x02`=exiting) | Tracks *real* reverse-gear transitions reliably when tested directly — but confirmed **not exclusive** to reverse gear: fires from a plain headlights toggle too (hardware-confirmed twice, ~29ms after the headlights frame, zero gear involvement). Also confirmed a hard no-op in the real vendor app for every payload captured so far | Aftermarket camera nav, volume cut, AA-resume nudge. **No longer used for the OEM camera path** (removed 2026-09-02) |
| `CMD 0x01` bit `2` (`payload[0]=0x15`) | Real disassembly convergence with `CMD 0x12`'s one meaningful case (same `MsnEvent 0x5026`, itself confirmed to have zero consumers). Separately: 2 real live captures both show it firing 150-214ms *before* genuine `CMD 0x04` radar activity — a cleaner correlation than `CMD 0x12` has shown so far | Not wired in — promising lead, not yet implemented |
| `/dev/carback` | Confirmed dead by **deliberate** 2026-07-17 design decision (real devicetree node removed, not a bug) — not revivable, its old GPIO pin gets reclaimed for the LCD once Linux boots | Present in code as a fallback, not usable on this hardware |

<details>
<summary>Revision history for this table (click to expand)</summary>

- **2026-08-31/09-01**: originally modeled as `CMD 0x04`=engage/`CMD 0x12`=disengage — wrong
  model, superseded once `CMD 0x12`'s own payload was found to carry both directions.
- **2026-09-01**: `CMD 0x12 payload[0]` direction mapping hardware-confirmed (user identified
  which of two captured events was the real enter vs. exit).
- **2026-09-02**: two real regressions hit on retest (exit flash-then-blank; boot-time false
  engage from the MCU's own startup burst) — both fixed (debounce + startup grace window,
  `custom_ui` commits `fbdcc7b`/`4b7f230`). Then the bigger issue: `payload[0]==0x01`
  confirmed to fire from headlights alone (see the CRITICAL section below). Ultimately
  resolved for the OEM path by removing its dependency on this signal entirely rather than
  continuing to patch around it (see the "RESOLVED" architecture section further down).

Full evidence trail: `docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md`.
</details>

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

## `CMD 0x82` / `CMD 0x84` real MCU-side trace (2026-09-02) -- the shared-gate conflict is settled

Fresh, direct disassembly of `hardware/MCU/can_app.bin` (Thumb-2, real STM32 firmware) this
time, not `libMcuCenter.so` -- these are SoC->MCU commands, so their real handlers live on the
MCU side. **Real base-address correction needed first**: this binary's application code does
not start at `0x08000000` -- that address range is the separate `bootloader.bin`. The real
base is `0x08004000` (confirmed by cross-checking: `CMD 0x84`'s already-documented handler
address `0x08008808` only resolves to valid, sensible code -- a real `push {r4,lr}` function
prologue -- under this base, not `0x08000000`, and every other address cited elsewhere in
this doc for `can_app.bin` is consistent with it). The real 9-entry SoC->MCU dispatch table
was re-extracted directly from the binary's own bytes (table base `0x0800B9E4`, confirmed
identical to the address already cited elsewhere in this doc) to get exact handler addresses:
`0x81`→`0x080088B5`, `0x82`→`0x08008BD5`, `0xA0`→`0x080089D9`, `0xFF`→`0x080088E9`,
`0xE1`→`0x080088E1`, `0x85`→`0x08008BA9`, `0x84`→`0x08008809`, `0x87`→`0x080087A1`,
`0x88`→`0x0800893D` (all Thumb, odd bit set).

**`CMD 0x84` handler (`0x08008808`), the exact real gate check**: reads the incoming frame's
byte from struct base `0x20000238` offset `+3` (confirmed to be a generic per-command
"current frame" struct: offset `+0`=cmd, `+1`=len, `+2`=`payload[0]`, `+3`=`payload[1]`,
etc., re-derived directly rather than assumed). Before calling the shared relay dispatcher
(`0x080058A4`), it checks `ldrb.w r0, [r0, #0x5e]` against a base loaded from literal
`0x08008898`, which resolves to **`0x200001D8`** -- so the real absolute address checked is
`0x200001D8 + 0x5E = 0x20000236`.

**`CMD 0xA0 id=0x11`'s own handler**, independently located and disassembled (found via every
real caller of the shared dispatcher `0x080058A4`, not assumed): its own gate-arming write
(`strb.w r0, [r1, #0x5e]` at `0x8008859a`/`0x80085ae`) uses a base loaded from literal
`0x080085dc`, which resolves to **the exact same `0x200001D8`** -- so its own absolute address
is also `0x20000236`.

**This resolves the doc's old "read from different SRAM struct bases, unclear if same flag"
conflict decisively: they are not different bases, they are the same base, same offset, same
absolute byte.** `CMD 0x84` and `CMD 0xA0 id=0x11` genuinely read/write **one shared flag
byte at `0x20000236`** -- not two independent flags that happen to occupy the same relative
offset by coincidence. This also gives the arm-then-trigger mechanism (already documented
elsewhere in this doc) a much more concrete mechanical explanation: `id=0x11` *arms* by
writing this byte to `1` only when the MCU's own `GPIOB Pin 2` handler has already set it
(i.e. "already reversing"), and fires the relay immediately in that case; `CMD 0x84` fires
the *opposite* case (byte `== 0`, "not currently reversing", the common case right after a
settings change or boot) -- both are reading and reacting to the same underlying "is the MCU
currently in a reverse-detected state" flag, from two different code paths with complementary
conditions. This is real, concrete confirmation of the mechanism this project has relied on
(sending both `id=0x11` and `CMD 0x84` together in `sync_video_relay()`) since before this
byte-level proof existed.

**CORRECTION (2026-09-02, same day): the "third call site" above was id=0x11 itself, not a
new command** -- re-verified by decoding `CMD 0xA0`'s real `tbb` (table-branch-byte) jump
table directly from the binary's own bytes (base `0x080089e6`, 18 real entries, bounds-checked
`id < 18` beforehand, matching the doc's existing "18-entry" claim) rather than assuming from
code shape alone. `id=0x0F` -> `0x08008b46`, `id=0x10` -> `0x08008b52`, `id=0x11` ->
`0x08008b5e` -- the call sites at `0x08008b7a`/`0x08008b82` are inside `id=0x11`'s *own* case
body, exactly where expected. No third command reaches the shared dispatcher this way; the
earlier "very likely id=0x0F" guess was wrong, corrected here rather than left standing.

**The real news is bigger: found the literal MCU-side source of the hardware-autonomous
relay-switching mechanism itself.** Tracing where the *previous* pass's `0x08008548`-area
block (originally mis-assumed to be part of `CMD 0xA0 id=0x11`'s own body -- it isn't, per the
correction above) actually comes from led to a genuinely new function, `0x080084A4`, which
**is not reached from either the SoC->MCU dispatch table or the `CMD 0xA0` id-switch at all**
-- it's a free-standing, periodically-invoked GPIO scanner:

- Calls 5 small wrapper functions, each reading one real GPIO pin via a shared
  `port_base + mask` helper (`0x08005582`) -- decoded directly from each wrapper's own
  literal-pool port-base value (STM32F105 standard: GPIOA=`0x40010800`, GPIOB=`0x40010C00`,
  GPIOC=`0x40011000`): **`GPIOA Pin 8`**, **`GPIOC Pin 9`**, **`GPIOC Pin 8`**,
  **`GPIOC Pin 7`**, and **`GPIOB Pin 2`** -- packed into bits `0`-`4` of one combined state
  byte.
- Compares the combined state against the *previous* scan's value (stored at `0x080085d8`)
  and returns immediately if unchanged -- a real, deliberate change-only/edge-triggered design,
  not a naive poll-and-act-every-time loop.
- On a real change: bits `0`/`1` (`PA8`/`PC9`) combine into a debounced 2-bit "mode" value
  (stored at struct offset `0x36`, requires 2 consistent reads before committing -- real
  hardware debounce); bits `2`/`3` (`PC8`/`PC7`) combine similarly into offset `0x46`; **bit
  `4` (`GPIOB Pin 2`) is extracted, inverted (`rsb r0,r0,#1`), debounced the same way, and on
  a confirmed change writes the shared `0x5e` flag (`0x20000236`, the exact byte this section
  already proved `CMD 0x84`/`CMD 0xA0 id=0x11` both gate on) and calls the shared relay
  dispatcher (`0x080058A4`) directly** -- the real call sites are `0x0800859e`/`0x080085bc`/
  `0x080085ce`, which is what this doc's *previous* pass had mistakenly attributed to
  `id=0x11`'s own body.

**This is the real, concrete mechanism behind the already-hardware-confirmed "MCU switches the
OEM camera relay autonomously, with `custom_ui` killed entirely" finding elsewhere in this
doc** -- not just corroborating evidence, the literal source code for it. `GPIOB Pin 2` is
read directly by dedicated, debounced, change-triggered polling code, independent of any UART
command from the SoC. **Real, valuable open follow-up, not chased this pass**: what `PA8`,
`PC9`, `PC8`, and `PC7` physically correspond to on this board -- their debounced 2-bit/2-bit
outputs (struct offsets `0x36`/`0x46`) aren't yet traced to any consumer, and are real
candidates for some of this doc's still-unconfirmed bit-meaning gaps (e.g. `CMD 0x06`'s
vehicle-dynamics bits, or the "backcar" mechanism's remaining loose threads) given they sit in
the exact same scan routine as the one pin already proven load-bearing.

**`CMD 0x82`'s own handler (`0x08008bd4`) re-confirmed independently**: reads `payload[0]`
from the same `0x20000238`-based "current frame" struct at offset `+2` (consistent with the
`+3`=`payload[1]` finding above -- same struct, same convention, cross-validating both).
`payload[0]==1` writes `{offset[0]=1, offset[1]=4}` into struct `0x20000282`; else
`{offset[0]=2, offset[1]=1}` -- both branches call the same event-queue function
(`0x08006228`) with identical arguments, matching the existing doc entry exactly. The
already-documented "who consumes `0x20000282`" question (89 real load sites) was not
re-attempted this pass -- still the real remaining gap for this command.

## Full `CMD 0x03` byte-level decode (2026-09-02) -- a dual-zone HVAC frame calling straight into `AirConditionDlg`

Full trace of `MCUAdapter_BoxP300::onRecvMcuProtocol`'s real `CMD 0x03` handler (`0x388b4`,
same unstripped `libMcuCenter.so` copy used throughout this doc). This handler is genuinely
large (~1300 bytes, the biggest single-command handler traced this session) because it isn't
just decoding a bitfield and posting one event -- it calls **directly into a real, named Qt
dialog class, `AirConditionDlg`**, synchronously, once per real HVAC setting. That settles the
old "who consumes this" ambiguity outright: the consumer is `AirConditionDlg` itself, not an
`MsnEvent` some other subsystem has to interpret.

Real byte layout, `payload[n]` = `byte[2+n]` as elsewhere in this doc:

```
payload[1] (byte[3]) -- 8-bit flags, QBitArray-packed one bit at a time:
  bit 5 -> AirConditionDlg::setCirculationMode(uchar)
  bit 6 -> AirConditionDlg::setACEnable(bool)
  bit 7 -> AirConditionDlg::setAirConditionEnable(bool)
  bits 0-4 -> read into the bitarray but no confirmed consumer found in this handler

payload[2] (byte[4]) -- 8-bit flags (same QBitArray pattern) AND reused whole:
  low nibble (payload[2] & 0xF) -> AirConditionDlg::setAirVolume(uchar) directly
    (0 -> sentinel 0xFF, i.e. likely "Auto"; 1-15 -> the raw value)
  bit 6, bit 7 of the bitarray -> composed into a 2-bit code (0/2, 0/4) feeding
    AirConditionDlg::setWindDirectEx(uchar) (left/main zone)
  bit 5 of the bitarray -> also feeds into setWindDirectEx_right's composition

payload[3] (byte[5]) -- left-zone target temperature:
  0x00 -> distinct branch (likely "Off")
  0x1F (31) -> distinct branch (likely "Auto" / high-limit)
  0x01-0x1C (1-28) -> range-checked, formatted into a QString via
    AirConditionDlg::setTemperature(QString left, QString right)'s left argument

payload[4] (byte[6]) -- right-zone target temperature, identical 0 / 31 / 1-28 encoding,
  same setTemperature() call's right argument

payload[5] (byte[7]) -- more flag bits, gates rather than data:
  bit 0 -> gates whether a literal "N/A" QString gets built for one of the temperature slots
  bit 1 -> AirConditionDlg::setACVisible(bool)
  bit 2 -> gates whether setWindDirectEx gets called at all
  bit 4 -> a second gate on the setWindDirectEx composition

payload[8] (byte[10]) -- bit 6 / bit 7 feed into setWindDirectEx_right's composition
  alongside payload[2]'s bit 5 above (the "right zone" wind-direction call)
```

**This corrects, not just extends, the old doc entry.** The previously "confirmed" claim --
"`payload[0]` bits `5`/`6`/`7` = `CirculationMode`/`ACEnable`/`AirConditionEnable`" -- had the
right bit positions and the right real setter names, but the wrong byte: those three calls
are driven by `payload[1]`, not `payload[0]`. `payload[0]` (byte `[2]`, the very first payload
byte) has **no confirmed consumer anywhere in this handler** -- not referenced by any of the
traced setter calls above. This is a real, previously-undocumented gap of its own, distinct
from the "bits 0-4 of `payload[1]` unconfirmed" gap.

**Not chased further this pass**: the exact `MsnEvent` type this handler posts (if any -- it's
possible this command's real effect is entirely the direct `AirConditionDlg` calls above, with
no event dispatch at all, unlike every other MCU->SoC command traced in this doc). The
function has several branch targets outside the `0x388b4`-`0x38d34` range walked here
(QString-formatting helper branches for the temperature sentinel values) that weren't
individually traced -- low priority, since they're formatting detail for the two temperature
strings already identified, not new fields.

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

## NEW FINDING (2026-09-02): the real "backcar enable/disable" mechanism is a filesystem
## marker consumed by `sink`, not a UART command -- found via a binary nobody had opened yet

The "still genuinely unresolved" item above (no SoC->MCU or MCU->SoC command matches the
DTS's own "backcar enable/disable command" description) is now closed differently than
expected. Two real, disassembly-backed steps:

**1. The `MCUAdapter_BoxP300::onRecvMcuProtocol` dispatch is confirmed exhaustively closed.**
Full linear disassembly of the dispatcher (`0x370d4`, the real, unstripped `libMcuCenter.so`
copy) shows it is a straight chain of `cmp`/`beq` comparisons, not a jump table -- and it
handles **exactly 14 values**: `0x01 0x02 0x03 0x04 0x05 0x06 0x0A 0x12 0x20 0x21 0x22 0x30
0x7F 0xE2 0xE4`. Every other byte (including `0x00`, `0x07`-`0x09`, `0x0B`-`0x11`, and the
whole `0x13`-`0xFF` range apart from the listed values) falls straight through to the shared
no-op epilogue at `0x37348`. This rules out possibility (a) from the "still genuinely
unresolved" note above -- there is no hidden extra command on this channel; the full set was
already in this doc.

**2. `MsnEvent 0x5026` (the event `CMD 0x12`'s `payload[1]==0x11` case and `CMD 0x01` bit `2`
both post) has exactly 2 real producers inside `MCUAdapter_BoxP300` and zero real consumers
anywhere in `MsnCoreApp`** -- re-confirmed this session directly against the real, unstripped
`MsnCoreApp` binary (`with debug_info, not stripped` per `file`): searched every `movw
r_,#20518` (the `0x5026` immediate load pattern) in the whole binary, zero hits. So the two
already-documented producers really do dead-end inside `MsnCoreApp` specifically.

**3. The real consumer lives in a completely different binary this project had never opened:
`usr/bin/sink`** (also unstripped). It has its own `ArkReverse` class with real, non-stub
logic (confirmed by disassembling every method, not guessed from symbol names alone):
- `ArkReverse::init()` opens a local socket (`ArkUtils::open_local_socket()`), then
  `inotify_init()` + `inotify_add_watch(fd, "/tmp", IN_CREATE|IN_DELETE)` -- watching the
  **`/tmp` directory itself**, not a device node.
- `ArkReverse::watchHandleIsExist()` does `access("/tmp/video", F_OK)`.
- `ArkReverse::run()`'s inotify event loop, on detecting `/tmp/video` come into existence,
  calls a real, genuinely vendor-named (typo and all) callback: `IArkCallbacks::
  enterResverseCallback()`.

**This is the real "backcar enable/disable" mechanism the DTS comment describes** -- it's a
**filesystem marker file (`/tmp/video`)**, watched via `inotify`, not a distinct UART command.
It explains why the SoC->MCU/MCU->SoC dispatch tables above never turned it up: it was never
a wire-protocol command to begin with, on either UART channel.

**Also confirmed while chasing this**: wired CarPlay's own equivalent hooks,
`carplay_backcar_enter`/`carplay_backcar_exit` (imported by `usr/bin/carplay`, exported by
`usr/lib/libScreenStream.so`), are **hard `bx lr` no-ops in all 4 real firmware images**
checked (`Prado firmware dump`, `firmware_source`, `ztuzauto_extracted`,
`AA-NEW-P306_extracted`) -- genuinely dead in every real build, not an artifact of checking
only one copy. Same "named hook, does nothing" pattern this project has hit repeatedly with
`MsnEvent 0x5026`'s own consumers.

**Real, still-open gap**: who actually creates/deletes `/tmp/video` was not found this pass.
Not in any `rc`/init script (`/etc/rc.d/rcS` and the whole rootfs tree grepped, no hit), not
in `MsnCoreApp`, and this rootfs has no `mdev.conf` at all (so it isn't a hotplug rule
reacting to a `/dev/videoX` node appearing, at least not through that mechanism). Real
candidates found referencing the same `"/tmp/video"` string: `carlife` (Baidu CarLife --
also just a consumer, confirmed via its own `inotify_add_watch`/`inotify_init` imports, same
watcher pattern as `sink`), `libAvin.so` (an "AV-input" UI module with real symbols like
`AvinWindow::onDetectSignal()`/`checkHandBreakOrAllowVideoPlay()` -- plausible-looking but
not confirmed as the producer, and this module reads more like a legally-required
handbrake/video-while-driving gate than a reverse-gear detector), and `ECLink` (stripped, not
traced). **Not yet resolved**: which of these (or something else entirely, e.g. a compiled-in
GPIO poll loop inside one of the stripped binaries) is the actual producer that creates
`/tmp/video` when the vehicle goes into reverse.

**Practical relevance to `custom_ui`**: this is a real, previously-undocumented, genuinely
different mechanism from everything traced earlier in this doc (not UART, not `/dev/carback`,
not `flag_5e`) -- but it's the stock **wireless-mirroring sink's** own reverse hook, i.e. it
exists to pause/interrupt CarPlay/AA video during reverse, not to switch the OEM camera relay.
`custom_ui`/`androidauto-sidecar` don't currently watch `/tmp/video` at all. Whether this is
worth wiring in (e.g. as a corroborating signal, or to replicate stock's own
pause-mirroring-during-reverse behavior) is a real, new, open decision -- not yet acted on.

**Important caveat, checked directly rather than assumed**: `usr/bin/sink` is **not part of
this project's own dynamic-rootfs deployment at all** -- `firmware_overlay_dyn/etc/rc.d/rcS`
explicitly documents dropping the whole `MsnCoreApp`/`blueware`/`sink`/
`com.arkmicro.auto.service` reference chain. So on the system this project actually ships,
`ArkReverse`/`enterResverseCallback()` never runs, and nothing is watching `/tmp` for this
marker regardless of what creates it. This finding explains real *stock* vendor-software
archaeology (what the DTS comment was originally describing, and why `/tmp/video` never
turned up chasing UART traffic) but does not by itself unblock anything for the currently
running system -- it would only become directly useful if `custom_ui`/`androidauto-sidecar`
deliberately chose to replicate the same marker-file convention as a new signal source.

**Re-checked `CarSignalsWatch::startWatchSignals()` while looking for other candidate GPIO
watchers** (in case a third pin, e.g. the backcar GPIO 5, was also being watched and missed by
the earlier bits-24-27 pass): confirmed it still only ever opens exactly 2 `GPIOOperater`
instances (matching the already-documented GPIO 30/31 audio/BT pair) -- no additional pin.
One real, previously-unnoted nuance: which pins/tags it watches is **read from
`MsnApplication::getFactorySetting()`** (a config-driven `QVariant::toUInt()`), not hardcoded
literals -- meaning the exact pin set is theoretically product-config-dependent, though this
specific build resolves to the same 2 pins either way.

**Real, cheap next step if this is worth settling for good**: rather than more static
analysis of the 3 remaining stripped candidates (`carlife`, `libAvin.so`, `ECLink` -- none of
which is deployed on this project's own system anyway), a live test on real stock firmware
(`inotifywait -m /tmp` or a simple polling `ls -la /tmp` loop, run while a stock-firmware unit
is put in reverse) would settle who creates/deletes `/tmp/video` in seconds. Low priority
given the caveat above -- `sink` isn't in this project's own boot path -- but noted here so
it isn't re-derived from scratch if ever revisited.
