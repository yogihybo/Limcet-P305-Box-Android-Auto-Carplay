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
| `0x82` | App foreground/mode change (`onModeAppChanged`) | Send-side `mode` `2`/`4`/`5`/`7`/`13` append extra byte `8`; `mode=23` appends `0x0A`; else nothing appended. Receive-side, MCU `payload[0]==1` writes state struct `0x20000282` (`offset[0]=1,[1]=4`); else `offset[0]=2,[1]=1` — both call the same internal event-queue function. **Unconfirmed hypothesis**: struct might be the input-focus switcher between factory/OEM and app mode (which subsystem gets knob ticks) — no consumer found that branches on it (89 real load sites, too broad to trace fully). A separate report's claim this was already proven ("routes rotary knob event target and CAN arbitration priority") was checked and rejected — overstates the trace. | ✅ confirmed both directions |
| `0x84` | Audio route select — **also drives the same `GPIOC13`/`PC2` relay dispatcher as `CMD 0xA0 id=0x11`**, see the [4-state table](#cmd-0x84--cmd-0xa0-id0x11-shared-relay-dispatcher) below | Masked to 4 bits, `≥6` ignored. Value `0` → `"AT+AUDROUTE=1\r\n"` + relay state `0`; value `3` → `"AT+AUDROUTE=2\r\n"` + relay state `1`; `1`/`2`/`4`/`5` → state-only, no relay action. **2 real, unreconciled conflicts**: (1) this session's MCU-side disassembly reads the value from frame `+3` (`payload[1]`), but `custom_ui`'s `handle_audio_route()` reads `payload[0]`; (2) this command's own gate ("proceed if struct offset `0x5e`==0") is the *opposite* polarity of `CMD 0xA0 id=0x11`'s gate ("proceed if its own offset `0x5e`==1") — read from different SRAM struct bases, so whether these are the same flag or two independent ones is unresolved. | ✅ confirmed, ⚠️ 2 unreconciled conflicts |
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
| `0x03` | Vehicle status / HVAC bitfield broadcast | `payload[0]` bits `5`/`6`/`7` confirmed by direct trace as `CirculationMode`/`ACEnable`/`AirConditionEnable`. Bits `0`-`4` and `payload[1]` have been guessed at (door/trunk/handbrake; fan-speed/AC/recirc/defrost) but the `payload[1]` guess is directly contradicted — the confirmed HVAC bits are all in `payload[0]`. | 🟡 partial — real layout spans more of the frame than mapped |
| `0x04` | Parking radar / distance level (`transRadarLevel`) — empirically correlates with reverse gear. See the [reverse-gear conflict](#the-reverse-gear-command-conflict--the-one-that-actually-matters-right-now) section | See the [byte-level `CMD 0x04` decode](#real-byte-level-decode-of-cmd-0x04s-payload-2026-09-02----and-why-this-strengthens-the-case-for-using-it-as-corroboration) further down for the full 4-channel `transRadarLevel()` breakdown | ✅ confirmed |
| `0x05` | Two competing theories, unreconciled (HVAC vs. radar-adjacent) | Theory A (HVAC/climate) shares `CMD 0x03`'s dispatch mechanism but no direct call into any `AirConditionDlg` setter was found. Theory B (radar-adjacent) has real structural evidence — `payload[1]` feeds the same `transRadarLevel()` function `CMD 0x04` uses. Neither is conclusive; `MCUAdapter_BoxP300` is confirmed the active adapter for this hardware, so `0x05` may simply never fire on real Prado hardware rather than being dead code. | ⚠️ conflict, zero live captures either way |
| `0x06` | Vehicle dynamics/safety bitfield — bit *positions* confirmed, *meanings* not | Real dispatch and event (`MsnEvent 0x501A` to app `0x190`) re-verified, but bit meanings are unconfirmed guesses only: bit `4`=Parking Brake, bit `5`=Footbrake, bit `6`=Turn Signals/Hazard, bit `7`=Reverse/Transmission — none disassembly-confirmed. | 🟡 partial |
| `0x07` | Not resolved | — | ❌ unconfirmed — single, never-cross-checked source |
| `0x0A` | Steering angle / reverse trajectory | `byte[3]` bit `0` = direction (confirmed), `byte[4..5]` = 16-bit magnitude scaled to a signed angle (position confirmed, scale factor not cracked) | ✅ confirmed |
| `0x12` | Reverse-gear-adjacent, real payload structure but NOT a reliable reverse-gear signal — see the [reverse-gear conflict](#the-reverse-gear-command-conflict--the-one-that-actually-matters-right-now) section, superseded 2026-09-02 by the [CRITICAL](#critical-2026-09-02-cmd-0x12-confirmed-unreliable-on-real-hardware----fires-from-headlights-alone-no-gear-change) findings further down | `payload[0]` = `0x01` entering / `0x02` exiting; only semantically meaningful in the real vendor app when `payload[1]==0x11` (every captured frame so far has `0x04` or `0x01` instead — a hard no-op per stock software, see the full trace further down) | ⚠️ confirmed unreliable |
| `0x20` | Touch coordinate report | X = `(payload[1]<<8)\|payload[0]`, Y = `(payload[3]<<8)\|payload[2]`, native `0`-`800`/`0`-`480` px, all confirmed. All-zero payload = release, confirmed. `payload[4]` (bit `7`) values `1`/`2` possibly distinguish touch-down vs. move, but `custom_ui` never branches on it and down/move/release all already work correctly via the `touch_pressed_` + coordinate-change state machine — stays unconfirmed, not a real gap. | ✅ confirmed, most rigorously verified finding in the corpus |
| `0x21`/`0x22` | Not resolved — hypothesis: multi-touch/gesture dispatcher | Scanned all 5 known firmware images, no MCU-side hit resembling an outbound frame | ❌ unconfirmed |
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
