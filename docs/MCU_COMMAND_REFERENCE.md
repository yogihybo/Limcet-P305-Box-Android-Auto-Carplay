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

## The command-number scheme (2026-09-03)

Not sequential, but genuinely organized once every confirmed command is laid out by value —
worth knowing before reading the two tables below, since it explains why the numbers look the
way they do.

**Direction correlates strongly with bit `7` (`0x80`).** Every real, confirmed SoC→MCU
command (`0x81`, `0x82`, `0x83` — found only on the `BoxC280` vehicle-adapter variant, not
this product's own `BoxP300` — `0x84`, `0x85`, `0x87`, `0x88`, `0xA0`, `0xE1`, `0xFF`) has the
high bit set (`≥0x80`). Every MCU→SoC command is `<0x80` *except* three: `0x7F` (sits right at
the boundary) and `0xE2`/`0xE4` (the firmware-update handshake replies — a real, deliberate
crossover, since that exchange is a negotiation rather than one-directional telemetry). Not a
perfect rule, but clearly the dominant organizing bit.

**Within each direction, values cluster by function, not by strict sequence:**

| Range | Direction | Function |
|---|---|---|
| `0x00`–`0x06`, `0x0A` | MCU→SoC | Real-time vehicle telemetry (headlights, buttons, HVAC, radar, dynamics, steering) |
| `0x12` | MCU→SoC | Reverse-camera-adjacent (the confirmed-unreliable one) |
| `0x20`–`0x22` | MCU→SoC | Touch/UI input |
| `0x30` | MCU→SoC | Display config |
| `0x40`, `0x60`, `0x7F` | MCU→SoC | Status/telemetry-burst markers, crypto reply, version report |
| `0xE2`, `0xE4` | MCU→SoC | Firmware-update handshake replies |
| `0x81`–`0x88` | SoC→MCU | Basic session control (init, mode, audio, ACK, BT relay, crypto challenge) — **`0x86` is a real, confirmed gap**, never found sent by any adapter variant checked |
| `0xA0` | SoC→MCU | Settings sync — its own nested 18-entry `id` sub-dispatch, not a new top-level command per setting |
| `0xE1` | SoC→MCU | Enter bootloader (pairs with the MCU's own `0xE2`/`0xE4` replies — same "E-family" spanning both directions) |
| `0xFF` | SoC→MCU | Not a system reset (old label, corrected) — its own nested sub-`id` dispatch (`0x00`–`0x09` no-op, `0x7F` queues a real CAN broadcast) |

**Real takeaway**: this reads as a genuinely coherent design — high bit for direction, value
ranges grouped by subsystem, and two commands (`0xA0`, `0xFF`) deliberately using a
second-level sub-ID rather than allocating a new top-level command per setting — not an
ad-hoc numbering. Very likely inherited from a broader shared-firmware convention this same
library reuses across every vehicle-adapter variant it contains (`BoxP100`...`BoxP900`,
`BoxC2xx`, etc.), not something invented specifically for this Prado build.

**Outside corroboration (2026-09-04)**: [`zugetor/simplesoft-canbus-box-reverse-engineer`](https://github.com/zugetor/simplesoft-canbus-box-reverse-engineer)
reverse-engineers a completely unrelated commercial product, the Simplesoft RP5-TY-101
Toyota CAN interface box, and its `decoder.py` shows the **exact same wire protocol shell**
this project's own `hal::McuInputHal` uses: `0x2E` sync byte, `38400` baud,
`[0x2E][func_id][len][data...][checksum]` framing, and the identical one's-complement
checksum algorithm (`~sum(cmd+len+payload) & 0xFF`, written there as
`(sum(packet[1:-1]) & 0xFF) ^ 0xFF` — the same formula). Real, independent evidence for the
"broader shared-firmware convention" theory above, not just this project's own speculation.

**Function IDs themselves don't carry over, though** — every byte value that happens to
overlap between the two products' tables means something different (their `0x20` = SWC key
input vs. our `0x20` = touch coordinates; their `0x82` = A/C settings vs. our `0x82` = app-mode
change; their `0x30` = CAN interface version vs. our `0x30` = display-profile selector), so
each vehicle-adapter variant clearly reassigns the shared ID space per product — nothing here
is safe to borrow directly. One useful negative-evidence note: their variant has a real `0x90`
"Data request" query command (head unit asks, box replies with current state) — genuine proof
the shared base firmware family supports a request/response query mechanism, which explains
*why* no such command exists in our own confirmed-exhaustive 9-entry SoC→MCU dispatch table
(a per-variant compile-time trim, not a gap in the underlying design) rather than suggesting
one was missed.

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
| `0x02` | Knob/button event | `b3=3`/`4` = Next/Prev Track (confirmed, live-tested — NOT volume). `b3=8`/`9` = Answer/Hangup. `b3=12`/`13` = Home/Knob push. `b3=36` = "Mode/Source" (single observation only). `b3=64`/`65` = Knob CCW/CW. The `8`/`9`, `12`/`13`, `64`/`65` pairs each share every bit except bit `0` (upper bits = control group, bit `0` = state/direction) — `3`/`4` don't fit this pattern, inferred not confirmed. **`b3=12` (HOME) `b4` third value confirmed 2026-09-04**: real capture during a HOME long-press showed a run of `payload=[0C 02]` frames (`b4=2`), not just the previously-documented `0`(release)/`1`(press) — almost certainly a held/repeat-tick state while the button stays down. `custom_ui`'s own handler (`mcu_input.cpp`'s `cmd==0x02` branch) only acts on `b4==1` and silently ignores `b4==2`, which is safe/correct (doesn't re-trigger the app-launcher action on every repeat tick) but wasn't a deliberately-designed choice — just happened to fall through harmlessly. | ✅ live-capture confirmed, what `custom_ui` runs on |
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

## SoC → MCU (commands the SoC sends)

The real inbound dispatch table (`0x0800B9E4` in `can_app.bin`) has exactly **9 entries** —
this set is closed and double-checked (cross-referenced between two independent disassembly
passes, 9/9 exact address matches). No command outside this list does anything on the real
MCU firmware.

| Cmd | Meaning | Values / detail | Real sender (stock) | Status |
|---|---|---|---|---|
| `0x81` | Init handshake / keepalive, resets internal state slots | `payload[0]=0x01` fixed | `MCUAdapter_BoxP300::onInited()` — first action at adapter init | ✅ confirmed (`0x080088B5`) |
| `0x82` | App foreground/mode change (`onModeAppChanged`) | Send-side `mode` `2`/`4`/`5`/`7`/`13` append extra byte `8`; `mode=23` appends `0x0A`; else nothing appended. Receive-side, MCU `payload[0]==1` writes state struct `0x20000282` (`offset[0]=1,[1]=4`); else `offset[0]=2,[1]=1` — both call the same internal event-queue function. **RESOLVED (2026-09-02)**, see the [full consumer trace](#cmd-0x82s-real-consumer-2026-09-02----not-an-input-focus-switcher-a-can-bus-mode-announcement) below: the old "input-focus switcher" hypothesis is wrong. The real consumer narrows the 89-site count down to the 2 that actually read the specific offsets `CMD 0x82` writes, and it transmits real outbound CAN messages when the state reaches a specific value -- a mode announcement to the rest of the vehicle's CAN bus, not a local knob/input router. | **3 real, independent senders**: `onModeAppChanged(uint,uint)` itself (the real Qt slot connected to `MsnApplication`'s `modeAppChanged` signal — builds its own frame directly, not via `makeMCUProtocol()`); `showApp(uint)` (caller not resolved — real vtable slot located, matching call pattern not found in either binary, see the dedicated section below); and, one layer up in `MsnCoreApp` itself, `pluginRunningStateChange()` (3 sites) and `onFirstInit()` (1 site) both emit the `modeAppChanged` signal that reaches `onModeAppChanged()` | ✅ confirmed both directions, consumer resolved |
| `0x84` | Audio route select — **also drives the same `GPIOC13`/`PC2` relay dispatcher as `CMD 0xA0 id=0x11`**, see the [4-state table](#cmd-0x84--cmd-0xa0-id0x11-shared-relay-dispatcher) below | Masked to 4 bits, `≥6` ignored. Value `0` → `"AT+AUDROUTE=1\r\n"` + relay state `0`; value `3` → `"AT+AUDROUTE=2\r\n"` + relay state `1`; `1`/`2`/`4`/`5` → state-only, no relay action. **RESOLVED (2026-09-02)**, see the [full `CMD 0x82`/`0x84` gate trace](#cmd-0x82--cmd-0x84-real-mcu-side-trace-2026-09-02----the-shared-gate-conflict-is-settled) below: the "2 unreconciled conflicts" are now 1 resolved + 1 reconfirmed. Read-byte conflict (`payload[1]` vs `payload[0]`) reconfirmed as a real, consistent frame-struct convention (offset `+2`=`payload[0]`, `+3`=`payload[1]`), not an actual disagreement. **Gate-polarity "conflict" is resolved**: both `CMD 0x84` and `CMD 0xA0 id=0x11` read/write the exact same absolute SRAM byte (`0x20000236`) for their offset-`0x5e` check — genuinely the same flag, opposite required polarity by design (arm-then-trigger vs. fire-when-not-already-armed), not two independent flags. | **FOUND (2026-09-02): confirmed hard-stubbed dead on this product, not missing evidence.** See the [`onRecvAppProtocol` dead-stub finding](#cmd-0x840x85s-real-provenance-found-2026-09-02----confirmed-hard-stubbed-dead-on-this-exact-product-not-missing-evidence) below | ✅ confirmed, gate-sharing conflict resolved |
| `0x85` | App-protocol ACK | `payload[0..2]` stored into an internal queue slot | Same dead-stub finding as `0x84` — see the section below | ✅ confirmed shape / ❌ clean-room reply content is an approximation |
| `0x87` | Bluetooth AT-command relay, verbatim passthrough to `USART3` (`PB10`/`PB11`) | Real bug: `AT+PIN=0000`'s digit-substitution is unwired, very likely always sent malformed | **FOUND (2026-09-02): real evidence this command is never actually used by stock software at all.** `libBlueTooth.so` (`BlueToothAdapter_Blueware::writeCommand()` and siblings) writes AT commands via its **own dedicated `ProtocolUtils`/`MsnSerialPort` instance** (`MsnSerialPortManager::addSerialPort()`), calling `MsnSerialPort::write()` directly with the raw AT-command text — **no `[0x2E][0x87][len]...[checksum]` framing at all**. See the [full trace](#cmd-0x87s-real-provenance-found-2026-09-02----bluetooth-never-goes-through-the-mcu-relay-at-all) below. | ✅ confirmed (`0x080087A1`) |
| `0x88` | TEA-cipher anti-clone challenge, 8-byte block | Real cipher, genuine key, decrypted reply read by nothing (inert as shipped). **`onHeartBeatTimer()`'s real cadence traced 2026-09-04** (direct ARM disassembly of `MsnCoreApp`, not stripped -- real symbols): the underlying `QTimer` fires every **3000ms**, confirmed via the literal `movw r4, #3000` compared against the timer's own stored interval at function entry (re-armed to 3000 if not already). But `CMD 0x88` is only actually sent on the **first 30 of those ticks** (~90s total, gated by a byte counter at struct offset `0x128` counting 0→29) -- once the counter passes 29, the *same* timer switches to a completely different job (showing a "Demo Soft Tips" popup via `MsnWindowManager`) and stops sending `CMD 0x88` on this path. So stock's real behavior is a **bounded ~90s startup burst at 3s intervals, not an ongoing keepalive** -- the name `onHeartBeatTimer` overstates what it actually does past the first 90 seconds. | `MsnCoreApp::sendEncryptDatas()` — called from `onEndInit()` (real init) and `onHeartBeatTimer()` (bounded periodic re-send, see above). The one MCU command `MsnCoreApp` sends directly, bypassing the adapter classes entirely | ✅ fully confirmed |

**`custom_ui`'s own use (2026-09-04, `hal::McuInputHal::run()`)**: unlike stock, `custom_ui` needed a real *ongoing* link-liveness check -- `reverse_gear_`/`night_mode_`/knob/touch input all depend entirely on this one UART link (see this doc's `CMD 0x01` bit-2 reverse-gear row above), and there was previously no way to notice a silently dead link (cable fault, MCU hang) short of a hard `read()` error. Since stock's own `onHeartBeatTimer()` cadence turned out to be a bounded burst rather than a real keepalive pattern to borrow, `custom_ui` picked its own interval: sends `CMD 0x88` (fixed dummy 8-byte payload) every **5s**, indefinitely.

**REAL HARDWARE RESULT (2026-09-04, same day): the probe never got a reply, and the staleness-triggered reconnect this was originally paired with was removed.** Two real captures: the probe's own send confirmed genuinely firing on schedule (a dedicated log line fires every 5s exactly, `write()` never errors) -- and across both captures, **zero `CMD 0x60`/`0x88` reply frames ever arrived**, contrary to what both candidate MCU firmware sources (the real vendor disassembly in this section, and this project's own clean-room `hardware/MCU/source/`) suggested should happen. Meanwhile a completely healthy link legitimately went 15+ seconds with zero frames of any kind during real idle (no headlights/knob/gear activity) -- so "no frame recently" was never a valid dead-link signal on this hardware, and treating it as one just repeatedly tore down a working connection every ~15s. The reconnect-on-staleness branch was removed; `custom_ui` now only reconnects on a genuine `read()` error/EOF (unambiguous), and the probe keeps sending (harmless, still no known effect) purely as a diagnostic in case a future firmware or condition ever does answer it. `McuInputHal::is_link_alive()` is now purely informational (a rough, honest, NOT hardware-confirmed ~60s guess at "clearly wrong," nothing wired to act on it) rather than reconnect-triggering.

**Open, unexplained**: *why* neither candidate MCU firmware's own documented/coded `CMD 0x88` handling actually produces a reply on this real hardware is still unresolved -- worth a dedicated look (is `hardware/MCU/source/` actually what's flashed on this unit right now? does its `SOC_CMD_CRYPTO_CHALLENGE` dispatch entry actually get reached in practice?) if this is ever revisited, but not chased further this session once the disruptive reconnect behavior was fixed.
| `0xA0` | UI settings sync — see the [dedicated sub-table](#cmd-0xa0-sub-table-settings-sync-by-id) below | — | `MCUAdapter_BoxP300::syncSettingDataToMcu(int)`, triggered by real user settings-screen interaction — the cleanest 1:1 architectural match to `custom_ui`'s own `sync_setting()` | ✅ confirmed |
| `0xE1` | Enter bootloader for update | **Real erase risk**: resident bootloader erases flash before waiting for the first byte, no recovery if nothing follows. `tools/mcu-probe --reboot-probe` gated behind `--confirm-erase-risk` | `MCUAdapter_BoxP300::onSendUpdateReadyTimer()` — a real timer callback, implying this is only armed during an actual firmware-update flow rather than firing ambiently | ✅ confirmed |
| `0xFF` | Sub-id dispatch, `0x7F` queues a real CAN broadcast — **not a system reset**, see detail | **Traced further (2026-09-03)**: handler (`0x080088E8`) reads `payload[0]` as a sub-id via `tbb`/explicit compares against `0`-`9` and `0x7F`. Sub-ids `0`-`9` all branch to a bare `pop{r4,pc}` -- confirmed hard no-ops, not "unimplemented," genuinely nothing happens. Only `0x7F` differs: it calls the MCU's own real CAN-message-queuing function (`0x080062FC`) with `type=12` -- the exact same mechanism (81-byte-stride SRAM table at `0x2000081E`) already documented for the firmware's periodic `CAN_Transmit()` broadcaster's "15 SRAM-resident message templates." **So `0xFF` `payload[0]==0x7F` doesn't reset anything -- it queues CAN message template slot `12` for transmission on the vehicle bus**, the same real infrastructure this doc's `CMD 0x82` consumer trace already found (which uses `type=5`/`6`). The immediately-adjacent handler in memory (`0x0800893C`, right after this one) does the same `type=13` queue call after an 8-byte copy + 2 calls to what look like real crypto routines (`0x08005124`/`0x080050A0`) -- very likely the real `CMD 0x88` TEA-challenge handler, sitting next to `0xFF` in the binary layout; not chased further, noted for context. The old "System state reset... clears CAN buffers" label (from an archived, less-rigorously-checked doc) doesn't match what this handler actually does -- corrected here. | Sender search broadened beyond `BoxP300` to every other `MCUAdapter_*` variant, then to `libCanBus.so` -- that whole avenue is now closed, not open: its factory function confirmed returns `NULL` for the real product config's `CanType=0`, so it never runs on this product at all. See the [`custom_ui` vs. stock comparison](#custom_ui-vs-stock----direct-send-side-comparison-2026-09-02) table below | ✅ confirmed: `0x00`-`0x09` no-op, `0x7F` queues real CAN template slot `12` |

### `CMD 0xA0` sub-table (settings sync, by `id`)

Real, closed, disassembly-confirmed range: `id` `0x00`–`0x11` (18 entries, real TBB jump
table). Anything outside this range (e.g. a separate report's claimed `id=0x87`) falls
outside the confirmed dispatch table and is very unlikely to do anything real.

| id | Vendor label | Values | Real MCU-side effect | `custom_ui` |
|---|---|---|---|---|
| `0x00` | Camera Type | `0`=AfterMarket / `1`=Factory / `2`=AfterMarket 360 / `3`=Factory 360 | `GPIOB Pin 1`. **Hardware-confirmed: controls the OEM camera relay.** (Value `2` is a distinct `AT+UPGRADE` trigger, not a camera value) | ✅ sent by `sync_video_relay()` |
| `0x01` | — (no real label) | — (no real values) | Dead no-op — shares the same handler as `0x02`–`0x06`/`0x0E` | not wired |
| `0x02`–`0x06` | — | — | Dead no-op (same shared handler) | not wired |
| `0x07` | "Radar" | — | **Re-confirmed (2026-09-02)**: write-only, no consumer. Writes struct offset `0x3a` (`0x8008a4a`/`0x8008a54`); a precise, code-only sweep of the entire binary for any real `ldrb`/`ldrb.w` *read* of that offset found zero hits | not wired |
| `0x08` | "Trajectory" | Off/On | **Re-confirmed (2026-09-02)**: write-only, no consumer. Writes offset `0x39`, same zero-hit sweep result | not wired |
| `0x09` | "Reversing mode" *(vendor mislabel)* | Off/On | Mic/audio input mux, `GPIOB Pin 6`. Confirmed byte-identical across 5 real firmware images | ✅ "OEM Factory Microphone" |
| `0x0A` | "360 camera" | CAN Active/12V Active/P Key Active | **Re-confirmed (2026-09-02)**: write-only, no consumer. Writes offset `0x3c`, same zero-hit sweep result -- see the note below on a real near-miss caught during this re-check | not wired |
| `0x0B` | "Front camera" | empty/dynamic | `PA15`/`PB8`/`PB9` 3-pin enable when cleared to `0` | not wired |
| `0x0C` | "Front camera time" | Off/Radar Active/5s/10s/15s | Real reader, thresholds `0x0B`'s 3-pin group | not wired |
| `0x0D` | "Speech button" | 5s/10s/15s | **Re-confirmed (2026-09-02)**: write-only, no consumer. Writes offset `0x42` -- checked directly against the real `PA15`/`PB8`/`PB9` relay function (`0x08005D30`, the same one `id=0x0F`/`id=0x10` feed) since it's structurally adjacent to those offsets (`0x43`/`0x44`); that function reads `0x43`/`0x44`/`0x3d`/`0x40`/`0x4c`/`0x19`/`0x5f` but never `0x42` -- genuinely not part of that subsystem either | not wired |
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

**Methodology note on the "write-only, no consumer" re-checks above (2026-09-02)**: a first,
cruder pass (grepping for the struct offset anywhere nearby in the disassembly) produced
false-positive "consumer" hits for `id=0x07`/`id=0x0A` at addresses just past the real
`0x0800B9E4` command-dispatch table (`0x0800BA0E`-`0x0800BA86`) — that region is raw constant
*data* (the tail of the dispatch table plus alignment/padding before the next real data
table), which `objdump`'s linear disassembly happily decodes into plausible-looking but
meaningless instructions. Caught before being reported, by checking the hit addresses against
the already-known dispatch-table range rather than trusting the grep. The re-confirmed
findings above used a precise, code-only sweep instead (every real `ldrb`/`ldrb.w` *read*
instruction in the binary, matched against each offset) — a stronger negative result than the
original claim had, not just a repeat of it.

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

## The reverse-gear command conflict — the one that actually matters right now

Real, current status of every candidate reverse-gear signal this project has traced. **The
OEM camera path no longer depends on any of these** (see the "RESOLVED" architecture section
further down) — this table now matters only for the Aftermarket camera path, volume cut, and
the AA-resume nudge, which key off `reverse_gear_` (`custom_ui/src/hal/mcu_input.h`), itself
now driven by `CMD 0x01` bit `2` as of 2026-09-03 -- see that row below, not `CMD 0x12`
directly (demoted to a logged-only cross-check, see its own row).

| Candidate | Real status | Currently used for |
|---|---|---|
| `CMD 0x04` presence | Confirmed **not** reverse-gear-related — real parking radar telemetry, correlates with reversing by coincidence (sensors activate around the same time) | Not used (demoted to a no-op) |
| `CMD 0x12` presence, direction-blind (old model) | Superseded — `CMD 0x12` carries both directions in its own payload, this model was wrong | superseded |
| `CMD 0x12` `payload[0]` direction (`0x01`=entering, `0x02`=exiting) | Tracks *real* reverse-gear transitions reliably when tested directly — but confirmed **not exclusive** to reverse gear: fires from a plain headlights toggle too (hardware-confirmed twice, ~29ms after the headlights frame, zero gear involvement). **Second, distinct false-trigger source confirmed 2026-09-04**: long-pressing the physical HOME button to switch from the OEM factory screen to `custom_ui` also fires a spurious `CMD 0x12` -- real capture shows it twice in one session, once ~1.4s after a `CMD 0x02 sub=0x0C` HOME long-press event, once in the middle of a run of `CMD 0x02 sub=0x0C payload=[0C 02]` frames (a newly-observed third payload value for that button code, `0x02` -- previously only `0x00`/`0x01` were documented; almost certainly a held/repeat-tick state distinct from the press/release edges, not yet formally added to the `CMD 0x02` row above). Neither false trigger corresponded to any real gear change. Also confirmed a hard no-op in the real vendor app for every payload captured so far. **Real unifying theory, user-spotted from a fresh capture, 2026-09-04**: `payload[0]` was never actually the *direction* of whatever triggered the frame -- the same real action produced different payloads on different occasions (headlights turning ON produced `[01,04,00]` once, then `[02,01,00]` a second time in the same session; headlights ON and OFF both produced `[01,04,00]` back to back). What stays constant is that `[01,04,00]`/`[02,01,00]` are always emitted as fixed pairs, never mixed-and-matched. Best-supported model now: `CMD 0x12` broadcasts the MCU's own current internal mode/state-flag byte, toggling for its own independent reason, whenever ANY of several unrelated events (headlights change, a button press, a real gear change) causes the firmware to re-evaluate and re-emit it -- not a signal that encodes anything about the triggering event itself. This unifies every false-trigger source found so far (headlights alone, HOME long-press) under one real mechanism rather than a growing list of unrelated special cases, though the exact real "mode" this flag reflects is still unconfirmed. **Third trigger context confirmed 2026-09-04, user-corrected same day**: a `CMD 0x12` (`payload=[02 01 00]`, exiting-shape) fired while the device was in the OEM factory LCD mode, and a follow-up capture caught one firing while in `custom_ui` mode too. **User correction**: this is NOT independent of display mode -- it IS tied to switching points specifically. The `custom_ui`-mode firing coincided with a real switching moment (not "no transition happening," as this doc briefly and wrongly stated). Real, corrected model: `CMD 0x12` fires at genuine transition/event moments -- a factory/`custom_ui` display-mode switch, a headlights change, or a real reverse-gear event -- consistent with the broader "MCU re-emits its current internal mode/state-flag whenever a real tracked transition happens" theory, just narrower and more precise than "any unrelated event": it's specifically switching-type transitions across these few tracked categories, not arbitrary MCU activity. **REAL UNDERSTANDING, 2026-09-04, user-identified, supersedes the "mode/state flag" theory above**: `payload[0]` isn't a generic internal flag -- it directly states which LCD source is now active. This explains every false-trigger source found in this row in one shot: a real reverse-gear engage genuinely switches the LCD source (OEM camera relay) and disengage switches back -- but so does a HOME-button long-press (user-confirmed: switches factory↔`custom_ui` screens directly) and a plain headlights toggle. All three are real LCD-source switches from the MCU's own point of view; this command just reports which one is now active, regardless of what caused the switch. Not a reverse-gear signal that happens to misfire -- a display-source signal that happens to correlate with reverse gear whenever gear-triggered switching is what caused it. **Mapping corrected same day (was initially recorded backwards)**: `0x01` = Aftermarket LCD mode (`custom_ui` feed active), `0x02` = Factory LCD mode (OEM feed active). | **DEMOTED 2026-09-03** — no longer sets `reverse_gear_` (see the `CMD 0x01` bit `2` row below, now the primary source). Cross-check logging **widened 2026-09-04**: full audit of every `CMD 0x12` event across 3 real captures found the original disagreement-only logging silently skewed what got printed -- every firing in those captures was a false trigger, but only `payload[0]=0x01` ("engaging-shape") ones ever disagreed with the resting `reverse_gear_=false` and got logged; `payload[0]=0x02` ("exiting-shape") false triggers coincidentally *matched* the resting false state and were completely invisible, not because they were less spurious. Now logs every non-grace-window `CMD 0x12` event's relationship to `reverse_gear_` (agreement included), not just disagreements. Volume cut and the AA-resume nudge in `main.cpp` key off the resulting `reverse_gear_` value, not off `CMD 0x12` directly. **Real hardware validation of the demotion itself (2026-09-04)**: every false trigger observed across all 3 captures was correctly logged/ignored and never touched `reverse_gear_`, which stayed `false` throughout -- confirms this demotion was the right call; had `CMD 0x12` still been the state-setter, several of these would have wrongly flashed the reverse-camera screen during completely unrelated actions (headlights, HOME button). **Log line rewritten 2026-09-04** to match the corrected LCD-source understanding above -- no longer prints "matches"/"disagrees with `reverse_gear_`" (that comparison was a leftover from the superseded reverse-gear-direction theory, not meaningful once `payload[0]` is understood as an LCD-source report). A brief intermediate version also added event-correlation labels (nearest headlights/reverse-gear/HOME-button event within a 3s window) -- **removed the same day, per explicit request**: `CMD 0x12` is simply an LCD-source status report, not an anomaly that needs explaining against other events, so no correlation tracking belongs in this log line at all. **Final format**: `CMD 0x12 payload[0]=0x%02X -> Aftermarket LCD mode` / `-> Factory LCD mode -- logged only, not acted on` (mapping per the corrected direction above), nothing else. |
| `CMD 0x01` bit `2` (`payload[0]=0x15`) | **PRIMARY SOURCE as of 2026-09-03** — supersedes the "hardware-confirmed pulse" conclusion recorded below, which turned out to rest on insufficient evidence. **Original (2026-09-03, since overturned) reasoning, kept for the record**: real disassembly convergence with `CMD 0x12`'s one meaningful case (same `MsnEvent 0x5026`, itself confirmed to have zero consumers). 2 earlier live captures showed it firing 150-214ms *before* genuine `CMD 0x04` radar activity. A fresh `mcu-handshake` capture then directly confirmed it via a real, user-narrated test in one session: `CMD 0x01`'s two sub-fields fired *independently* and correctly for two different real actions -- bit `1` (`0x11`↔`0x13`) flipped while toggling headlights, then later in the same capture bit `2` (`0x11`↔`0x15`, bit `1` staying clear) flipped while repeatedly engaging/disengaging real reverse gear. Real, transient/edge behavior observed -- `0x15` appears once per transition then reverts to `0x11`, not a held "currently reversing" state, similar in shape to `CMD 0x12`'s own pulse behavior. A dedicated isolated-variable follow-up test (only reverse-gear cycling, nothing else) then confirmed: **2/2 real (non-startup-artifact) `CMD 0x12 sub=0x01` events were each directly paired with a bit-2 pulse** (the one unpaired `CMD 0x12` in that capture matched the already-known startup-telemetry-burst false positive exactly, correctly excluded); and **the pulse fires symmetrically on both directions** -- the same `0x15`→`0x11` pattern for entering and exiting alike, confirmed via a direct enter/exit comparison. So bit `2` carries no direction of its own (unlike `CMD 0x12`'s `payload[0]`) -- it's a real, hardware-confirmed *"a transition just happened"* signal, not a directional one. Two more single-line captures ("in reverse gear: sub=0x15" / "out of reverse gear: sub=0x11") briefly raised the possibility this is actually a **held** state rather than a pulse, but the user clarified both were captured at the moment of the gear change, not sampled repeatedly while stationary in each state -- equally consistent with a pulse, doesn't move the model. Real confirmation either way needs a capture watching `CMD 0x01` across several consecutive frames while sitting in reverse. **CORRECTION, same day, a few hours later**: that follow-up capture happened. A real hardware test confirmed exactly one `CMD 0x01` frame arrived on entering reverse (`payload[0]=0x15`), and critically, **no further `CMD 0x01` frame arrived while still sitting in reverse** -- no spontaneous mid-reverse reversion, the one thing a real pulse would produce. The bit only cleared (`payload[0]=0x11`) on the frame marking the real exit. That's the decisive data point the earlier test lacked, and it points the other way: this is a real, held, level-encoded field, transmitted on-change only, the same convention as bit `1`/headlights -- not a pulse. The earlier "2/2 paired with a bit-2 pulse, symmetric both directions" finding is now understood differently: a level re-sent on every real transition (both entering and exiting) *also* produces exactly one frame per direction, which is what was actually observed then too -- the "pulse" framing was the wrong model for the same real data, not a separate contradicting data point. Also directly answers the original motivating question in this file (can `custom_ui` learn the real current reverse-gear state at load, e.g. if the vehicle is already in reverse when it starts): yes -- this same frame is confirmed to arrive unprompted right after connecting with real current bit `1`/bit `2` values (`mcu-handshake` capture, real screenshot), so a boot-time seed comes for free, same as headlights already gets via `first_light`. **CONFIRMED CLEAR (2026-09-04)**: the one remaining open item -- whether bit `2` shares `CMD 0x12`'s own confirmed weakness of false-triggering from a plain headlights toggle alone (vehicle in Park, no gear change) -- is now settled. User confirmed directly testing exactly that scenario against the earlier `mcu-handshake` capture: headlights-only toggling with the vehicle in Park, watching `CMD 0x01`, reliable every time -- bit `2` never flipped without a real gear change. Real, deliberate isolated-variable test, not just an absence of noticed problems in general use. | **Wired as the primary `reverse_gear_` source** (`custom_ui/src/hal/mcu_input.cpp`, 2026-09-03) -- same `first_reverse`-seeded level-tracking pattern as bit `1`/`night_mode_`. `CMD 0x12` demoted to a logged-only cross-check (see its own row above). Headlights-alone false-trigger risk (`CMD 0x12`'s own known weakness) confirmed 2026-09-04 NOT shared by bit `2` -- see the confirmation note in the left column. No open items remaining on this row. |
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
command from the SoC.

**Follow-up chased and closed (2026-09-02, same day)**: traced `PA8`/`PC9`/`PC8`/`PC7`'s real
consumers in `can_app.bin` (full detail, including a same-day correction to how the `PC8`/
`PC7` consumer was first characterized, in `MCU_FIRMWARE_VERIFIED_FINDINGS.md`). They are
**not** candidates for `CMD 0x06`'s or any other command's vehicle-dynamics bits after all --
`PA8`/`PC9` select between 3 internal CAN-message-ID dispatch tables, and `PC8`/`PC7` feed a
real (not inert) `CMD 0x30` "Arkdata display-profile selector" send, using the exact one
sub-type value the SoC-side dispatch treats as meaningful. Real behavior reads as a hardware
board-variant/configuration selector (consistent with this firmware's multi-vehicle-brand
build convention), not a live sensor input -- corrected here rather than left as an open lead
that turned out to be a dead end.

**`CMD 0x82`'s own handler (`0x08008bd4`) re-confirmed independently**: reads `payload[0]`
from the same `0x20000238`-based "current frame" struct at offset `+2` (consistent with the
`+3`=`payload[1]` finding above -- same struct, same convention, cross-validating both).
`payload[0]==1` writes `{offset[0]=1, offset[1]=4}` into struct `0x20000282`; else
`{offset[0]=2, offset[1]=1}` -- both branches call the same event-queue function
(`0x08006228`) with identical arguments, matching the existing doc entry exactly. The
already-documented "who consumes `0x20000282`" question (89 real load sites) was not
re-attempted this pass -- still the real remaining gap for this command.

## `CMD 0x82`'s real consumer (2026-09-02) -- not an input-focus switcher; one path is a real MCU->SoC `CMD 0x12` sender, the other is genuine CAN telemetry

The "89 real load sites, too broad to trace fully" excuse this doc carried for a while turned
out to be solvable by narrowing the search, not by reading all 89: `CMD 0x82`'s receive-side
handler only ever writes struct `0x20000282`'s **offset `0` and offset `1`** -- so the real
question is just "which of the 89 sites read *those two specific offsets*," not "what does
every site referencing this struct do." A precise sweep (every real `ldrb`/`ldrb.w` read of
offset `0` or offset `1` specifically, not the struct's other fields) found exactly **2** real
sites.

**CORRECTION (2026-09-02, same day): Site 1 is a real MCU->SoC `CMD 0x12` sender over
`ttyHS0`, not a generic "over the bus" readback -- this is a significant, load-bearing
finding, not a footnote.** Re-derived `0x08007e0c`'s exact calling convention mechanically
(its own prologue: `mov r7,r0` (payload pointer), `mov r5,r1`, `mov r9,r2`; then it writes
`[sync=0x2E][r9][r5][payload, r5 bytes][checksum]` -- so the caller's second argument is
`len`, third is `cmd`) rather than trusting an earlier, backwards guess. At the real call site
(`0x0800917a`): `movs r2,#18; movs r1,#3` → **`cmd=18` (`0x12`), `len=3`** -- a real `CMD 0x12`
frame, exactly matching this doc's own `[0x2E][cmd][len][payload][checksum]` format and every
real `CMD 0x12` capture's length (3 bytes) this whole project has ever gotten.

- If the incoming query's own sub-byte is `0`: sends `CMD 0x12` payload `[1, 17, 0]` --
  `payload[1]=17` (`0x11`) is a literal, hardcoded value. **This is the exact one case
  `libMcuCenter.so`'s real dispatch (`0x38144`, traced earlier in this doc) treats as
  semantically meaningful** -- the only value that posts the real `MsnEvent 0x5026`.
- Otherwise: sends `CMD 0x12` payload `[1-or-2, X, 0]`, where `X` is **`CMD 0x82`'s own raw
  state byte** -- literally whatever the receive-side handler most recently wrote (`4` when
  the SoC sent `payload[0]==1`, `1` otherwise).

**Why this matters well beyond just closing the consumer question**: every real `CMD 0x12`
capture this whole project has ever gotten has `payload[1]` of `0x04` or `0x01` -- exactly the
two values `CMD 0x82`'s state field can hold. This function is a strong, concrete candidate
for where every one of those captures actually originated -- not a dedicated reverse-gear
signal at all, but this app-mode-state echo mechanism riding on `CMD 0x12`'s frame format. If
`MsnCoreApp` sends `CMD 0x82` during ordinary app/UI-state transitions (plausibly including
whatever accompanies a headlights-driven night-mode switch), this would explain the
already-confirmed real headlights-alone false trigger documented earlier in this doc. **Not
yet proven end-to-end** -- would need confirming `MsnCoreApp` actually sends `CMD 0x82` around
a headlights event, which is outside what `can_app.bin` alone can show.

**Site 1's real trigger mechanism, traced as far as time allowed**: it's one entry (item id
`4`) in a real priority-based event scheduler (`0x0800B8A0`), which reads a "pending flags"
byte pair at SRAM `0x20001365+5`/`+6`, picks one set bit by fixed priority order, and queues
`(type=4, itemId)` via `0x08005B90` into a type-indexed dispatch table (`~0x0800BA2C`, our
handler at index `4`). Item id `4` corresponds to **bit `1`** of the flags byte at `+5`. **Not
chased to the end**: which specific event actually sets that bit -- searched the 3 other real
users of this struct (all inside dense CAN-message-ID-matching code, `cmp r5,#0xD3`/`#0x50`/
`#0xD6`-style real arbitration-ID checks) without finding the exact setter in the time spent.
Real, bounded follow-up if this is ever worth finishing.

**Site 2 (`0x080093d0`) remains genuine CAN telemetry, unaffected by the Site-1 correction
above** -- it calls a distinctly different function (`0x08004684`, a retry-loop wrapping real
CAN-mailbox-availability checks at `0x08006ba4` and a real transmit call at `0x08006c38`, not
`0x08007e0c`'s UART frame format at all). Gated on offset `0 != 2` and offset `1 == 4`
(matching `payload[0]==1`'s written state), it calls that CAN transmit path twice, with
message type `6`/state `0` then type `5`/state `2`. Real, honest gap left open: the exact
real-world meaning of message types `5`/`6` is outside what `can_app.bin` alone can answer --
would need the receiving ECU's own firmware or a real CAN-bus capture.

**This settles the old hypothesis: `CMD 0x82` is not an input-focus switcher.** The
"factory/OEM vs. app mode, which subsystem gets knob ticks" guess (already flagged as
unconfirmed, and the separate report's stronger "routes rotary knob event target and CAN
arbitration priority" claim already rejected as overstating the evidence) doesn't hold up --
the real, traced consequences are a `CMD 0x12` echo back to the SoC (Site 1) and genuine
outbound CAN telemetry to another ECU (Site 2), neither of which reroutes local input focus.

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

## Where the real stock software actually issues MCU commands from (2026-09-02)

Prompted directly by a real question: does `MsnCoreApp` send MCU commands at init, and from
where? Traced by searching the real, unstripped `MsnCoreApp` binary for every call site into
the two real MCU-facing entry points it imports (`MsnApplication::sendMsgToMcuCenter()` and
`MsnLink::setMCUUUID()`), plus tracing `libMcuCenter.so`'s own `MCUAdapter_BoxP300::
onInited()` directly. The real, complete picture is more layered than "the app sends
commands":

**`MsnCoreApp` itself barely touches the MCU protocol directly.** Its only import of
`sendMsgToMcuCenter()` has exactly **one call site** in the whole binary, inside
`MsnCoreApp::sendEncryptDatas()` -- the real TEA-cipher anti-clone challenge (`CMD 0x88`,
already fully traced elsewhere in this doc). That function itself has exactly 2 real callers:
**`MsnCoreApp::onEndInit()`** (a real init-completion hook -- so yes, `CMD 0x88` genuinely
fires at init) and **`MsnCoreApp::onHeartBeatTimer()`** (so it's also re-sent periodically,
not just once at startup).

**Everything else is issued from *inside* `libMcuCenter.so`'s `MCUAdapter_BoxP300` itself,
wired via Qt signals -- not direct calls `MsnCoreApp`'s own code makes.** Traced
`MCUAdapter_BoxP300::onInited()` (`0x34f50`) directly:

1. **First thing it does**: calls `makeMCUProtocol()` with `cmd=0x81` (`mov r2,#129` right
   before the call, unambiguous) -- **`CMD 0x81`, the init handshake, is confirmed sent as
   literally the first MCU-facing action this adapter takes.**
2. Then wires up 2 real `QObject::connect()` calls to `MsnApplication::instance()`, with real,
   readable signal/slot strings recovered directly from the binary's own string pool:
   - `SIGNAL(modeAppChanged(uint,uint))` → `SLOT(onModeAppChanged(uint,uint))` -- **this is
     the real trigger for `CMD 0x82`**, fired later at runtime whenever `MsnApplication`
     emits this signal, not at init itself.
   - `SIGNAL(diskDeviceStatusChange(int,int))` → `SLOT(onDiskStatusChange(int,int))` --
     unrelated to the MCU protocol (disk/USB device status).

**Traced who actually emits `modeAppChanged` back in `MsnCoreApp`** (4 real call sites into
the signal's emit stub, `MsnApplication::modeAppChanged(uint,uint)@plt`):

- **3 sites inside `MsnCoreApp::pluginRunningStateChange(uint, int)`** -- different branches
  of this one function, very likely covering distinct plugin/app-module lifecycle transitions
  (start/stop/switch). This is the real, concrete, **runtime** trigger for `CMD 0x82` --
  every time a "plugin" (this vendor architecture's term for an app module -- radio, media,
  Bluetooth, AA/CarPlay, navigation, etc.) changes running state, `CMD 0x82` fires.
- **1 site inside `MsnCoreApp::onFirstInit()`** -- so `CMD 0x82` is *also* sent once at real
  first-init, separately from `CMD 0x81`/`CMD 0x88`'s own init hooks.

**Real, honest connection to the still-open `CMD 0x12`/headlights false-trigger question**:
this doc already found (see the `CMD 0x82` consumer section above) that `CMD 0x82`'s written
state can produce a real MCU->SoC `CMD 0x12` echo with `payload[1]` matching every real
false-positive capture's byte pattern. This pass adds the missing piece on the *sending* side
-- `CMD 0x82` fires from `pluginRunningStateChange()`, a genuine runtime event, not just
init -- but **whether a headlights-driven UI change specifically routes through
`pluginRunningStateChange()`** wasn't traced this pass (would mean finding what calls *that*
function, a real next step, not yet done).

**Summary answer to "is it first init"**: partially. `CMD 0x81` (adapter init) and `CMD 0x82`
(via `onFirstInit()`) are both genuinely sent at init. `CMD 0x88` (anti-clone) is sent at init
*and* periodically via heartbeat. But `CMD 0x82` is also a live, runtime-triggered command
(plugin state changes) -- not an init-only event -- and the bulk of the remaining MCU->SoC
command traffic (`CMD 0x84`/`CMD 0xA0`/etc.) isn't issued from `MsnCoreApp`'s own code at all,
it's internal to `MCUAdapter_BoxP300` itself, reacting to the same Qt signal infrastructure.

## Every real `makeMCUProtocol()` call site inside `MCUAdapter_BoxP300` (2026-09-02)

Following up directly: found every real call into `makeMCUProtocol()` (the actual frame
builder every outbound command goes through) within the confirmed-active `MCUAdapter_BoxP300`
class -- **6 real sites**, not just the `CMD 0x81`/`onInited()` one already traced:

| Call site (enclosing function) | `cmd` sent | What it is |
|---|---|---|
| `showApp(unsigned int)` (`0x31f2c`) | `0x82` | **A second, independent trigger for `CMD 0x82`**, distinct from the `pluginRunningStateChange()`/`onFirstInit()` paths already traced above |
| `syncSettingDataToMcu(int)` (`0x34a78`) | `0xA0` | Confirmed, matches this doc's existing settings-sync finding |
| `onInited()` (`0x34f50`) | `0x81` | Already traced above -- the real init handshake |
| `onSendUpdateReadyTimer()` (`0x36598`) | `0xE1` | **Real: bootloader-entry is sent from a timer callback**, not on-demand from user action -- consistent with a real firmware-update-flow gate (presumably only armed during an actual update sequence, not fired ambiently) |
| Inside `onRecvMcuProtocol()`'s own `CMD 0xE2` handler body (`0x37d14`) | `0x81` | A real reply-send: receiving the firmware-update-handshake command (`CMD 0xE2`, "End Update Mcu!") makes the adapter re-send the init handshake -- plausibly a post-update re-sync |
| Inside `onRecvMcuProtocol()`'s own `CMD 0x7F` handler body (`0x398d4`) | `0xE3` | **A real send of `CMD 0xE3`, a value not in this doc's own closed 9-entry SoC->MCU dispatch table.** Since that closed set was independently verified exhaustive earlier in this doc (`0x81 0x82 0xA0 0xFF 0xE1 0x85 0x84 0x87 0x88`, confirmed via the real dispatch loop in `can_app.bin`), a genuine `CMD 0xE3` send from the SoC is a hard no-op on the real MCU firmware -- interesting (the app really does send it, after receiving the MCU's version-report frame) but not actionable. |

**`showApp()`'s own caller, chased via real vtable analysis (2026-09-02) -- real limitation
hit, no reliable answer found.** It has zero direct `bl` call sites anywhere in either
`MsnCoreApp` or `libMcuCenter.so`, consistent with it being a **virtual method** on the shared
`MCUAdapter` base-class interface (matching this whole class family's naming convention --
`MCUAdapter_BoxP100`, `_BoxP210`, etc. all likely share it). Located `showApp()`'s real vtable
slot precisely, not guessed: `.rel.dyn`'s own `R_ARM_ABS32` relocation for
`MCUAdapter_BoxP300`'s vtable (`0x000BD930`) shows offset `0x58` (slot `22`) resolves to
`0x00031F2C` -- `showApp()` itself.

Searched both binaries for the matching real call pattern (`ldr r3,[r_,#88/0x58]` immediately
followed by `blx r3`, the same real technique that resolved `CMD 0x21`/`0x22`'s dispatch
earlier in this doc) -- exactly **one** hit, in `MsnCoreApp` at `0x51334`, inside
`CalibrateDialog::onReadyReadStandardOutput()`. **This is almost certainly a false positive,
not the real answer** -- a touch-calibration dialog reading a subprocess's stdout has no
plausible reason to call into an MCU-protocol adapter, and vtable slot `0x58` is just a
byte-offset coincidence shared by an unrelated class hierarchy's own, differently-typed
virtual function. Reported here as a real limitation honestly reached, not as a finding.

**Real, more promising lead not yet chased**: neither `MsnCoreApp` nor `libMcuCenter.so`
imports `MsnLink` at all (the class whose `setMCUUUID()` method `MsnCoreApp` *does* import,
suggesting `MsnLink` is the real class that owns the active `MCUAdapter*` and could plausibly
call `showApp()` on it). `MsnLink` is actually defined in a separate shared library,
`libLinkLibs.so` -- the common infrastructure underneath all of this firmware's wireless-
mirroring protocol libraries (`libMsnCarAuto.so`, `libMsnCarLife.so`, `libMsnCarPlay.so`,
`libMsnECLink.so`, `libMsnHiCar.so`, `libMsnMirrLink.so`). A real, bounded next step if this
is worth finishing: trace `libLinkLibs.so`'s own disassembly for the real `showApp()` call,
rather than continuing to search the two binaries already checked.

**Real, still-open connection to the `CMD 0x12`/headlights lead**: this now gives `CMD 0x82`
**three** independent real triggers (`pluginRunningStateChange()`, `onFirstInit()`,
`showApp()`) instead of one -- broadening, not narrowing, the set of real events that could
plausibly produce the `CMD 0x12` echo already traced. `showApp()` in particular sounds like
exactly the kind of UI-transition call a night-mode/headlights-driven screen change could
route through, but this remains unconfirmed -- same honest gap as before, just with one more
concrete name attached to it.

## Full stock-software `CMD` flow map (2026-09-02) -- every real transmit path, not just `makeMCUProtocol()`

Systematic follow-up: rather than only tracking `makeMCUProtocol()` callers, found **every**
real call into the one function that actually transmits, `ProtocolUtils::writeDatas()`
(**122 call sites** across the whole `libMcuCenter.so`, most belonging to the *other* vehicle
adapter classes this shared library also contains -- `BoxC270`, `BoxP210`, etc. -- which are
inactive on this hardware and out of scope). Filtered to just the confirmed-active
`MCUAdapter_BoxP300` -- **7 real sites**, one more than the `makeMCUProtocol()`-only pass
found:

| Real sender | `cmd` | Note |
|---|---|---|
| `onInited()` | `0x81` | Already traced -- first action at adapter init |
| `onModeAppChanged(uint,uint)` | `0x82` | **The real Qt slot connected to `modeAppChanged` -- builds its own frame directly** (not via `makeMCUProtocol()`, which is why the earlier pass missed it). Byte-for-byte matches this doc's existing "mode `2`/`4`/`5`/`7`/`13` append `8`; mode `23` appends `0x0A`" detail, now confirmed against the real send-side code, not just inferred |
| `showApp(uint)` | `0x82` | Already traced -- a second, still-unconfirmed-caller sender of the same command |
| `syncSettingDataToMcu(int)` | `0xA0` | Already traced |
| `onSendUpdateReadyTimer()` | `0xE1` | Already traced |
| `onRecvMcuProtocol()` (inside `CMD 0xE2`'s own handler body) | `0x81` | Already traced -- reply-send |
| `onRecvMcuProtocol()` (inside `CMD 0x7F`'s own handler body) | `0xE3` | Already traced -- reply-send, confirmed no-op on the real MCU |

**This means `CMD 0x84`/`0x85`/`0x87`/`0xFF`/`0x88` are never sent from anywhere inside
`MCUAdapter_BoxP300` itself** -- a real, decisive negative result, not an oversight (the
81/82/A0/E1/E3 set above is now the *complete* real list of everything this specific class
ever transmits). `CMD 0x88` was already found to be sent from `MsnCoreApp` directly
(`sendEncryptDatas()`, not through the adapter at all).

**Checked the shared `MCUAdapter` base class too, for anything sent from common code all
subclasses would inherit -- none found.** But this surfaced a genuinely new, previously
undocumented class in the process: **`McuCenterPlugin`**, whose own `customEvent(QEvent*)`
method has 3 of the 122 real `writeDatas()` sites. Traced its real dispatch:

- It's a generic **Qt custom-event router**: checks the incoming `QEvent`'s own type against 4
  specific custom type values (`0xC739`/`0xC73D`/`0xC73F`/`0xC743`), each independently
  dispatched.
- The `0xC73F` case (the one holding 2 of the 3 `writeDatas()` calls found) calls
  `MsnEvent::getByteArrayParam()` -- **extracting a pre-built, already-complete raw byte array
  attached to the event, and forwarding it to the MCU verbatim**, with no per-command
  structure of its own. This is a real, generic "send these exact bytes" mechanism, distinct
  from `makeMCUProtocol()`'s structured `(cmd, payload, len)` interface.

**CORRECTION (2026-09-02, same day): the `CMD 0x87` hypothesis above was weaker than
presented -- `0xC73F` is not a rare, BT-relay-specific tag.** Checked directly rather than
trusting the earlier read: the exact same `movw r3,#51007 @ 0xc73f` instruction appears **65
times** throughout `libMcuCenter.so`, as the routine 4th (`MsnEventType kind`) argument to
`MsnEvent::MsnEvent(int app, int type, MsnEventType kind)` constructor calls scattered across
completely unrelated features (confirmed several sites sit inside `onRecvMcuProtocol`'s own
`CMD 0x03`/`CMD 0x0A` handler bodies, nowhere near Bluetooth). So `0xC73F` is very likely the
**generic/default `MsnEventType`** this whole subsystem uses for ordinary events, and its
reuse as a `QEvent::type()` value in `McuCenterPlugin::customEvent()`'s dispatch is
architecturally coherent (this codebase appears to reuse `MsnEventType` enum values directly
as custom `QEvent::Type` values when an event needs cross-object delivery via Qt's queue) --
but that means the byte-array-forwarder case is a **general-purpose delivery path**, not one
narrowly tied to Bluetooth. The original structural argument for `CMD 0x87` (dynamic,
variable-length AT-command content fitting a generic forwarder better than a fixed
`makeMCUProtocol()` call) still stands as a plausible reason to route *through* this
mechanism, but "which of the many real posters actually carries `CMD 0x87`'s bytes" remains
genuinely unconfirmed, not just under-evidenced. Real next step, if this is worth finishing:
search for a raw byte-array construction with `0x87` as its second byte (the cmd position)
somewhere upstream of one of these `MsnEvent` constructions, rather than trying to narrow by
the event-kind tag alone.

**Consolidated picture for comparing against `custom_ui`'s own architecture**: stock's real
send-side is genuinely split across at least 3 different mechanisms, not one uniform path --
(1) `MCUAdapter_BoxP300`'s own structured `makeMCUProtocol()` calls (the majority: `0x81`,
`0x82`×2, `0xA0`, `0xE1`, plus 2 reply-sends), (2) `MsnCoreApp`'s own direct
`sendMsgToMcuCenter()` call (`0x88` only), and (3) `McuCenterPlugin`'s generic raw-byte
event-forwarder (very likely `0x87`, possibly others not yet traced). `custom_ui`'s own HAL
(`custom_ui/src/hal/mcu_input.cpp`) collapses all of this into one uniform `send_mcu_frame()`
call used everywhere -- a real, deliberate architectural simplification versus stock's
3-mechanism split, worth keeping in mind as a difference in kind, not just in which commands
get sent.

## `custom_ui` vs. stock -- direct send-side comparison (2026-09-02)

With both sides now mapped (stock above; `custom_ui`'s own real call sites in
`custom_ui/src/hal/mcu_input.cpp`), a direct comparison:

| Cmd | `custom_ui` sends it... | Stock's real sender | Match? |
|---|---|---|---|
| `0x81` | Once, in `send_startup_sequence()` (`McuInputHal`'s own connect-time init, `mcu_input.cpp:230`) | `MCUAdapter_BoxP300::onInited()` | ✅ Same command, different trigger shape -- stock's is the adapter's own real init hook; `custom_ui`'s is a hardcoded burst sent once at device-open time. Functionally equivalent for this command. |
| `0x82` | Once, fixed `mode=4` payload, same startup burst | 3 real triggers, **varying mode value** (`onFirstInit()`, `pluginRunningStateChange()` with `2`/`4`/`5`/`7`/`13`/`23`, `showApp()`) | ⚠️ Partial -- `custom_ui` sends one fixed value where stock sends a value that actually varies with *which* app/plugin state changed. `custom_ui` never needs to express "which app is foreground" the way stock's multi-plugin architecture does, so this simplification is likely fine in practice, but it's a real architectural gap, not just a smaller command set. |
| `0x84` | Sent via `sync_audio_route()`, called from `sync_video_relay()` (camera/mic toggle path) | **FOUND (2026-09-02): confirmed hard-stubbed dead for this product.** `MCUAdapter_BoxP300::onRecvAppProtocol()` -- the real, confirmed-virtual method (vtable slot `27`) that a different vehicle-adapter variant (`MCUAdapter_BoxC280`) uses to send this exact command -- is a single-instruction `bx lr` no-op on `BoxP300`. See the [full trace](#cmd-0x840x85s-real-provenance-found-2026-09-02----confirmed-hard-stubbed-dead-on-this-exact-product-not-missing-evidence) above. | ✅ **Resolved, and reframed**: not a "stock sender we couldn't find" gap -- stock's real send path for this product is deliberately dead code. `custom_ui`'s send is real, hardware-confirmed functionality the shared MCU protocol supports and other vehicle variants use, that this specific product's own stock software never exercises. |
| `0x85` | Sent once, empty payload, same startup burst | Same dead-stub finding as `0x84` -- `onRecvAppProtocol` is the one real candidate entry point for both, and it's a no-op on this product | ✅ Same reframing as `0x84` -- real capability, not a stock-behavior gap. |
| `0xA0` | Sent from `sync_setting()` (generic, used by every settings-screen toggle and `sync_video_relay()`) | `MCUAdapter_BoxP300::syncSettingDataToMcu(int)`, triggered by real user settings-screen interaction | ✅ **The closest 1:1 architectural match of any command** -- both sides route every settings change through one generic per-item sync function. |
| `0x87`, `0x88`, `0xE1` | **Never sent** | `0x87`: real sender still unconfirmed (see the `McuCenterPlugin` section above). `0x88`: `MsnCoreApp::sendEncryptDatas()`, real init + heartbeat. `0xE1`: `onSendUpdateReadyTimer()` | ✅ **Correctly, deliberately absent, not a gap** -- `custom_ui` handles Bluetooth via BlueZ (not the MCU relay), has no vendor anti-clone DRM to satisfy, and implements no MCU firmware-update flow. Omitting all three is the right call, not missing functionality. |
| `0xFF` | Never sent | **`libCanBus.so` avenue closed conclusively (2026-09-03), not left open.** Full sweep found a real ~15-vehicle-brand CAN-dashboard framework there (`CanBus_XinRi`, `_Raise_Volkswagen`, `_XBS_Mazda`, `_LiHang_JMCE200N`, `_Huida_ZD`, `_XinHang`, `_Raise_Honda`/`_Nissan`/`_GM`/`_Haval`/`_Jeep`, and more), confirmed using the same wire framing (`mov r3,#0x2e` in `makeCanBusProtocol()`) -- but its own real factory function (`CanBusAdapter::getAdapterInstance()`, `0x22B64`) returns `NULL` for `type==0` (a confirmed bounds-check underflow, not a guess), and the real product config (`MsnProductInfo.ini`) sets exactly `CanType=0`. **So this entire library never runs on this product at all** -- not "checked and found nothing," but "confirmed structurally incapable of running here." See `MCU_FIRMWARE_VERIFIED_FINDINGS.md`'s dedicated section for the full trace. | -- Neither side has a confirmed real sender; `libCanBus.so` is now a closed avenue, not an open one. |

**Real, honest summary**: `custom_ui`'s `CMD 0xA0` path is architecturally faithful to stock's
real design. Its `CMD 0x81`/`0x82` startup handling captures the *effect* stock achieves
(handshake + one mode announcement) via a simpler, hardcoded mechanism rather than stock's
event-driven one -- a reasonable simplification given `custom_ui` has no multi-plugin
architecture to announce transitions for. The real open items are `CMD 0x84`/`0x85`'s
still-unconfirmed stock provenance -- both commands work correctly in `custom_ui` (verified
independently from the MCU-firmware receive side and real hardware testing), but exactly
which stock code path originates them was not found this pass, so "does `custom_ui` replicate
stock's real trigger conditions for these two" remains genuinely open rather than confirmed
either way.

## `CMD 0x84`/`0x85`'s real provenance, found (2026-09-02) -- confirmed hard-stubbed dead on this exact product, not missing evidence

Direct follow-up to the open gap above. Real answer found, and it's more decisive than "we
couldn't locate the sender":

**Step 1 -- confirmed the commands are real and implemented, just not on this product.**
Broadened the `makeMCUProtocol()` cmd-value search from `MCUAdapter_BoxP300` to every real
vehicle-adapter class this shared library contains (each has its own mangled
`makeMCUProtocol` instance). Found a real hit: `MCUAdapter_BoxC280::onRecvAppProtocol(QByteArray
const&)` (`0x0007CD40`) sends `CMD 0x84` via its own `makeMCUProtocol()` call (`0x0007CEA8`,
`mov r2,#132`) -- a different vehicle-adapter variant, in the same binary, genuinely
implements this send. (Same pass turned up `MCUAdapter_BoxC280` also sending `cmd=0x83`
[131] -- a value not in this doc's own catalog at all, flagged here in case it's ever worth a
separate trace, not investigated further.)

**Step 2 -- checked `MCUAdapter_BoxP300`'s own `onRecvAppProtocol`, same method name, same
signature.** It exists (confirmed via `nm`, `0x00031E5C`) -- but its entire body is a single
instruction: **`bx lr`**. A hard, confirmed no-op. Nothing else in the function -- no
conditional skip, no partial implementation, just an immediate return.

**Step 3 -- confirmed this is a real virtual dispatch, not incidental.** `onRecvAppProtocol`'s
vtable slot was located precisely the same way `showApp()`'s was: `.rel.dyn`'s own
`R_ARM_ABS32` relocation for `MCUAdapter_BoxP300`'s vtable (`0x000BD930`) shows offset `0x6C`
(slot `27`) resolves to `0x00031E5C` -- confirming `onRecvAppProtocol(QByteArray)` really is a
polymorphic method every adapter subclass gets its own implementation of, and `BoxC280`'s real
implementation (which sends `CMD 0x84`) sits at the exact same conceptual slot as `BoxP300`'s
stub.

**Real, decisive conclusion**: `CMD 0x84`/`0x85` are genuinely implemented, real protocol
commands in this shared vendor library -- just not for this exact product. Whatever generic
mechanism hands raw app-protocol bytes off to the currently-active adapter (the method's own
name, `onRecvAppProtocol`, and its match to `McuCenterPlugin`'s generic byte-forwarding
`customEvent()` dispatch traced earlier in this doc, both point at this being the real landing
spot for that mechanism) calls a method that, for `BoxP300` specifically, does nothing at all.
This is not a gap in this project's own tracing -- it's confirmed, on-purpose dead code for
this vehicle variant, most likely because whatever vehicle/head-unit configuration `BoxC280`
targets uses a genuinely different audio-routing/ACK design than the Prado's `BoxP300`.

**What this settles for `custom_ui`**: its own `CMD 0x84`/`0x85` sends are not replicating any
real, currently-exercised stock behavior for this product -- they're implementing
functionality the real vendor software for this exact vehicle variant never triggers. That
doesn't make them wrong (`CMD 0x84`'s real *effect*, independently reverse-engineered from the
MCU firmware's own receive-side code, is genuine and hardware-confirmed to fix a real bug) --
but it reframes the comparison: this isn't "`custom_ui` replicates stock's trigger," it's
"`custom_ui` exercises a real capability of the shared MCU protocol that stock's software for
this product deliberately never uses."

## `CMD 0x87`'s real provenance, found (2026-09-02) -- Bluetooth never goes through the MCU relay at all

Chased the second loose end. Real, decisive answer, in the same spirit as `CMD 0x84`/`0x85`'s
finding above -- but different in kind: this isn't dead code on this product, it's evidence
the *entire mechanism* may never be exercised by real stock software.

**First, confirmed no vehicle-adapter variant sends it either**: broadened the same
cross-class `makeMCUProtocol()` search used for `CMD 0x84` to `cmd=0x87` (135) -- zero hits,
across every adapter class in `libMcuCenter.so`, not just `BoxP300`. Unlike `0x84` (found alive
in `BoxC280`), nothing in this shared library sends `0x87` at all.

**Then found where Bluetooth AT commands actually get sent**: `libBlueTooth.so` -- a
completely separate library from `libMcuCenter.so`, never previously examined in this doc --
imports `ProtocolUtils::ProtocolUtils(QObject*, int)` and `MsnSerialPortManager::
addSerialPort(int, ProtocolUtils*)` directly. `BlueToothAdapter::setupBluetoothSerialPort(int)`
constructs its **own, independent `ProtocolUtils` instance** and registers it as its own
serial port -- a structurally separate connection from whatever `MCUAdapter_BoxP300` uses,
not a shared one.

Three real Bluetooth-chip adapter classes exist (`BlueToothAdapter_SD851`,
`BlueToothAdapter_HD6956`, `BlueToothAdapter_Blueware` -- the last matching this project's own
already-documented legacy "blueware" stack this whole migration effort exists to retire), each
with their own `writeCommand(QString const&)`. Traced `BlueToothAdapter_Blueware::
writeCommand()` (`0x00045304`) directly: builds the AT command text via `QString::arg()`,
converts to raw bytes via `QString::toLocal8Bit()`, and calls **`MsnSerialPort::write(void
const*, int)` directly** -- **no `0x2E` sync byte, no `cmd` byte, no length byte, no
checksum. Just the raw AT-command text, verbatim, over its own dedicated serial connection.**

**Real, decisive conclusion**: stock software's real Bluetooth-command path does not use
`CMD 0x87`'s `[0x2E][0x87][len][AT command][checksum]` framing at all -- it opens its own
serial port and writes AT commands directly to whatever device is on the other end. This
doesn't contradict the MCU firmware's own confirmed real `CMD 0x87` handler (`0x080087A1`,
verbatim passthrough to `USART3`) -- that capability is real and implemented on the MCU side
-- but real stock application software, at least via this path, appears to talk to the
Bluetooth module directly rather than relaying through the MCU chip. **Not confirmed whether
this dedicated Bluetooth serial port is a genuinely separate physical UART or the same
`ttyHS0` channel used for everything else in this doc** -- `setupBluetoothSerialPort()`'s own
device-path string wasn't resolved this pass (real next step if it matters: same PC-relative
string-recovery technique used throughout this doc, applied to the literal at `0x36230`'s
target).

## `showApp()`'s real caller -- second real attempt, same result: not reliably findable this way

Chased the other loose end further. Broadened the vtable-slot-`88` (`0x58`) call-pattern
search from the two binaries already checked (`MsnCoreApp`, `libMcuCenter.so`, both negative
or false-positive) to the 6 wireless-mirroring protocol libraries built on `libLinkLibs.so`
(`libMsnCarAuto.so`, `libMsnCarLife.so`, `libMsnCarPlay.so`, `libMsnECLink.so`,
`libMsnHiCar.so`, `libMsnMirrLink.so`) -- the real, plausible candidates flagged in the prior
pass, since `MsnLink` (the class that plausibly owns the active `MCUAdapter*`) lives in
`libLinkLibs.so` and these are exactly the libraries built on top of it.

**Real hits in 5 of 6** (`libMsnCarLife.so`, `libMsnCarPlay.so`, `libMsnECLink.so`,
`libMsnHiCar.so`, `libMsnMirrLink.so` -- not `libMsnCarAuto.so`), at first a promising sign
given how semantically fitting "wireless-mirroring libraries call something related to
showing an app" sounds. **Checked the actual context of one, and it's a second real false
positive**: `libMsnCarLife.so`'s hit (`0x6728`) sits inside `CarLifeWindow::onLoadUiSkin()`,
and the value returned from the vtable call flows straight into building a `QString` for
`QWidget::setStyleSheet()` -- a stylesheet/theming helper call, unrelated to the MCU protocol
entirely.

**Real conclusion: vtable slot `0x58` is evidently a common, frequently-overridden slot shared
by many unrelated `QObject`-derived class hierarchies across this whole codebase** (plausibly
an early, generic virtual function most classes provide their own version of), which makes
byte-pattern matching on the slot offset alone fundamentally unreliable here without
type-aware tooling (a real decompiler with RTTI/vtable-layout resolution) this project doesn't
have available. Confirmed unreliable twice now, in two independent binaries, not a one-off --
**`showApp()`'s real caller is being recorded as genuinely not found by this project's current
tracing methods, not just "not yet chased far enough."** Real alternative if this ever matters
again: dynamic tracing on real hardware (a breakpoint/log at `showApp()`'s own real address,
`0x00031F2C`, would settle it in one real run) rather than more static vtable-offset guessing.
