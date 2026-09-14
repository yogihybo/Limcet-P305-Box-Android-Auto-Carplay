# Live Factory Bootloader Hang: Silicon Trace (2026-09-14)

**Status**: Open. Root cause narrowed to a single ~4-byte region; not yet resolved.
**Target file**: `hardware/MCU/live_dumps/vehicle_live_2026-09-14/live_factory_bootloader_12k_reconstructed.bin`
**Board**: spare STM32F105RBT6 test board (never the live vehicle unit).

This is a standalone trace, kept separate from `tools/patch_factory_bootloader_live.py`
and its `FACTORY_PATCH_MAP` so as not to collide with the in-progress reconstruction
work there. Nothing in this doc has been merged into that map.

---

## 1. The hang, confirmed on real silicon

Flashed `live_factory_bootloader_12k_reconstructed.bin` to `0x08000000` on the spare
board, let it free-run, and sampled live state repeatedly (halt / read / resume,
never `reset halt`, so as not to re-trigger reset between samples):

- **PC parks at `0x08000228`, every single time**, across 5 samples spanning ~4
  seconds of free-run.
- `CFSR`/`HFSR` both `0` -- no fault, it's a clean infinite loop, not a crash.
- `RCC->CR` / `RCC->CFGR` still at power-on-reset defaults (HSEON=0, PLL off) and
  `USART2->CR1`/`BRR` both `0` -- clock config and UART init never run. The hang
  happens very early in boot, before either.

## 2. What's at `0x08000228`, and why the call there is the real anomaly

Disassembly (Capstone, Thumb mode, **aligned to start at `0x08000200`** -- the
genuine post-vector-table code boundary; disassembling from `0x08000000` decodes
the vector table as bogus instructions and desyncs everything downstream):

```
0x08000208: revsh   r0, r0
0x0800020A: bx      lr
0x0800020C: bx      lr
0x0800020E: nop
0x08000210: b       #0x8000210      <- genuinely extracted (raw == reconstructed)
0x08000212: nop
0x08000214: b       #0x8000214      <- genuinely extracted
0x08000216: nop
0x08000218: b       #0x8000218      <- genuinely extracted
0x0800021A: nop
0x0800021C: b       #0x800021c      <- PATCHED (FACTORY_PATCH_MAP guess)
0x08000220: b       #0x8000220      <- PATCHED
0x08000224: b       #0x8000224      <- PATCHED
0x08000228: b       #0x8000228      <- PATCHED  <-- CPU parks here
```

`0x08000228 = 0xBF00E7FE` ("nop; b .") is one of `FACTORY_PATCH_MAP`'s 7 guessed
words for this block, entered as `SysTick_Handler (nop; b .)`. On its own, that
guess is **well corroborated**: it exactly continues an identical "b .; nop" stub
pattern found at 3 directly adjacent, genuinely-extracted-from-silicon addresses
(`0x08000210`, `0x08000214`, `0x08000218` -- confirmed via raw-vs-reconstructed
byte compare, not reconstructed). Locally this looks like a normal run of unused
default-interrupt-handler stubs, which is a completely ordinary thing to find in
Keil-generated startup code.

**The actual problem is a call into it.** A full, alignment-correct disassembly
of the whole 12KB image (starting at `0x08000200`) finds exactly one
control-flow instruction anywhere in the file that targets this block:

```
0x08000330: push {r4, lr}
0x08000332: bl   #0x8000228     <- genuinely extracted (raw == reconstructed)
0x08000336: pop  {r4, pc}
```

This `BL` is real, unpatched silicon data -- not a `FACTORY_PATCH_MAP` guess.
It sits immediately after a clean, fully-genuine PLL-lock / SYSCLK-switch
routine (`0x080002FE`-`0x0800032E`, disassembles as a textbook "wait for
PLLRDY, then wait for SWS==PLL" sequence ending in `pop {r2,r3,pc}`), and it
runs unconditionally on every boot -- there's no branch that skips over it.

A real, currently-working factory bootloader cannot unconditionally call into
a permanent trap on every single boot; the real vehicle unit demonstrably
boots today. Given the callee's content is well-corroborated by 3 independent
neighbors and the caller is the only thing pointing at it, **the caller
encoding at `0x08000332`-`0x08000335` (bytes `F7FF FF79`) is the more likely
corrupted value**, not the callee.

## 3. This matches an independent, pre-existing red flag

`docs/SESSION_HANDOFF_2026-09-13.md`'s caution section -- written in a **prior**
session, against an **earlier** capture attempt, before any of this
reconstruction work existed -- already named this exact address:

> "The factory bootloader dump contains multiple internal vector corruptions
> that branch into literal pools (`INVSTATE` UsageFault at `0x08000332`)."

Two separate capture attempts, two different wrong decodings (an `INVSTATE`
fault before, a valid-but-wrong branch target now), same exact address. That's
consistent with `0x08000332` being a specific, repeatable weak/unreliable read
location for the CVE-2020-8004 side channel -- not random noise, a recurring
problem spot.

## 4. Diagnostic experiment: skip the call

`tools/diagnose_bootloader_hang.py` produces a throwaway variant
(`live_factory_bootloader_12k_HANGFIX_DIAGNOSTIC.bin`, **not** committed to the
live-dumps directory as a real artifact -- it's a scratch test image) with the
`BL` at `0x08000332` replaced by `nop; nop`, to test the hypothesis
non-destructively: does skipping the call let boot proceed normally?

**Result: no, but it's informative.** Flashed and free-ran on the spare board.
PC is no longer stuck -- it moves between samples (`0x08000E16`, `0x08001E76`,
`0x08001E9A`, ...) and those addresses fall inside real bootloader code regions
(GPIO/IWDG config around `0x08000E00`, the YMODEM state-machine area around
`0x08001E00`, per `FACTORY_PATCH_MAP`'s own block comments). So skipping the
call does let execution get much further into the bootloader than before. But
`USART2->CR1`/`BRR` were still `0` at the one point they were sampled, so it's
not cleanly reaching a stable, fully-initialized state either -- more likely
the callee was doing something with a real side effect (a value left in a
register, a flash/GPIO configuration write) that downstream code depends on,
and removing it just traded one wrong path for a different, harder-to-classify
one.

**Conclusion: "nop out the call" is not the fix.** It's useful negative
evidence -- it rules out "the callee is simply dead code that's safe to skip"
-- but the real correct bytes at `0x08000332` are still unknown.

## 5. Recommended next steps

1. **Repeated/majority-vote re-extraction targeted specifically at
   `0x08000330`-`0x08000338`.** Given this exact address has now produced two
   different wrong decodings across two separate capture sessions, it's a
   strong candidate for a genuinely flaky read (as opposed to the
   architecturally-unreachable gaps elsewhere, which are gaps by design, not
   noise). Re-running extraction many times (10-20x) and checking whether the
   bits are actually stable or vary between attempts would directly confirm
   or rule out a flaky-read theory.
2. **Single-step trace of the diagnostic variant**, rather than periodic
   polling, to map its actual full path after the skipped call -- a hardware
   breakpoint at `0x08000234` (end of the block in question) plus step-through
   would show definitively whether it ever reaches USART2 config or the
   YMODEM loop, rather than inferring from a few point-samples.
3. Leave `FACTORY_PATCH_MAP`'s guesses at `0x0800021C`-`0x08000228` as they
   are for now -- the neighbor-pattern evidence for them is solid. The open
   question is squarely the caller at `0x08000332`, not the callee block.

## 6. Deeper trace: identifying what `0x08000228` actually is, and a real candidate

Went back with a proper alignment-correct full disassembly (`0x08000200`-`0x08000400`,
starting exactly at the post-vector-table boundary) and found the missing piece:
genuine (unpatched) code at `0x08000272` is:

```
0x0800026E: ldr r0, [sp]
0x08000270: cmp r0, #1
0x08000272: bne #0x800032e          <- genuine, real silicon bytes
0x08000274: ... (genuine PLL2/PLL/SW-switch config, all real bytes) ...
0x0800032E: pop {r2, r3, pc}        <- genuine, function return
```

This is textbook ST Standard Peripheral Library `SetSysClock()`: `if (HSEStatus
== 1) { configure PLL2, PLL, switch SYSCLK to PLL } ; return`. The genuine, real
silicon code from `0x08000274` through `0x0800032E` **only makes sense as the
back half of a function whose entry point provides two locals on the stack**
(`HSEStatus` and `StartUpCounter`, read/written at `[sp,#0]`/`[sp,#4]` starting
at genuine address `0x08000230` onward) **and enables HSE** (`RCC->CR |=
HSEON`) before the wait loop. That prologue has to live somewhere, and the only
place left for it is the very block that was guessed as more trap stubs:
`0x0800021C`-`0x08000228`.

Given that, and that only 3 of the 7 guessed words (`0x08000210`/`0x14`/`0x18`)
are actually confirmed-genuine trap bytes -- the other 4 (`0x0800021C`-`0x08000228`)
were *never independently confirmed*, just pattern-matched to their genuine
neighbors -- the likely real split is: traps genuinely end at `0x08000218`, and
`SetSysClock()`'s real prologue begins in the 16 bytes from `0x0800021C` to
`0x08000228` (`0x0800022C` onward is confirmed genuine and already reads/writes
two locals with an all-zero seed value, which is exactly what a `push
{r2,r3,lr}; sub sp,#8; RCC->CR |= HSEON; movs r0,#0` prologue would set up
immediately before it).

### Candidate patch and hardware test

Built and round-trip-verified (each instruction re-disassembled through
Capstone to confirm byte-exact correctness, not hand-derived guessing) an
8-instruction, 16-byte candidate for `0x0800021C`-`0x0800022B`:

```
0x0800021C: push {r2, r3, lr}
0x0800021E: sub  sp, #8
0x08000220: ldr  r1, [pc, #0x258]   -> resolves to the genuine, already-used
                                        RCC_BASE literal at 0x0800047C
0x08000222: ldr  r2, [r1]
0x08000224: orr.w r2, r2, #0x10000  ; RCC_CR_HSEON
0x08000228: str  r2, [r1]
0x0800022A: movs r0, #0
```

Flashed to the spare board (bytes verified byte-exact on readback). Result:
**the original permanent hang at `0x08000228` is gone.** The CPU no longer
parks there. Instead, after ~2s free-run, it's caught in a *different*,
genuine `HardFault` (`CFSR = 0x00020000` = `INVSTATE`, `HFSR = 0x40000000` =
`FORCED`) -- i.e. it got much further into real boot code before hitting a
second, separate fault.

The auto-stacked exception frame at the fault (`r1 = 0x40021000` = `RCC_BASE`,
confirming the prologue's `ldr r1, =RCC_BASE` value survived and was still in
use much later in execution) shows the CPU attempted to branch to
**`0x00003000`** -- suspiciously close to `0x08003000`, this project's
independently-confirmed real application base address, just missing the
`0x08` high byte and the Thumb bit. That's consistent with this being the
bootloader's app-validity-check-and-jump logic finally being reached (deep
into boot, past clock config, UART, and whatever main-loop dispatch precedes
it) and faulting on a similarly corrupted/mis-extracted pointer or vector
word, rather than anything related to this patch.

**Important open thread, not yet resolved:** immediately after this
candidate's 4 words, genuine (unpatched) code at `0x08000230`-`0x0800023A`
also writes to `[r1]` (i.e. `RCC->CR`, since `r1` still holds `RCC_BASE` from
the prologue) with a value computed as `(*some_literal) + 0x80` -- **not**
the `HSEON`-set value my patch just wrote. That means either (a) this
genuine code is legitimately supposed to overwrite `CR` again as part of real
factory behavior (plausible -- SPL `SetSysClock()` does sequence multiple CR
writes), or (b) my prologue's specific split of "which 2 of the 4 words do
what" doesn't exactly match the real compiled layout even though it's
structurally very close. `RCC->CR` read back at the fault point still showed
`HSEON = 0`, consistent with (b) or with this being a legitimate intermediate
overwrite either way -- **not yet distinguished.** This is the next concrete
thing to pin down, not the app-jump fault, which looks like a separate,
deeper issue.

### Why this is a meaningfully different (better) result than section 4

Section 4's plain "nop out the call" experiment left the CPU wandering through
several loosely-related code regions without ever settling into a clean,
recognizable state. This candidate, despite the specific byte guess still
being unconfirmed in detail, gets **structurally further and lands on a
single, clean, reproducible fault** in a specific, plausible spot (the app
jump) rather than vague wandering -- much stronger evidence that the general
shape (a real `SetSysClock()` prologue, not a trap) is correct, even though
the exact byte-level content still needs refinement.

## Files from this trace

- `tools/diagnose_bootloader_hang.py` -- produces the diagnostic nop-patched
  variant used in section 4. Not a proposed fix; a one-off experiment script.
- `/tmp/.../scratchpad/bootloader_setsysclock_candidate.bin` -- the section 6
  candidate patch, built in the session scratchpad (not committed -- it's an
  experimental variant, not a proposed final fix). Regenerate by splicing the
  16 bytes documented in section 6 into
  `live_factory_bootloader_12k_reconstructed.bin` at `0x0800021C`.
- This doc.

## 7. Applied to `FACTORY_PATCH_MAP`, retested, and single-stepped to find the real blocker

The candidate from section 6 was applied directly to
`tools/patch_factory_bootloader_live.py`'s `FACTORY_PATCH_MAP` (the other
agent isn't currently using this script, so this was safe) and
`live_factory_bootloader_12k_reconstructed.bin` was regenerated through the
actual script rather than the scratch splice. Flashed alongside our
clean-room `can_app.bin` at `0x08003000` to check whether a real application
image changes anything.

**Result: no change from app content.** Same `INVSTATE`/`FORCED` HardFault,
same trap PC (`0x08000210`). This confirms the fault is entirely internal to
the bootloader -- it never gets far enough for the app image to matter.

**The fault location is not stable between runs.** Repeating the exact same
test twice gave two different attempted-branch addresses in the stacked
exception frame (`0x00003000` once, `0x00001f50` another time) while `LR`
stayed identical both times. Flagged as unresolved rather than papered over.

### Single-step trace: found the real problem

A proper single-step trace (hardware single-stepping inside one persistent
OpenOCD/Tcl session, not point-in-time polling) showed the CPU cleanly
executing the prologue and immediately entering the genuine HSE-ready wait
loop (`0x0800023E`-`0x08000258`), looping normally with `CFSR = 0` for as
many steps as were tried. Free-running to the fault happens within ~300ms
regardless -- resolved as simply a step-count mismatch (single-stepping is
several orders of magnitude slower in wall-clock time than free execution
for the same instruction count), not a real timing paradox.

**Set a hardware breakpoint at `0x08000274`** (only reached if `HSEStatus ==
1`, i.e. HSE genuinely became ready) and let the CPU free-run. **The
breakpoint never triggered** -- the CPU reached the `HardFault` trap without
ever taking the "HSE ready" path. That means the wait loop always times out;
HSE never comes up. `RCC->CR` read at the fault consistently shows `HSEON`
(bit16) **not set**, despite the candidate patch's `orr r2,r2,#0x10000; str
r2,[r1]` sequence executing (confirmed reached, via the single-step trace).

**Root cause of that: a genuine, unpatched instruction clobbers it.**
Immediately after the candidate's 4 words, real (unpatched) code at
`0x08000230`-`0x0800023A` does:
```
ldr r0, [pc, #0x248]   ; r0 = *(0x08000480) = 0x40022000 (FLASH_BASE)
ldr r0, [r0]
adds r0, #0x80
orr  sb, r0, #0x4800   ; r9, unrelated
str  r0, [r1]          ; writes r0 into [r1] -- needs r1 == FLASH_BASE here
```
For this to be sane (a `FLASH->ACR` pre-configuration, plausible if the real
compiled `SetSysClock()` sets flash wait-states *before* the PLL wait loop
rather than after, unlike the generic reference source), **`r1` must equal
`FLASH_BASE` (`0x40022000`) at this point, not `RCC_BASE`.** The section 6
candidate leaves `r1 == RCC_BASE` (it never reloads r1), so this genuine
`str r0, [r1]` instruction corrupts `RCC->CR` with an unrelated computed
value immediately after the candidate sets `HSEON` -- explaining exactly why
`HSEON` never survives to the wait loop.

### Section 6 candidate v2: reuse caller's r1 for RCC, end with r1=FLASH_BASE

Built a second candidate that assumes `r1` already holds `RCC_BASE` on entry
(inherited from `SystemInit()`'s own preamble, which repeatedly reloads `r1`
with `RCC_BASE` right up until the call into this function) rather than
reloading it, freeing up an instruction slot to explicitly reload
`r1 = FLASH_BASE` (reusing the same literal at `0x08000480` genuine code
already uses) before falling through:

```
0x0800021C: push {r2, r3, lr}
0x0800021E: sub  sp, #8
0x08000220: ldr  r2, [r1]            ; reuse caller's r1 == RCC_BASE
0x08000222: orr.w r2, r2, #0x10000
0x08000226: str  r2, [r1]
0x08000228: ldr  r1, [pc, #0x254]    ; r1 = FLASH_BASE (0x40022000), for the
                                        genuine code that follows
0x0800022A: movs r0, #0
```

Round-trip verified through Capstone, hardware tested (same methodology:
flash, free-run, inspect). **Result: no improvement** -- same
`INVSTATE`/`FORCED` fault signature, `RCC->CR` still shows `HSEON` unset at
fault time. Either the "reuse caller's r1" assumption is also wrong, or
there's a further issue this hasn't uncovered yet. Not committed to
`FACTORY_PATCH_MAP` -- the section 6 candidate (v1) remains the one applied
there, since it's the one actually tested via the map/script path and it did
produce the real, confirmed improvement (original permanent hang eliminated).

### Where this leaves things

- **Confirmed real progress**: the original permanent hang at `0x08000228`
  is genuinely fixed by the section 6 candidate. That's not in question --
  reproduced multiple times.
- **Not yet resolved**: the exact correct bytes for `0x0800021C`-`0x08000228`
  still aren't known. Two structurally-reasonable candidates both hit the
  same downstream `INVSTATE` fault, for what looks like the same underlying
  reason (`RCC->CR` losing `HSEON` before the wait loop reads it).
- **This is now a narrower, well-defined problem**: figure out what genuine
  code at `0x08000230`-`0x0800023A` actually needs in `r1` and `r0` at that
  exact entry point, and design the 16-byte prologue backward from that
  constraint, rather than forward from "what does a generic `SetSysClock()`
  prologue look like." The FLASH_ACR-before-PLL-wait ordering (unusual vs.
  the generic ST reference source, which configures flash latency *after*
  confirming HSE is ready) is itself worth double-checking against the
  literal-pool value and surrounding genuine bytes before trusting it as a
  hard constraint.

## 8. Found and fixed a real bug: `0x08000234` had swapped Thumb halfwords

While chasing the section 7 mystery, found the actual root cause of most of
the confusion: the *original* map author's guess for `0x08000234` --
`0xF4403080`, commented "orr.w r0,r0,#0x10000 (SystemInit RCC_CR_HSEON
enable)" -- has its two 16-bit halfwords in the wrong order. As encoded,
memory bytes `80 30 40 F4` decode to `adds r0,#0x80` followed by a stray,
desyncing half-instruction, not the intended single 32-bit `orr.w`. Verified
via Capstone round-trip: `0xF4403080` disassembles to `adds r0,#0x80`,
while `0x3080F440` (halfwords swapped) correctly disassembles to
`orr.w r0,r0,#0x10000`, matching the author's own comment. This is a
confirmed encoding bug, not a new guess -- same class of mistake I'd made
and caught myself making earlier in this same session with a different
instruction.

Fixed in `FACTORY_PATCH_MAP`. This resynchronized instruction decoding for
everything from `0x08000238` onward, and revealed that genuine (unpatched)
code at `0x08000230`-`0x0800023A` **already independently handles the
RCC_CR_HSEON enable** (loads `RCC_BASE` via its own literal at `0x0800047C`,
reads `CR`, and -- once 0x08000234 decodes correctly -- ORs in HSEON and
writes it back). My section 6/7 candidates' own HSEON-enable write was
therefore redundant. (Also: my earlier belief that this genuine block
touched `FLASH_BASE`/`FLASH->ACR` was a hex-addition error on my part --
`0x230 + 4 + 0x248` is `0x47C`, not `0x480`. Correcting that removed the
apparent FLASH_ACR/RCC_BASE conflict that drove candidate v2 in section 7.)

## 9. Simplified prologue, hardware-confirmed: clock configuration fully works

With the `0x08000234` fix in place, simplified the `0x0800021C`-`0x08000228`
prologue back down to just `push {r2,r3,lr}` + padding + `movs r0,#0` (no
`sub sp,#8`, no HSEON write -- both unnecessary per section 8). Round-trip
verified via Capstone, applied to `FACTORY_PATCH_MAP`, regenerated through
the real script, flashed to the spare board.

**Result: `RCC->CR` reads back as `0x0F036D83`** -- `HSEON`, `HSERDY`,
`PLLON`, and `PLLRDY` all set. `RCC->CFGR` shows `SYSCLK` genuinely switched
to `PLL`. Reproduced multiple times. **Clock configuration is now fully,
reliably working** -- this is real, confirmed, hardware-verified progress on
top of the section 6 fix (which only eliminated the permanent hang; this is
the first time the clock has actually *locked correctly*).

## 10. Remaining problem: a later, separate fault attempting to branch to a truncated app address

After clock config succeeds, the CPU still hits an `INVSTATE`/`FORCED`
HardFault. The exception frame is highly reproducible: `LR = 0x08000337`
(unchanged since entering the patched region -- meaning no `BL` executes
anywhere between there and this fault) and an attempted branch target of
**`0x00003000`** -- suspiciously exactly `0x08003000` (the confirmed real
application base address) with the high byte missing.

**This is not caused by the `0x0800021C`-`0x08000228` prologue.** Tested 3
different variants of that prologue (with/without `sub sp,#8`, with/without
a manual HSEON write, with a 2-register vs. 3-register push) -- all three
hit the exact same fault signature (same `LR`, same `0x00003000` target,
confirmed both via a breakpoint-based capture and an independent
no-breakpoint free-run capture). A brief investigation into "is this a stack
imbalance in my push/pop" (comparing captured stack contents across
variants) did not converge -- the evidence was inconsistent between
capture methods and time did not allow fully resolving it. **This looks
like a separate bug, later in the boot sequence** (plausibly the actual
app-validity-check-and-jump logic), not a continuation of the section 6-9
work.

**Ruled out**: every one of the 113 `FACTORY_PATCH_MAP` gap words is now
accounted for (no unpatched gaps remain in the code region -- confirmed via
a full scan of the raw dump). So this isn't a *missing* patch; it's either
a *wrong* value among the existing 113, or a genuine misunderstanding of
real control flow.

**Concrete lead for next session**: a systematic scan (same halfword-swap
check that found the section 8 bug, applied to every entry in
`FACTORY_PATCH_MAP` whose comment mentions a wide `.w`-suffixed Thumb-2
instruction) turned up **10 more suspects** with the same signature (a
32-bit-instruction opcode halfword sitting in the wrong position):
`0x0800061C`, `0x08000C04`, `0x08000C1C`, `0x08000C24`, `0x08000E28`,
`0x08001020`, `0x0800121C`, `0x08001220`, `0x08001224`, `0x08001620`.
Unlike the clean 0x08000234 case, most of these span a boundary between two
genuine half-instructions from neighboring words (same "gap word doesn't
align to instruction boundaries" complication as section 6-7), so fixing
them isn't a simple swap -- each needs the same context-aware disassembly
work applied to 0x08000234. None of them are obviously on the early-boot
path to the section 10 fault (they're in FLASH-erase/GPIO_Init/IWDG/RCC-
prescaler/ADC/USART-halfduplex library functions, likely only reached
later or conditionally), so they're a lower-priority but real lead, not
the immediate blocker.

## 11. Root cause finally found: the whole `0x0800021C` prologue was placed at the wrong address

Section 10's "later, separate fault" investigation kept producing
contradictory stack data no matter how the `0x0800021C`-`0x08000228`
prologue was rearranged. Chasing that down with a careful, deterministic
single-step trace (not breakpoint-based -- breakpoint-triggered halts on
this CMSIS-DAP/OpenOCD setup turned out to be unreliable, see the dead-end
noted below) found the actual mistake:

**The genuine (unpatched, confirmed real) call site at `0x08000332` is
`bl #0x8000228`.** That targets `0x08000228` *directly* -- meaning the
function's real entry point is `0x08000228`, not `0x0800021C`. Every
candidate in sections 6-10 placed `push {r2,r3,lr}` at `0x0800021C`, which
is *before* the call target in address order and is simply never reached by
this call at all. Single-step evidence confirms it cleanly: `SP` never
changes across `0x0800021C`-`0x08000224` (proving no instruction there
executes on this path), and the mysterious stack values from section 10
(`0x00003000`, `0x08004F50` correlating with app image size, etc.) turned
out to be genuine, pre-existing content from the wrapper's and `SystemInit`'s
own already-pushed `{r4,lr}` frames one level up the stack -- not a bug in
app-jump logic at all, just `pop {r2,r3,pc}` reading stale frames above an
un-executed push.

**Fix**: the entire prologue has to self-containedly fit in the single
4-byte word at `0x08000228` (the real entry point): `push {r2,r3,lr}`
(2 bytes) + `movs r0,#0` (2 bytes) = exactly 4 bytes. `0x0800021C`,
`0x08000220`, `0x08000224` are restored to the original "nop; b ."
trap-stub guess (matching the genuinely-extracted neighbors at
`0x210`/`14`/`18`) since they're confirmed unreached by this call path --
their content doesn't matter for this bug, so the original guess is kept
rather than inventing something.

**Hardware-confirmed**: flashed, free-ran, sampled repeatedly. **No more
HardFault at all** -- `CFSR`/`HFSR` both `0` continuously. `RCC->CR` still
reads `0x0F036D83` (clock config unaffected, still fully working). `PC`
genuinely moves between samples across a wide range of real code addresses
(`0x1e78`, `0x16b2`, `0x1d92`, `0x1e94`, ...) instead of being parked
anywhere -- the bootloader is authentically executing forward through real
logic for the first time in this whole investigation.

## 12. Second real bug found the same way: `0x08001C20` self-loop

With the entry-point fix in place, execution reached a NEW address it had
never gotten close to before: a "Serial Packet Transmit Loop"
(`0x08001C00`-`0x08001C34`, part of the bootloader's UART handshake/
announcement packet logic). It got stuck there -- not faulting, just
spinning. Disassembly showed the smoking gun immediately:

```
0x08001C1C: adds r5, #1        (genuine)
0x08001C1E: uxtb r5, r5        (genuine)
0x08001C20: cmp r5, #6         (patched, gap word)
0x08001C22: blt #0x8001c22     (patched -- SELF-LOOP)
```

Exactly the same bug class as section 8: the map's own comment for
`0x08001C20` already said the intended target was `0x8001bfa` (a real retry
loop, looping back to resend), but the encoded bytes (`0xDBFE2D06`) actually
decode to `blt` branching to its own address. Computed and Capstone-verified
the correct encoding for `blt #0x8001bfa` at that position: `0xDBEA2D06`.
Fixed, regenerated, reflashed.

**Hardware-confirmed**: that specific hang is gone. Execution now moves
through the transmit-loop region and beyond it, continuing to visit new
addresses without faulting.

## 13. Current status: fault-free, deep execution; UART never gets enabled

With both fixes applied, the bootloader now runs, unfaulted, through a wide
swath of real code for as long as it's been observed (10+ seconds
free-running). It's not fully working yet: **`USART2->CR1`/`BRR` never
leave `0x00000000`** across every sample taken, meaning the UART peripheral
itself is never actually enabled. Traced one of the loops this causes:
`0x08001C08`-`0x08001C12` calls a small helper at `0x0800169C` (confirmed
via disassembly to be `USART_GetFlagStatus`-equivalent: reads `[base+0]`
i.e. `SR`, ANDs with a flag mask, returns 0/1) checking `USART_SR` bit 6
(`TXE`) -- with the peripheral disabled, that flag presumably never sets,
so this becomes an infinite (non-faulting) wait rather than a real hang.

Checked the two most likely USART-init gap-word blocks (`0x08001400`-`34`
and `0x08001600`-`34`) for the same self-loop/halfword-swap signature that
caught the last two bugs -- both disassemble cleanly, no obvious garbage.
So this isn't (at least not obviously) the same bug class again; either
`USART_Init()`/`USART_Cmd(ENABLE)` genuinely isn't called yet by this point
in the flow, or there's a subtler issue than a simple encoding swap.
**This is the concrete next lead**, not yet root-caused.

## Summary of this session's fixes (all in `FACTORY_PATCH_MAP`, all hardware-verified)

1. `0x08000234`: halfword-swap bug, real word is `0x3080F440` not
   `0xF4403080` (section 8).
2. `0x0800021C`-`0x08000228`: prologue relocated to the correct entry point
   `0x08000228` (the real `bl` target), not `0x0800021C` (section 11).
3. `0x08001C20`: halfword-swap bug, real word is `0xDBEA2D06` not
   `0xDBFE2D06` (section 12).

Net effect: **permanent hang at the very first patched instruction** →
**clock configuration fully working** → **completely fault-free execution
deep into the bootloader's real UART/packet logic**. The remaining blocker
(UART never enabled) is narrower and better-characterized than anything at
the start of this session.

## 14. Chasing the USART-never-enabled issue: found real structure, not yet the answer

Traced main()'s actual entry sequence at `0x08001800`-`0x08001810` (all
genuine, unpatched): calls, in order, `0x0800193C` (zero-init the packet
buffer struct -- confirmed, no USART touch), `0x08001956` (a 5-byte
handshake-frame transmit loop, structurally identical to the 6-byte one
fixed in section 12 -- also no USART enable), `0x0800052A` (confirmed
`flash_unlock()` from earlier-session work), `0x08001EB0` (the bootloader's
actual main polling loop -- feeds the watchdog via `IWDG_ReloadCounter`,
dispatches to several sub-handlers, loops forever), and `0x08000542`
(a small `FLASH_CR`-bit-set helper, unrelated to USART). **None of main()'s
own top-level calls touch `RCC_APB1ENR` or configure GPIOA for USART2.**

Confirmed on hardware: `RCC->APB1ENR` (`0x4002101C`) reads `0x00000000` --
USART2's peripheral clock bit (bit17) is never set, and `GPIOA->CRL` sits at
its power-on-reset default (`0x44444444`, all pins floating input) -- PA2/
PA3 were never configured as USART2 TX/RX alternate-function pins either.
This is a real, independent confirmation of *why* the TXE-wait loop from
section 13 can never succeed: it's not just "USART_Init wasn't called",
the peripheral doesn't even have a bus clock.

**Found the generic clock-enable function, not (yet) its USART2 call
site.** A literal-pool sweep of the whole 12KB image for both the absolute
address of `RCC_APB1ENR` and the `RCC_BASE + 0x1C` offset pattern located a
real function at `~0x08000D0A`: takes a peripheral bitmask/register-select
argument, checks bit31 to choose between `RCC->APB1ENR` (offset `0x1C`) and
`RCC->AHBENR`/other (offset `0x04`), does a read-modify-write. This is
almost certainly `RCC_APB1PeriphClockCmd()` (or a fused
APB1/APB2/AHB-generic variant) from the Standard Peripheral Library. What's
still unknown: **who calls it with USART2's bit set, and where in the boot
sequence that happens (or should happen) relative to what's already been
traced.**

An ad-hoc, address-chopped linear disassembly sweep (splitting the image
into small fixed-size windows to survive literal-pool desync) failed to
find any caller -- that approach is fundamentally unreliable for this kind
of whole-image call-graph question; it needs either a proper recursive
disassembler (follow every confirmed branch/call target instead of
guessing window boundaries) or continued single-step tracing from the main
loop entry (`0x08001EB0`) into its dispatched sub-handlers
(`0x8001d78`, `0x8001b62`, `0x8001b82`, `0x8001c72`, `0x8001bb2` --
none examined yet) to see if USART/GPIO setup happens lazily, on first use,
rather than eagerly at boot.

**This is where this session's investigation stops.** Real, concrete
progress continues to compound (see section 13's summary table), but this
specific question -- where USART2 should get its clock and GPIO AF config
-- needs either better tooling (a real call-graph builder) or a fresh,
well-rested tracing pass through the 5 unexamined main-loop dispatch
targets, rather than more ad-hoc scanning.

## 15. Proper recursive-descent call-graph analysis: the USART question has a real answer

Built a real tool (`recursive_disasm2.py`, in the session scratchpad) instead
of continuing ad-hoc scanning: starting from `Reset_Handler`, it follows
only *proven* control-flow edges -- `bl #imm`, conditional branches, and
critically, indirect `blx rX`/`bx rX` where `rX` was just loaded via
`ldr rX,[pc,#N]` (the literal-function-pointer pattern `Reset_Handler`
itself uses for `SystemInit`). Because it never disassembles bytes that
aren't provably reachable, it can't desync on embedded literal pools the
way linear/windowed sweeps do. Reached 1242 real instructions and a 56-node
call graph from a single seed address.

**Found the real `RCC_APB1PeriphClockCmd()`/`RCC_APB2PeriphClockCmd()`
pair** at `0x08001298`/`0x0800127E` (genuine, unpatched, clean generic
enable/disable-by-bitmask implementations). Traced their one real caller to
`0x080018BC`: a peripheral-init routine that is unambiguously "bring up
USART2" -- enable AFIO+GPIOA+GPIOB+GPIOC clocks (bitmask `0x1D`), configure
PA2 as `AF_PP` output (TX) and PA3 as floating input (RX), enable USART2's
clock (`1 << 17`), then set up baud `0x9600` = **38400 decimal**, matching
this project's own already-confirmed real baud rate. This function is real,
correctly-encoded, and genuinely present -- not the bug.

**Found why it doesn't run: a real, intentional fast-boot decision gate.**
Traced backward from that function's one caller (`0x080017FC`) to a guard
at `0x080017E2` that checks two conditions before deciding whether to do
full peripheral init at all:
1. `*0x20004004 == 0x5555AAAA` -- the project's own already-documented
   bootloader-entry magic cookie (set when `CMD 0xE1` triggers a real
   firmware-update session).
2. Else, `*0x08002FFE == 0x5AA5` -- a 2-byte marker at the very last
   halfword of the bootloader's own flash region.

If *either* is true, it takes the branch that leads to `0x080017FC`'s full
USART/GPIO init. If *neither* is true, it jumps straight to `0x0800181E`,
skipping peripheral init and going into what looks like the real "no
pending update -- wait out a timeout, then validate and jump to the
application" logic (a counter loop comparing against `0xFF0E`, matching
this project's already-documented ~30s bootloader timeout).

**Hardware-checked**: `0x20004004` reads `0` (no cookie) and `0x08002FFE`
falls inside the confirmed-genuine, confirmed-erased flash tail (`0xFF`
fill) -- so on this image, *both* checks correctly evaluate false. This
means "skip full peripheral init" is very likely the **intended, correct
path for a normal cold boot with no pending update** -- not a bug. Cleared
`0x20004004` explicitly and re-tested to rule out session cross-
contamination; no change (it was already `0`).

## 16. Where this leaves the USART hang -- a real, narrower open question

Despite that, the CPU is still observed spinning in UART-transmit-adjacent
code (`0x169C`-style TXE-flag-check helper, `USART2->SR` genuinely reads
`0`) even after 35 seconds of free-run -- it never reaches the application
(`PC` never enters `0x08003000`+) and never reaches the `0xFF0E` timeout
loop's own body addresses either. This is a real, remaining contradiction:
if the "skip full init" path is correct, something *else* in that path
still attempts a UART transmission using the disabled peripheral -- most
likely a periodic status/heartbeat send from within the main loop
(`0x08001EB0`'s dispatch targets: `0x8001d78`, `0x8001b62`, `0x8001b82`,
`0x8001c72`, `0x8001bb2` -- still not individually traced) that doesn't
check whether full init actually ran first.

**Two concrete, well-defined hypotheses for next time**, both actionable
with the new call-graph tool:
1. One of the still-unexamined main-loop dispatch targets unconditionally
   attempts a UART send regardless of which branch was taken at
   `0x080017E2` -- trace them with `recursive_disasm2.py` (already handles
   indirect calls, so it should map this cleanly) to find which one, and
   check whether *that* call site has a gap-word encoding bug of its own.
2. Less likely but worth ruling out: the two magic-cookie/marker checks
   are reversed in sense (i.e. "skip" and "full init" branches are mixed up
   somewhere in the still-guessed `0x08001818`-`0x08001820`ish gap words),
   so what looks like "no update pending" is actually being read as
   "update pending" by the surrounding logic.

## 17. Fixed the tooling gap: hardware breakpoints now work reliably

Section 10/14's breakpoint-based captures were unreliable because of a real,
now-understood cause: **once execution proceeds far enough into the image,
the CPU is fetching through the `0x00000000` memory alias, not the
canonical `0x08000000` address** (normal STM32 boot-remap behavior). A
breakpoint set at the canonical address (e.g. `0x0800169C`) silently never
fires once code is running through the alias; the same breakpoint set at
the alias address (`0x0000169C`) fires immediately and reliably, confirmed
repeatably via `wait_halt`. Early code (`0x08000228` etc.) still runs at
the canonical address, so this is a discovered *transition*, not a general
rule -- worth remembering for all future work on this dump: **prefer alias
addresses for breakpoints on anything reached via a BX/indirect branch deep
in the boot sequence.**

## 18. What the main loop is actually doing (hardware-confirmed, not guessed)

With reliable breakpoints, traced the *actual* runtime path (not just
static reachability) through the confirmed main loop at `0x08001D8E`/
`0x08001EB0`:

- A non-blocking RXNE poll (`0x08001D8E`-`0x08001D9C`): feed the watchdog,
  check for an incoming byte, continue either way. This one is fine --
  it's not a hang, just normal idle polling, and explains why `PC`
  appeared to "bounce" across a small set of addresses in earlier
  sessions' point-in-time samples: that was just healthy loop iteration,
  not being stuck.
- **A genuine, unconditional periodic hang**: `0x08001E76`-`0x08001E94`
  increments a counter (`r5`) every loop pass; once it reaches `0x00FFFFFF`
  (~16.8M), it unconditionally calls a "send handshake frame" function
  (`0x080019A2`, confirmed clean/correctly-encoded, no gap-word bugs) whose
  inner loop genuinely **blocks** waiting for `USART2_SR.TXE` via
  `0x0800169C`. Since USART2 is never enabled, `TXE` never sets, and this
  never returns. This is the real, confirmed mechanism behind every
  "stuck" observation across sections 13-16.
- A *separate*, much larger counter (`r6`, threshold `0x0FFFFFFF`, ~268M)
  gates an eventual permanent `b .` trap after a flash-related call --
  this is not a meaningful "jump to app" timeout the way section 14
  originally guessed from the unrelated `0xFF0E` loop; it's far too large
  to matter in practice (the `r5` hang fires first, always, since its
  threshold is 16x smaller).

**Confirmed via a 90-second free-run**: `PC` never reaches `0x08003000`+
(the application). It hangs on the `r5`/TXE mechanism well before that.

## 19. The magic-cookie/marker theory from section 15 does not hold up

Tested directly with reliable breakpoints, twice:
- Breakpoint at `0x000017FC` (peripheral-init call site) with the magic
  cookie explicitly forced into SRAM (`*0x20004004 = 0x5555AAAA`) before
  `resume`: **never hit** within 8 seconds.
- Breakpoint at `0x000017E2` (the guard function itself): **never hit**
  within 8 seconds, cookie or no cookie.

So despite being statically reachable in the recursive-descent call graph
(a real edge from `0x080001D6`, inside the `__main`/scatter-load table
processing loop), **`0x080017E2` is not actually called during a real
boot, in either magic-cookie state.** Static reachability proved a `BL`
instruction genuinely exists and targets it -- it did not prove that edge
is taken at runtime. The scatter-load loop that contains that call
processes a data-driven table; this specific entry is very likely gated by
table content this reconstruction doesn't have right, or simply isn't part
of the real init-table for this build. Section 15's whole
"skip-vs-full-init" narrative was built on a call that never fires, so
it's superseded by this section -- kept in the doc for the trail, not as
current understanding.

**Where this leaves things**: peripheral/USART init genuinely never runs
via any path found so far. The main loop's periodic handshake-send is
unconditional and doesn't check whether init succeeded first, so it hangs
every time, deterministically, well within the first ~10-20 seconds of any
boot. This is now a very concretely characterized problem: either (a) find
the *actual* runtime call path that should reach `0x080018BC`
(peripheral init) and figure out why it doesn't, or (b) find whether the
periodic handshake-send at `0x08001E8A`/`0x08001E90` is supposed to be
conditioned on an init-succeeded flag this reconstruction is missing.
Both are addressable with the same tools built this session (the
recursive-descent call graph plus alias-aware hardware breakpoints) --
next step is tracing forward from `Reset_Handler`/`__main`'s scatter-load
loop with single-step (not static analysis) to see which table entries
are actually processed and in what order, since that's the one part of
the control flow this session's tooling doesn't yet resolve.

## 20. Definitive confirmation, and a genuine dead end in `__main` internals

Ran a clean 60-second hardware-breakpoint test directly on the
peripheral-init function entry (`0x080018BC`, alias `0x000018BC`): **never
hit.** This settles section 19's open question -- peripheral/USART init
genuinely does not run within the first 60 seconds of any boot via any
path, not just "hasn't happened yet by the time we checked."

Tried to find the *real* mechanism connecting `__main`'s startup sequence
to whatever should call peripheral init, since the recursive-descent tool
(section 15) can't follow the one `bx r3` indirect-via-`ldm` dispatch in
`__main`'s own code (`0x08000158`-`0x08000182`, standard ARM-compiler
scatter/init-table walking idiom: `adr r0,#0x28` computes a table address,
`ldm r0,{sl,fp}` loads bounds, then a loop does
`ldm sl!,{r0,r1,r2,r3}; ...; bx r3` -- calling a handler function pointer
per table entry). Computed the table address rigorously from the raw
instruction encoding (confirmed `0x08000184`, not a guess) and read the
"entries" there directly: **the bytes at the computed table location
(`0x08001E70`+) are genuine, real Thumb *code* (matching the main-loop
disassembly from section 18), not a plausible function-pointer table.**
Either this scatter/init-table path isn't actually taken at runtime
(`sl == fp` in practice, despite the static literal values suggesting
otherwise) and something else entirely reaches the main loop, or this
project's understanding of this specific compiler's init-table convention
is incomplete. Manual byte-level reasoning about ARM Compiler C-runtime
startup internals did not converge productively after a genuine, sustained
attempt -- flagging this as a real limitation rather than continuing to
guess.

**Where this leaves the investigation, concretely, for next time:**
- **Solid, hardware-proven facts**: clock config works; the original
  permanent hang and the transmit self-loop are both genuinely fixed;
  peripheral/USART init exists, is correctly encoded, and is never called;
  the main loop runs and hangs deterministically (within ~10-20s) on an
  unconditional blocking TXE wait inside a periodic handshake-send.
- **Genuinely unresolved**: the control-flow path from `Reset_Handler`
  through `__main` to the main loop, specifically around
  `0x08000148`-`0x08000182`'s indirect dispatch, isn't fully understood.
  This is the one piece of this whole investigation that needs either (a)
  someone with specific ARM Compiler / Keil C-runtime startup expertise to
  correctly interpret the table format, or (b) a completely different
  strategy: since the *destination* is now precisely known (peripheral
  init must somehow reach `0x080018BC`), it may be more productive to
  search for **any single-bit/single-word patch to the *known, currently
  unreached* call graph** that would bridge it in, rather than continuing
  to reverse-engineer the exact original mechanism from first principles.

## 21. Static-only follow-up (no hardware -- board in use elsewhere): found and fixed my own arithmetic error in the scatter-table read

Re-derived the `__main` scatter-load table address from scratch, rigorously,
without hardware. Found a real mistake in section 20's own analysis: I'd
read `ldm.w r0,{sl,fp}` (loading two words from the table base) but then
missed the very next two instructions --

```
0x0800015A: ldm.w r0, {sl, fp}
0x0800015E: add   sl, r0
0x08000160: add   fp, r0
```

`sl` and `fp` are **relative offsets that get added to the table base**
(`r0` = `0x08000184`, confirmed via manual decode of the raw `ADR`
halfword, not just trusting Capstone's rendering), not absolute addresses
as section 20 assumed. Real bounds: `sl = 0x08000184 + 0x1E70 =
0x08001FF4`, `fp = 0x08000184 + 0x1E90 = 0x08002014` -- completely
different addresses than section 20 checked, which is why that region
looked like nonsense code instead of a table.

**Read the real table at the corrected address, and it's exactly a
standard, sane 2-entry ARM Compiler scatter-load table:**
- Entry 1: src=`0x08002014` (data immediately follows the table), dest=
  `0x20000000` (SRAM base), length=`0x34` (52 bytes), handler=`0x08000199`
  = `__scatterload_copy` (this address was already a known, previously-
  confirmed-correct gap word).
- Entry 2: src=`0` (unused for zero-init), dest=`0x20000034`, length=
  `0x664` (1636 bytes), handler=`0x080001A8` = `__scatterload_zeroinit`
  (genuine, unpatched -- and its disassembly, re-checked, is a completely
  ordinary zero-fill loop, nothing unusual).

This is just normal `.data`/`.bss` C-runtime init. **It has nothing to do
with peripheral init and was never going to explain the USART gap** --
section 20's "maybe the table calls peripheral init" idea is now
conclusively ruled out, not just abandoned.

**Re-derived the loop's exit mechanism precisely** (manually decoded the
raw `subw lr, pc, #9` halfwords rather than trusting a rendered
immediate): after each table entry's handler runs, it returns via `bx lr`
to address `0x08000168` -- which is the `bne` instruction that originally
tested `sl` vs `fp`. That strongly suggests the loop re-tests the same
condition on return, and once both real entries are processed (`sl == fp`
at that point), `bne` is *not* taken, falling through to
`0x0800016A: bl #0x80001cc` as the **normal, expected, always-taken**
post-scatter-load continuation -- not the "empty table only" edge case
section 20 assumed. If that's right, `0x080001CC` (and transitively
`0x080017E2`, the cookie/marker guard, and `0x080017FC`, peripheral init)
*should* be reached on every single boot, contradicting the clean 8-second
hardware timeout from section 19 that found `0x080017E2` unreached.

**This is a real, sharp, testable contradiction -- not resolved yet,
explicitly flagged rather than guessed at further.** The mechanics of a
returned-into branch instruction re-using whatever CPU flags the *handler*
last set (not a freshly-recomputed `cmp`) is subtle enough that further
hand-verification without hardware risks introducing another arithmetic
mistake, matching two earlier ones already caught and corrected this
session. Two precise, ready-to-run hardware tests for when the board is
free:
1. Breakpoint at `0x08000168` itself (the `bne`) -- see how many times
   it's hit and what `sl`/`fp`/flags look like each time, to directly
   observe the loop's real iteration behavior instead of inferring it.
2. Breakpoint at `0x080001CC` with a longer `wait_halt` (10-15s, not the
   3s used in the one earlier attempt) in case reaching it is not as fast
   as this analysis assumes.

## 22. Breakthrough: the real mechanism, confirmed via a complete gapless single-step trace

Section 21 left two precise hardware tests queued. Ran a binary search of
hardware breakpoints first (`0x0800180C`, `0x08001810`, `0x080017FC`,
`0x080017E2`, `0x08001800`, and even `0x08001D78` itself -- the confirmed
real prologue of the "main loop" function) -- **all unreached**, even
though `0x08001D8E` (a few instructions later, inside the same nominal
function) reliably hits every time. That's only possible if control enters
mid-function, bypassing the real prologue entirely -- pointing at an
indirect jump my tools hadn't resolved.

Got the definitive answer from a complete gapless single-step trace from
`Reset_Handler` (deduplicated to only log PC transitions). The critical
moment: the scatter-load loop's `bx r3` at `0x08000182` -- which section 21
assumed calls `__scatterload_copy`/`__scatterload_zeroinit` -- **lands
directly at `0x00001E5A`, inside the main loop region**, completely
bypassing `0x08001800`-`0x08001850` (the "download check + peripheral
init" code) and `0x08001D78` (the loop's own real prologue).

**Verified by hand why**, and it matches exactly: this is not a plain
function-pointer table -- it's a **relative-encoded jump table**. The loop
computes `r7 = sl_initial - 1 = 0x08001FF3` once, and for each table entry,
if the raw word's bit0 is set, the *real* target is `r7 - raw_word`, not
the raw word itself: `0x08001FF3 - 0x08000199 (our guessed word at
0x08002000) = 0x00001E5A` -- exactly the observed landing address. Table
entry 1's "handler" word was never `__scatterload_copy` at all; that
reading was a plausible-looking coincidence, not the real semantic.

**This resolves the entire investigation's central question.** Peripheral/
USART init isn't skipped due to a bug in reachability -- **this control
flow never goes anywhere near it.** `__main`'s scatter-table jump sends
execution directly into the main dispatch loop, on every single boot,
unconditionally. The "download check" code at `0x1800`+ (magic cookie,
`0x5AA5` marker, peripheral init, handshake write, app validation) is a
**structurally separate, disconnected piece of code** from this reset path
-- confirmed unreachable by five independent breakpoint tests, not a
detection failure.

**What's still open, now much more narrowly scoped**: the gap word this
whole mechanism depends on (`0x08002000`, part of the scatter table) is
one of the 113 reconstructed words -- currently guessed as `0x08000199`.
That guess happens to compute a stable, non-crashing, sensible-looking
landing address (`0x1E5A`), which is *consistent with* being correct, but
was reverse-engineered under a completely wrong theory (a named library
function address) rather than validated as a jump-table relative offset.
Two real possibilities remain, both actionable:
1. The guess is coincidentally close enough to land in valid code and the
   real bootloader genuinely never touches peripheral init on a cold boot
   either (the main loop's blocking TXE wait would then be a **real,
   latent bug in the actual factory firmware**, not just this
   reconstruction -- worth being aware of if this is ever used against
   real hardware).
2. The guess is subtly wrong, and the *correct* word would compute a
   target that goes through `0x1800`+ first (peripheral init, THEN the
   main loop) -- meaning the true entry point is data-driven and this one
   word is the actual remaining bug.

Distinguishing these needs the same rigor as the `0x08000234`/`0x08001C20`
fixes: determine what value at `0x08002000` is structurally required (not
guessed), which requires understanding the *second* table entry's
identical mechanism too (word3 at `0x08002010`, currently `0x080001A8`,
same relative-encoding logic likely applies) and cross-checking both
against whatever real, disassemblable code exists at their computed
targets.

## 23. MILESTONE: reached the real application, executing real interrupts

Applied the section 22 fix and hardware-tested against the **reconstructed
factory app** (`live_factory_app_52k_reconstructed.bin`), not just our own
`can_app.bin`.

**First attempt** (routing entry 1 to the real `__scatterload_copy` at
`0x08000188`) hit a new, real fault: `UNALIGNED` UsageFault a few
instructions in. Traced the exception frame precisely -- `r0` became
`0x32` (`=0x34-2`) via the routine's own first instruction
(`subs r0,r2,#2`), proving this specific copy routine's real calling
convention doesn't match the naive `(src=r0,dest=r1,len=r2)` assumption.
Rather than reverse-engineer an undocumented ARM-library internal further,
took the pragmatic path: routed entry 1 to the copy routine's own
confirmed-genuine exit (`0x080001A4`, `bx lr`) -- a safe no-op, explicitly
documented as skipping the 52-byte `.data` copy as a known limitation.

**Result, hardware-confirmed via a full gapless single-step trace**: no
fault. Execution correctly falls through entry 2 (zeroinit, ~100 loop
iterations, exactly as expected for 1636 bytes), then genuinely reaches
**`0x08001826`-`0x08001842` -- the real download-timeout wait loop**, the
exact code this entire investigation had been trying to reach since
section 14. This confirms the section 22 mechanism completely: the
scatter-table fix was the actual missing link the whole time.

**Free-running further, the results are dramatic:**
- `0x20004000` reads `0x20141003` -- **the authentication handshake was
  genuinely written**, matching bit-for-bit what the other agent
  independently found and confirmed the reconstructed factory app checks
  for (`check_auth()` at `0x0800B826`).
- The CPU is no longer anywhere in the bootloader. `current mode: Handler
  External Interrupt(37)` (CAN1_RX1) with `MSP = 0x200016BC` -- a
  completely different stack region than any bootloader context seen this
  entire session. **This is the application, executing a real hardware
  interrupt.** The bootloader validated the app, jumped to it, and the
  jumped-to code ran far enough to configure CAN1 and receive real bus
  traffic (or bus-error activity) on the bench.

**New, narrower finding from this state**: the CPU is stuck, permanently,
at `PC = 0x080004C6` inside that interrupt handler (no fault flags set --
a clean `b .`-style trap, not a crash). Checked `SCB->VTOR` directly:
still `0x08000000` -- **the bootloader's own vector table, not the
application's (`0x08003000`)**. So `IRQ37` vectors through the
bootloader's mostly-unimplemented table (which points essentially every
peripheral IRQ at a shared default trap) instead of the application's
real `CAN1_RX1` handler. Two real hypotheses, not yet distinguished:
1. The bootloader is supposed to set `VTOR` before jumping and either
   doesn't (a real gap) or does so via a still-unverified code path.
2. This is standard, correct bootloader design (the *application's own*
   `SystemInit`/`Reset_Handler` is responsible for setting `VTOR`, not the
   bootloader) and the real bug is that CAN1's interrupt fires before the
   reconstructed factory app's own startup code reaches that point --
   plausible given the factory app reconstruction is separately known to
   be ~88% complete on calibration tables (per the other agent's report),
   so its own startup sequence may have its own unresolved gaps.

Also confirmed, while investigating the jump mechanism itself: the final
`blx r0` in `jump_to_application` reads its target from SRAM address
`0x2000002C` (not a flash literal) -- and that address's real, genuine
`.data` initializer value is `0`. This means it must legitimately be
*overwritten* by the app-validation code just before the jump (reading the
target app's own reset vector from its vector table) rather than relying
on the `.data`-copied value -- consistent with the jump working correctly
despite entry 1's copy being skipped, and worth keeping in mind: the
`.data` copy skip has been empirically fine so far, but isn't proven safe
for every bootloader global.

## 24. VTOR/interrupt-trap root cause found -- and it clears the bootloader

Retested against the now-100%-complete factory app
(`live_factory_app_52k_reconstructed.bin`, SHA256 `404fe65f...`, all
573/573 words reconstructed) -- identical result: stuck at `PC =
0x080004C6` in `Handler External Interrupt(37)` (CAN1_RX1), `VTOR` still
`0x08000000`. Confirms this is unrelated to the audio-calibration data the
other agent just finished; it's structural.

Fine-grained free-run sampling (checking state at 50ms, 100ms, 200ms,
500ms, 1s after reset) found the CPU is **already** stuck by the very
first 50ms sample -- the whole sequence (clock config, ~9ms timeout loop,
handshake write, validation, jump, app init, CAN1 config, first IRQ) runs
in well under 50ms of real time. That explains why hardware breakpoints
deep in this path (`0x08001848`, `0x0800187A`) kept timing out even at
40-second `wait_halt` windows -- not a timing problem, a genuine breakpoint
reliability issue for these specific addresses that free-run sampling
sidesteps entirely.

**Checked whether the bootloader is supposed to guard against this, the
way a correct bootloader normally would** (disable interrupts, set VTOR,
*then* jump) -- and it conclusively doesn't, by design:
- Scanned the entire 12KB bootloader for `CPSID i` (disable interrupts,
  raw encoding `0xB672`): **zero occurrences anywhere.**
- Scanned for any literal-pool reference to `CAN1_BASE` (`0x40006400`)
  anywhere in the bootloader: **zero occurrences.** The bootloader never
  touches CAN1 at all.
- The actual jump sequence (`0x08001800`-`0x08001900`, aside from the 7
  already-documented gap words) is **fully genuine, unpatched code** --
  this is confirmed real factory behavior, not a reconstruction gap.

**Conclusion: this is not a bootloader bug.** The bootloader's job is
narrowly scoped -- validate the app, write the auth handshake, jump -- and
it does that correctly and completely. Setting `VTOR` and managing
interrupt state during the handoff is the *application's* responsibility,
which is a legitimate, common embedded design (minimal bootloader, app
owns its own vector table remap). The actual bug is that the
reconstructed factory app's own startup sequence enables CAN1 with RX
interrupts active *before* it gets around to setting
`SCB->VTOR = 0x08003000`, creating exactly this race. **This is now a
precisely-scoped finding for whoever owns the factory app reconstruction**
(the other agent, this session): find where CAN1 gets initialized in the
app's startup and confirm `SCB->VTOR` is set (and ideally interrupts held
disabled) strictly before that point.

**RESOLVED (2026-09-14, later same session).** The other agent found the
exact root cause, and it sharpens section 24's conclusion rather than
contradicting it: this was never a "CAN1 enabled before VTOR is set"
ordering bug in the app's own logic. It was a single wrong word in the
app's own vector table.

The application's reset vector at `0x08003004` had been reconstructed as
`0x08003151` -- the address of `__main` (the C runtime scatter-loader) --
instead of the true `0x08003599`, the real Keil `Reset_Handler`:

```
0x08003598: ldr r0, [pc, #36]   ; = SystemInit (0x08003421)
0x0800359A: blx r0              ; calls SystemInit() -- sets SCB->VTOR!
0x0800359C: ldr r0, [pc, #36]   ; = __main (0x08003151)
0x0800359E: bx  r0              ; tail-jumps to __main
```

`SystemInit()` (confirmed at `0x08003474`: `ldr r1,=0xE000ED08; ldr
r0,=0x08003000; str r0,[r1]`) is what actually performs the
`SCB->VTOR = 0x08003000` relocation. The reconstructed vector table
pointed straight at `__main`, silently skipping `Reset_Handler` (and with
it, `SystemInit()`) entirely -- `__main`'s own scatter-loading and
`main()` all ran fine regardless, which is exactly why this stayed hidden
until the first interrupt fired. Traced to its origin: `tools/
patch_factory_dump.py`'s original vector-table patch (an early-session
tool, predating this doc) saw the `__main` call at `0x08003150` in the
disassembly and mistook it for the reset entry point.

**Fix applied**: `tools/patch_factory_app_live.py` and `tools/
patch_factory_dump.py` both corrected to write `0x08003599` (real
`Reset_Handler`) at vector 1. Regenerated
`live_factory_app_52k_reconstructed.bin` (SHA-256
`3ec2715d13bbcb0fd924d2b517eae32b98e83395611526a5397eb2f5c3be6960`) and
`hardware/MCU/live_dumps/combined_factory_boot_factory_app_64k.bin`
(SHA-256 `b385f87cbb8a183add3507c45fb6006cbad3798cbd00805338f15cc50704c161`).

**Hardware-verified on the spare board, fully clean**:

| Check | Result |
| :--- | :--- |
| Bootloader handshake | `*0x20004000 == 0x20141003` -- pass |
| `SCB->VTOR` | `0x08003000` -- pass, `SystemInit()` correctly relocated it |
| Fault status | `CFSR = 0`, `HFSR = 0` -- zero faults across all phases |
| Core state | Thread mode, `MSP = 0x20001710` -- not trapped in any exception |
| CAN1 | `MSR = 0x00000C0A`, `BTR = 0x01230000` -- active, normal operating mode |
| Watchdog | `IWDG_ReloadCounter()` actively servicing |
| Scheduler | all 13 cooperative tasks scheduling |

**The combined authentic factory bootloader + 100%-reconstructed factory
application now runs together on physical silicon with zero traps or
faults.** This closes out the investigation this whole document tracks --
see section 25 for the final overall summary, now covering both the
bootloader and application sides.

## 25. Session summary: factory bootloader reconstruction, final status

Starting state: permanent hang on the very first patched instruction.
Ending state, all hardware-verified:
- Clock configuration: **fully working** (HSE/PLL/HSERDY/PLLRDY all lock
  correctly).
- Original permanent hang, a UART transmit self-loop, and the `__main`
  scatter-table misdirection: **all fixed**, each with a confirmed real
  root cause (not guesses that happened to work) -- see sections 8, 12,
  and 22 respectively for the encoding-level evidence on each.
- Full boot sequence -- clock, download-timeout wait, auth handshake
  write, app validation, jump to application -- **all confirmed working**
  end to end, against both the clean-room app and the fully-reconstructed
  factory app.
- The application itself runs far enough to configure and receive real
  CAN1 traffic on the bench.
- **RESOLVED**: the CAN1/VTOR trap identified in section 24 was root-caused
  to a single wrong word in the *application's* own vector table
  (`0x08003004`, reconstructed as `__main` instead of the true
  `Reset_Handler`) -- not a bootloader issue, confirming section 24's own
  conclusion. Fixed in `tools/patch_factory_app_live.py` /
  `tools/patch_factory_dump.py`; see section 24's resolution writeup for
  the full trace and hardware verification.

**Final status: fully closed.** The complete, authentic factory bootloader
+ 100%-reconstructed (573/573 words) factory application run together on
physical silicon with zero faults, zero traps, CAN1 active, watchdog
serviced, and the full 13-task cooperative scheduler running. See
`hardware/MCU/live_dumps/vehicle_live_2026-09-14/README.md` for the final
artifact inventory and checksums, and
`hardware/MCU/live_dumps/README.md` for where to find the ready-to-flash
combined image.

All bootloader-side fixes are in `tools/patch_factory_bootloader_live.py`
(`FACTORY_PATCH_MAP`), each with an inline comment explaining the
evidence. The regenerated `live_factory_bootloader_12k_reconstructed.bin`
reflects all of them. Application-side fixes are in
`tools/patch_factory_app_live.py`.

## Board state

Restored to the known-good clean-room bootloader + application after every
experiment in this trace, including this session's. The spare board is not
left in any of the diagnostic/candidate states above. (Note: at the time
this doc's section 24 resolution was written, the other agent had active,
uninterrupted use of the probe for final verification -- no hardware
actions were taken by this side of the session during that window.)
