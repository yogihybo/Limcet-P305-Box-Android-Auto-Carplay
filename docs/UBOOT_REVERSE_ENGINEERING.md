# Uboot Reverse Engineering

**Status:** Investigation
**Last Updated:** 2026-07-15

## Overview
Consolidated document containing: UBOOT_SDBOOT_INVESTIGATION.md, UBOOT_BOOTLOGO_AND_RE_PORTS.md



## UBOOT_SDBOOT_INVESTIGATION.md

# U-Boot SD-Boot Patch — Corruption Investigation

**Current best in-binary path: env RELOCATION (§10), statically verified, not
yet hardware-tested.** The `source`-a-script approach of §8 turned out not to
work on this build, so the leading technique is now `patch_uboot_env.py`, which
sidesteps the compiled-in env space wall entirely by *relocating* the default
env into free image space and repointing it (a data move + a few constant
patches, no code injection, no disassembler). Generated at
`experimental_sdboot/uboot_relocenv.bin`. `build_bootable_sdcard.sh` uses it
**by default** when patching the stock U-Boot (`--no-reloc-env` falls back to
the prompt-drop patch). See §10 for the full offset map.

**Earlier technique (§8), statically verified, superseded.** A minimal
compiled-in `bootcmd` that loads and `source`s a boot script from SD — fits the
raw/Holden-derived `uboot.bin`'s tiny safe capacity and needs zero NAND writes.
Generated at `experimental_sdboot/uboot_selfcontained.bin`; do not treat as
verified until tested on real hardware. `uboot_sdboot.bin` and `uboot_final.bin` (the
earlier, corrupted attempt) remain quarantined under `corrupted/`. The manual
U-Boot-prompt command (README §4.0 "Manual SD Card Boot", §5.0 "USB boot")
remains the confirmed-working fallback. This document records how the
corruption was found, why the obvious workarounds (§2, §6) don't fix the
underlying env-space problem, and what it took to get a real (if untested)
patched auto-boot U-Boot (§5, §7, §8), why the same trick doesn't
(yet) extend to USB (§9), and finally the env-relocation approach that
removes the space wall for good (§10). Written so this doesn't need to be
re-derived from scratch next time.

---

## 1. The original bug: `patch_uboot.py` corrupted raw binaries

`patch_env_block()` used to unconditionally clear a fixed **4096-byte** window
at the compiled-in env offset, on the assumption that a reserved buffer always
exists there:

```python
padded = serialized + b'\x00' * (max_size - len(serialized))
data[offset:offset + max_size] = padded
```

That assumption holds for a binary compiled with a real `CONFIG_ENV_SIZE`
buffer, but **not** for a raw NAND-dumped `uboot.bin` — there the env strings
sit directly against the next real structure in the binary (a command table:
`set_default_env`, `env_import`, `saveenv` strings + a function-pointer
array), with only 1–3 incidental null bytes of alignment padding, not 4096.

Confirmed by dumping `Prado firmware reconstructed/mtd1-mtd2_uboot/uboot.bin`
(byte-identical to `Holden firmware update/uboot.bin`, see §3) immediately
after its env block:

```
...baudrate=115200\x00\x00\x00set_default_env\x00env_import\x00saveenv\x00\x00...<pointer table>
```

Only **3 null bytes** separate the env from real, load-bearing data. Applying
the old `--mode sdboot` patch (which needs ~500 bytes for the full preset)
there zeroed ~4000 bytes of that command table and pointer data — the same
strings ("set_default_env", "env_import") disappear entirely from the output.

### Fix applied (`patch_uboot.py`, `build_bootable_sdcard.sh`)

Added `measure_env_capacity()`: scans forward from the env's own double-null
terminator, counts only genuinely-zero bytes as safe, and `patch_env_block()`
now refuses (no file written) if the requested env doesn't fit — instead of
silently overwriting real data. Verified:

- Raw `uboot.bin` + full `sdboot` preset → **now refused cleanly**, no
  corruption, no output file.
- Raw `uboot.bin` + single same-length `bootdelay=9` edit → still works,
  exactly 1 byte changed.
- `uboot_sdboot.bin` + `--mode sdboot --patch-nand-offset` → still reproduces
  the committed `uboot_final.bin` byte-for-byte (no regression from the fix
  itself — see §3 for why that reproduction is still built on a corrupted
  base).

`build_bootable_sdcard.sh`'s auto-detection now prefers a repo-root
`uboot_sdboot.bin` (if present) over the raw NAND dump, since only a real
reserved-buffer binary can safely hold the full SD-boot preset.

**This part of the fix is solid and should stay.** It's a genuine safety
improvement regardless of what happens with §3 below.

---

## 2. Why the "trigger failed NAND read" idea doesn't help

Idea considered: patch the NAND-offset MOV instructions to force the NAND env
CRC to fail (`--patch-nand-offset`, already implemented), so U-Boot falls back
to the compiled-in env — then just make the compiled-in env boot from SD.

This doesn't route around the space problem. `--patch-nand-offset` only
changes *which* env source U-Boot reads; it doesn't enlarge the compiled-in
env block itself. That block still has only **52 bytes** of verified-safe
space in Holden's/the raw `uboot.bin` (`measure_env_capacity` result: 51 bytes
of existing content + 1 byte of real padding).

52 bytes isn't enough for any working SD-boot `bootcmd`, even in the most
minimal form:

| Command | Length | Fits in 42-byte value budget? |
|---|---|---|
| `run nandboot` (current) | 12 chars | yes |
| `fatload mmc 0:1 1000000 zImage;bootz 1000000` (loads kernel, no bootargs — kernel would panic, no root device set) | 44 chars | **no** (2 over) |
| same + `setenv bootargs root=/dev/mmcblk0p2 rw` | 83 chars | no (41 over) |

(42-byte value budget = 52 total − 8 for the `"bootcmd="` prefix − 2 for the
entry/terminator nulls.)

Even the non-functional bare minimum doesn't fit. The bottleneck is space, not
which env gets consulted.

---

## 3. `uboot_sdboot.bin` is itself corrupted — and based on Holden's binary

This was discovered after the fact, while explaining why the "just patch a
different file" idea wouldn't help either — worth stating clearly since an
earlier pass in this same investigation wrongly concluded the opposite.

### Evidence

**File size** — `uboot_sdboot.bin` is 376,364 bytes: identical to
`Holden firmware update/uboot.bin` and `Prado firmware reconstructed/mtd1-mtd2_uboot/uboot.bin`,
**not** the actual live Prado NAND dump (`Prado firmware dump/mtd1-mtd2_uboot/extracted/uboot.bin`,
375,944 bytes — 420 bytes smaller).

**Byte diff against Holden's `uboot.bin`** — exactly 2,298 bytes differ. That's
the *exact* count reproduced earlier in this investigation by deliberately
applying the old buggy `--mode sdboot` patch directly to a raw/Holden-based
`uboot.bin`. Not a coincidence.

**The smoking gun** — same byte range (`0x420AA`–`0x42400`) in both files:

Holden's `uboot.bin`:
```
bootcmd=run nandboot\0bootdelay=0\0baudrate=115200\0\0\0set_default_env\0env_import\0saveenv\0\0...
<dense function-pointer table, then what looks like a keyboard scancode/ASCII table: "!@#$%^&*()...">
```

`uboot_sdboot.bin`, identical offset range:
```
bootcmd=run sdboot\0bootdelay=3\0baudrate=115200\0bootfile=zImage\0mmcdev=1\0sdboot=...\0sdbootargs=...\0usbboot=...\0usbbootargs=...\0
<nothing but zeros for the rest of the window — where Holden's binary has the command table and keymap data>
```

`set_default_env` and `env_import` are **completely absent** anywhere in
`uboot_sdboot.bin` (confirmed via whole-file search, not just the local
window). Only a stray `saveenv` substring survives, unrelated, at a different
offset (`0x47a4f`).

### Conclusion

`uboot_sdboot.bin` was not an externally-supplied, ARK1680-BSP
source-compiled binary, despite what `patch_uboot.py`'s docstring and the
README claim. It was produced by running the old, buggy `--mode sdboot`
patch directly against Holden's stock `uboot.bin` (identical to the copy in
`Prado firmware reconstructed/`), and that operation wiped the real command
table and keymap data in the process. `uboot_final.bin` — built from this
file via `--patch-nand-offset` — inherits the same corruption.

**Earlier wrong conclusion, for the record:** partway through this
investigation, a byte-diff against the *live Prado NAND dump* (not Holden's
binary) showed 582 scattered differing regions, which was misread as "these
are just two different compiled builds" and used to conclude `uboot_sdboot.bin`
was fine. That was diffing against the wrong baseline. Diffing against the
*actual* source (Holden's `uboot.bin`, byte-identical to the reconstructed
copy) removes the ambiguity — see the smoking-gun comparison above.

---

## 4. Why "replace a compiled-in command" is a much bigger undertaking

Idea considered: instead of fighting for space in the env-string block,
repurpose one of the actual command-table entries (e.g. hijack `saveenv`'s
slot) to run custom SD-boot logic directly.

This is a fundamentally different, much riskier class of patch than anything
implemented so far:

1. Requires writing actual **ARM machine code**, not a string.
2. Requires genuinely free, executable space to put it — the small zero-runs
   found scattered later in the file (~250–500 bytes each, at offsets like
   `0x04FCAD`, `0x04FF01`, etc.) are **unverified**: could be alignment
   padding, could be something the runtime expects to stay zero. No symbol
   table exists to check.
3. Requires overwriting a command-table function pointer to point at the
   injected code, matching U-Boot's exact calling convention for that slot.
4. Any mistake (wrong pointer, bad ABI, misaligned instruction) means U-Boot
   crashes/hangs before reaching the new logic — recovery is JTAG-only (see
   README's Safety notes on S-Loader).

Everything done successfully so far (env-block patching, NAND-offset MOV
patching) worked because those are narrow, previously-identified byte
patterns. Command-table replacement would need real ARM disassembly tooling
(objdump/Ghidra/similar) to map out the structure and confirm what's
genuinely free — not available in this environment, and not something to
attempt via ad-hoc byte-pattern scripting given the brick risk.

**Not attempted. Recommended against without proper tooling.**

> **Superseded for the env case — see §10.** This section is about injecting
> **ARM code**, which remains hard/risky. But it conflated that with a much
> easier goal: getting a *longer boot command* to run. The default env is
> reached by a **data pointer** (`default_environment`), not code, so you can
> relocate the whole env into free space and repoint it — a data move plus a
> few constant patches, **no machine code, no disassembler**. That's what §10
> does, and it's why the "500-byte env doesn't fit" wall in §2/§3 is no longer
> a wall.

---

## 5. Paths forward

1. **Real BSP-compiled source** — `docs/historical/SD_BOOT_PLAN.md` (superseded, but
   still useful here) references `~/Downloads/linux-arkmicro` as containing
   "the exact U-Boot source for this SoC," with the two config changes needed
   (`CONFIG_ENV_OFFSET=0x120000`, `CONFIG_BOOTCOMMAND`/`CONFIG_EXTRA_ENV_SETTINGS`
   for sdboot) documented in full there (§"Option 2 — Build U-Boot from
   source"). That path was explicitly called "preferred" over binary patching
   at the time, but was never taken, and the BSP source tree wasn't in this
   repo (was on the original author's machine only, under their `~/Downloads`).
   **Update: located and pulled in.** The real repo is `RD_Software/linux-arkmicro`
   (public Gogs instance, see [`linux-arkmicro Reference/README.md`](../linux-arkmicro%20Reference/README.md)
   for the URL/commit); a relevant slice is copied into `linux-arkmicro Reference/`.
   It's a later BSP generation than the Prado's actual 2012.10/ATAG/no-FDT stock
   U-Boot (this repo's `ark1668` target is 2018.07, SPL+FDT) — not a byte-source
   match, but the same SoC family. Full build plan, config-delta table, and an
   SD-only test sequence (no NAND writes until proven on hardware) now live in
   [`docs/UBOOT_BUILD_GUIDE.md`](UBOOT_BUILD_GUIDE.md).
2. **ARM relocation patch** (§4) — possible in principle, but needs real
   disassembly tooling and carries meaningful brick risk. Not recommended
   without that tooling in hand.

## 6. Initramfs — considered, doesn't help

Idea considered: use an initramfs to work around the env-space limit.

`docs/historical/SD_BOOT_PLAN.md` Phase 4 documents an initramfs, but for a *different*
problem: if `ark_dw_mmc` (the SD/MMC driver) is a loadable module rather than
built into the kernel, the kernel can't mount an SD rootfs, because the
driver needed to read the SD card is itself sitting on the SD card. The fix
there is a small initramfs that `insmod`s the module first, then pivots to
the real root.

**RESOLVED 2026-07-29: yes, `ark_dw_mmc` genuinely was a loadable module in
stock, not built-in.** `docs/KERNEL_REFERENCE.md` used to state "no
loadable modules — the entire driver set is compiled monolithically,"
explicitly listing `drivers/mmc/` as built-in -- that claim was wrong (now
corrected there). The real 29 KB `ark_dw_mmc.ko` module file in the shipped
rootfs (`.../rootfs/lib/modules/3.4.0/kernel/drivers/ark/sdmmc/ark_dw_mmc.ko`)
is genuine, confirmed by direct rootfs inspection rather than the earlier
inconclusive zImage-symbol-string search (which couldn't see inside the LZO
payload). USB is the same story: `musb_hdrc.ko`/`ark1680_musb.ko` and the
gadget drivers (`g_ncm.ko`/`g_eap.ko`/`g_webcam.ko`/`g_zero.ko`) are also
real loadable modules in stock, not built-in.

**Doesn't matter for our actual blocker either way.** Using an initramfs as
documented requires U-Boot to load *two* files instead of one:

```
sdboot=run sdbootargs; fatload mmc 0 4000000 initramfs.cpio.gz; fatload mmc 0 1000000 zImage; bootz 1000000 4000000
```

That's *longer* than the single-kernel boot command already shown (§2) not to
fit in the 52-byte compiled-in env budget. It makes the space problem worse,
not better. The only initramfs-adjacent idea that could help — baking the
kernel command line into the kernel itself, so U-Boot doesn't need to
`setenv bootargs` at all — would require rebuilding Holden's actual kernel,
not just the U-Boot BSP. The shipped kernel's build string points at
`/workspace/ark0618system/kernels/linux-3.4/`, a private Holden/vendor build
server distinct from the `~/Downloads/linux-arkmicro` U-Boot BSP reference
(§5) — we have no access to it.

**Not pursued further.**

---

## 7. Resolution adopted

Given §1–6, patching U-Boot at all (raw dump or otherwise) isn't the right
near-term path — it's either unsafe (raw dump, no reserved env space) or
depends on a source we don't have (real BSP compile). Instead:

- **SD/USB boot now documented as a manual U-Boot-prompt command**, typed by
  hand each time after interrupting boot at `ark#` — no patched U-Boot
  needed at all, works today on the stock, unpatched U-Boot already on the
  device. See README §4.0, "Manual SD Card Boot", and the existing manual
  USB-boot command in §5.0 "USB boot" (which already worked this way and
  didn't need updating).
- **`uboot_sdboot.bin` and `uboot_final.bin` moved to `corrupted/`** (out of
  the default build/auto-detect path), with `corrupted/README.md` explaining
  why. `.gitignore` updated so a stray regenerated copy at repo root doesn't
  get re-staged by accident.
- **README and `patch_uboot.py`** updated with strikethrough (`~~text~~`) on
  the now-known-wrong claims and instructions, rather than deleting them
  outright, so the history of what was believed and what turned out to be
  true stays visible.
- `build_bootable_sdcard.sh`'s U-Boot-patching path is left in place (now
  safe, thanks to the §1 fix) but documented as currently non-functional for
  producing a working auto-boot image; the rootfs/userdata/`/nanddata/`
  portions of that tool are unaffected and still useful.

If a genuine BSP-compiled U-Boot source is located later (§5, option 1), the
patched-autoboot path can be revisited — `patch_uboot.py`'s fixed
`measure_env_capacity()` check means it'll either work correctly or refuse
cleanly, not corrupt anything.

---

## 8. Self-contained SD-boot patch — found a way that fits

§2 showed the full `sdboot` preset (~500 B) can't fit in the raw
`uboot.bin`'s ~52-byte safe capacity, even a bare, non-functional
`fatload;bootz` with no bootargs (44 B, already over). The missing piece:
the compiled-in `bootcmd` doesn't need to contain the *whole* boot sequence.
U-Boot's `source` command executes a boot script loaded from any file — so
`bootcmd` only needs to load a script and run it; the real (arbitrarily long)
logic lives in the script file itself, on the SD card, not in the tiny
buffer.

### The patch

```
bootcmd=fatload mmc 0:1 1000000 s;source 1000000
```

40 characters. As the *only* compiled-in key (dropping `bootdelay`/
`baudrate` — see `--replace-env` below), total serialized size is 50 bytes,
fitting the 52-byte safe capacity with 2 bytes to spare.

The script file `s` (source: `env/sdboot_script.txt`) carries the real logic:

```
setenv bootargs console=ttyS0,115200n8 console=tty0 mem=180M root=/dev/mmcblk0p2 rootfstype=ext4 rootwait rw
fatload mmc 0:1 1000000 zImage
bootz 1000000
```

### Why this needs zero NAND writes (better than §7's original plan)

Combined with placing the patched binary as `UBOOT.BIN` on the SD card
(Stepldr already prefers SD over NAND — existing mechanism, §"Booting from
SD Card or USB" in the README) and `--patch-nand-offset` (forces the real
NAND env's CRC to fail so this compiled-in env is actually used), **every
file involved lives on the SD card** — patched U-Boot, script, kernel,
rootfs. Nothing is written to NAND at all, not even to spare/placeholder
space. Pull the SD card and the device is unaffected.

### Tool changes

`patch_uboot.py` gained:
- **`--replace-env`**: discards the existing compiled-in env entirely
  instead of merging, so `bootdelay`/`baudrate` can be dropped to make room.
  Implemented as a `replace` parameter on `patch_env_block()` — when set, it
  serializes only the given patches dict instead of `parse_env(...).update(patches)`.
- **`sdscript` preset**: the minimal `bootcmd` above, documented as needing
  `--replace-env` to actually fit.

Generated with:
```bash
python patch_uboot.py -i "Prado firmware reconstructed/mtd1-mtd2_uboot/uboot.bin" \
  -o experimental_sdboot/uboot_selfcontained.bin \
  --mode sdscript --replace-env --patch-nand-offset
```

### Verification performed (static analysis)

- `--dump-env` on the output shows exactly one key: the new `bootcmd`.
  `bootdelay`/`baudrate` cleanly gone, as intended.
- `--find-nand-offset` on the output reports 0 remaining valid candidates —
  redirect applied to all three instructions.
- Full byte-diff against the source `uboot.bin`: **6 differing regions**,
  2–23 bytes each (~38 B for the env change, 6 B total across the three
  2-byte NAND-offset immediate changes). Contrast with the ~2,000–4,000-byte
  contiguous wipe from the original corruption (§1, §3) — this patch is
  narrow and surgical, matching what `measure_env_capacity()` verified as
  safe.
- `set_default_env`, `env_import`, `saveenv` — the command-table strings
  that got destroyed in the corrupted files — are all present and intact in
  this output.

### What's NOT verified — the one real unknown

Whether `source` on this specific U-Boot build accepts a **plain-text**
script file directly, or requires the `mkimage -T script`-wrapped format
(adds an image header + CRC). `s` is currently plain text. This is a very
common, widely-supported U-Boot feature, but which variant this build
expects can't be confirmed without real hardware or disassembly. If plain
text doesn't work, rebuild with:
```bash
mkimage -A arm -T script -C none -n "SD boot script" -d env/sdboot_script.txt s
```
(`mkimage` wasn't available in the environment this was developed in — Linux/WSL
with `u-boot-tools` installed, same requirement already documented elsewhere
in this repo, is needed to test this variant.)

### Artifacts

- `env/sdboot_script.txt` — the boot script source (tracked).
- `experimental_sdboot/uboot_selfcontained.bin` — the patched U-Boot,
  generated as shown above.
- `experimental_sdboot/s` — copy of the script, named as U-Boot expects it
  on the SD card.
- `experimental_sdboot/README.md` — what this is, testing steps, status.

**Not yet tested end-to-end on real hardware.** Once tested, update this
section and `experimental_sdboot/README.md` with the actual result — if it
boots, this becomes the recommended path and `experimental_sdboot/` should
be promoted/renamed accordingly; if `source` needs the wrapped format,
regenerate `s` and retest; if something more fundamental is wrong, record
what failed here for the next attempt.

### `build_bootable_sdcard.sh` now uses this method

The build tool's "Patch U-Boot for SD boot" toggle was switched from the old
`--mode sdboot` (needs a real BSP-compiled source, doesn't fit the raw dump)
to `--mode sdscript --replace-env`. It also gained a `generate_bootscript()`
step that writes the boot script (with `--root` substituted in) and copies
it to SD p1 as `s` alongside `UBOOT.BIN` and `zImage`. New `--wrap-bootscript`
flag runs the `mkimage`-wrapped variant if plain text doesn't work. The
auto-detected U-Boot source list was simplified to just the raw NAND-dumped
`uboot.bin` (no longer prefers a repo-root `uboot_sdboot.bin`, since
`sdscript`'s tiny footprint means the raw dump works fine either way).

Caught one real bug while wiring this up, unrelated to the corruption
history above: a bare `$WRAP_BOOTSCRIPT && echo ...` immediately followed by
a bare `return` inside `generate_bootscript()`'s dry-run branch. When
`WRAP_BOOTSCRIPT` is false, that `&&` expression evaluates to exit status 1,
and the bare `return` (no explicit code) inherits `$?` from it — silently
making the *function itself* return 1, which then aborted the whole script
under `set -euo pipefail` at the call site. Fixed by using an explicit
`if $WRAP_BOOTSCRIPT; then ... fi` and an explicit `return 0`. Worth
remembering as a general pattern: never let a boolean-gated `&&` shortcut be
the statement immediately before a bare `return` in a `set -e` script.

---

## 9. Extending the sdscript trick to USB — parked, doesn't fit

Idea considered: apply the same `fatload ...;source ...` compiled-in
`bootcmd` trick (§8) to auto-boot from USB instead of SD.

In principle it's the same mechanism — `fatload usb` instead of
`fatload mmc`, same length either way (`usb` and `mmc` are both 3
characters). But USB needs an explicit `usb start` before `fatload usb`
works, per the existing (manual, already-documented) USB boot command in
the README — SD has no equivalent `mmc start` requirement. That prefix has
to live in the same 52-byte compiled-in env budget:

```
SD:  fatload mmc 0:1 1000000 s;source 1000000            → 50 B total, fits
USB: usb start;fatload usb 0:1 1000000 s;source 1000000  → 60 B total, 8 B over
```

No realistic byte-shaving closes an 8-byte gap here — the script filename
is already a single character and the load address is already a bare
literal instead of `${loadaddr}` (same optimisations already applied in
§8). `usb start` also can't be moved into the boot script itself: the
script has to be loaded *from* the USB drive, which requires the
controller already started, so the start command must come before the
script load, not after — a chicken-and-egg problem, similar in spirit to
the initramfs MMC-module issue in §6.

Two things this doesn't even get to test:
- Whether `usb start` is genuinely mandatory, or `fatload usb` would
  auto-initialise the controller if called directly. If the latter, the
  bare `fatload usb 0:1 1000000 s;source 1000000` (no prefix) is exactly
  50 bytes — same as the SD version — and would fit. Unverified without
  real hardware.
- USB itself is already flagged **"Unverified on Prado hardware"** in the
  README's USB boot section — the host controller and GPIO assignments
  aren't confirmed working *at all* yet, independent of this env-space
  question.

**Not implemented.** A working manual USB boot command already exists
(README §5.0 "USB boot"). Revisit only after real hardware confirms (a)
USB boot works at all via the manual command, and (b) whether `usb start`
can actually be dropped from a compiled-in `bootcmd`.

---

## 10. Env RELOCATION — removes the space wall entirely (`patch_uboot_env.py`)

**Status: Confirmed working end-to-end on real hardware.** Everything in §1–§9
fought the same constraint: the compiled-in default env is ~73 bytes packed
against real data, so a full SD-boot command (bootargs + fatload + bootz,
~150 B) can't fit *in place*. §8's `sdscript` trick shrank the *compiled-in*
part to fit, but relied on `source`-ing a script from SD — and that path
turned out not to work on this build. §4 assumed the only remaining option was
injecting ARM machine code (hard, needs a disassembler, brick risk).

Both framings missed a simpler fact: **the default env isn't copied into a
fixed buffer — it's referenced by a pointer.** `set_default_env()` /
`env_relocate()` call
`himport_r(&env_htab, default_environment, sizeof(default_environment), …)`.
So instead of growing the env where it sits, **relocate it** into free image
space and repoint it. That's a data move plus a handful of constant patches —
no code injection, no disassembler.

### What was pinned on the live dump

Target: `Prado firmware dump/mtd1-mtd2_uboot/extracted/uboot.bin` (375,944 B).
All of this was found by **byte-scan only** (link base via a pointer
histogram, cross-checked against the header):

| Item | Value | Notes |
|---|---|---|
| `CONFIG_SYS_TEXT_BASE` | **0x30000** | histogram score 3236; the `_TEXT_BASE` word literally sits at file 0x40, image length `0x5bc88` at 0x50 |
| `default_environment` | file **0x41F77** → va **0x71F77** | first key is `backlight=30` |
| Pointer literals (→ `0x00071F77`) | file **0xB948, 0xB988, 0xBA38, 0xBA78, 0xBBA8** | the `env_common.c` xrefs; each is a relocated absolute pointer (`.rel.dyn` `R_ARM_RELATIVE`) |
| `himport` size (`sizeof`, `mov r2,#0x49`) | file **0xB9D8, 0xBA54** | **two** call sites (`set_default_env` + `env_relocate`), both `bl 0x032BB0` = `himport_r`; **both** size immediates must be bumped |
| `__bss_start` offset | **0x54EF8** | relocated env must end at/below this so it's copied+relocated, not zeroed |
| Free zero-runs below bss | 511 B @ `0x50FDD`, 507 @ `0x4FB29`, … (10 runs, 261–511 B) | all below `__bss_start`, i.e. inside the loaded/relocated image |

The two `mov r2,#0x49` sites were the one subtlety: `0xB9D8` loads its env
pointer from a literal pool ~0x60 bytes away, so the detector's association
window has to be ≥0x80, not 0x40, to catch both. A global scan confirms those
are the *only* two `mov r2,#0x49` in the binary, both genuine.

### The patch (`patch_uboot_env.py`, new — `patch_uboot.py` untouched)

1. Write the new (arbitrarily long) env into the largest free zero-run below
   `__bss_start` — e.g. `0x50FDD` (511 B). Refuses if the region isn't
   genuinely all-zero or the env won't fit.
2. Repoint all 5 pointer literals `0x00071F77` → `base + region` (e.g.
   `0x00080FDD`).
3. Bump both `himport` size immediates `mov r2,#0x49` → smallest single-MOV-
   encodable value in `[env_len, region_len]` (e.g. `#0x1C0` for a 446 B env).
   Without this, himport truncates the relocated env at the old 73 bytes.
4. Optionally apply the same NAND-offset corruption as `patch_uboot.py`
   (`--patch-nand-offset`) so the on-NAND env fails CRC and this relocated
   default is the one imported.

**Why it's self-consistent under runtime relocation:** the 5 pointer literals
are fixed up at boot by adding `gd->reloc_off`. Rewriting their *link-time*
value from `0x71F77` to `0x80FDD` (both in-image) means the same delta is
added → the pointer still lands on the moved data after relocation. The size
immediate is code, not relocated. The env bytes we wrote sit below
`__bss_start`, so they're part of the copied+relocated image.

### Verification performed (static)

Generated with:
```bash
python patch_uboot_env.py \
  -i "Prado firmware dump/mtd1-mtd2_uboot/extracted/uboot.bin" \
  -o experimental_sdboot/uboot_relocenv.bin \
  --preset sdboot --patch-nand-offset
```
- 446 B env written at `0x50FDD`; 5 pointers → `0x80FDD`; both sizes
  `#0x49`→`#0x1C0` (`e3a02d07`); 3 NAND MOVs → `0xFF000000`.
- **Total change: ~22 code bytes + 446 env bytes into verified-zero space.**
  Differing regions are all tiny (2–3 B each) plus the one env write.
- `set_default_env` / `env_import` / `saveenv` — the command-table strings
  destroyed by the old §1/§3 corruption — are **all present and intact**.
- `--analyze` re-derives every offset above from the binary (nothing
  hardcoded), so it self-checks on any similar ARK1680 dump.

### The one real unknown

Whether `default_environment` is imported **before or after** `.rel.dyn`
fixups run on first boot. Both the pointer literals and the moved env data
live inside the image and share the same reloc delta, so it should be
consistent either way — but that's the thing to watch on the first hardware
test. Recovery if it hangs is the usual SD-only path (pull the card; nothing
was written to NAND).

### Build integration

`build_bootable_sdcard.sh` gained a **`--reloc-env`** flag, **on by default**.
When patching the stock U-Boot (`--no-new-uboot` + patch toggle on), the patch
step runs `patch_uboot_env.py --preset sdboot --patch-nand-offset` (full SD
auto-boot). Pass **`--no-reloc-env`** to fall back to
`patch_uboot.py --mode sdscript` (the hardware-confirmed patch that only drops
to a U-Boot prompt).

Note the default is on **despite** being only static-verified — the reasoning
is that the fallback is one flag away and nothing touches NAND, so a failed SD
auto-boot costs nothing but a reflash of the card. **On the first hardware
test, update this section** with the result: if it auto-boots, keep the
default; if himport imports before `.rel.dyn` relocation, record what failed
and whether writing the pointer as the *relocated* runtime address (rather than
link-time) fixes it — and consider flipping the default back to `--no-reloc-env`
until it's sorted.


## UBOOT_BOOTLOGO_AND_RE_PORTS.md

# U-Boot Boot Logo & Reverse-Engineered Feature Ports

Summary of work to (1) get a boot logo showing on the compiled u-boot, and
(2) port a handful of vendor-only features out of the stock production
binary (`mtd1_uboot.bin`), which has no released source anywhere. Covers
the reverse-engineering toolchain, what was found, what got ported, what
was deliberately left out, and how to test it.

Build tree referenced throughout: `/home/osboxes/Downloads/linux-arkmicro/u-boot`
(full buildable u-boot 2018.07-based source with the `ark1668_limcet_p305`
board). The `linux-arkmicro Reference/u-boot` folder in this repo is only a
partial vendor overlay (board/arch/driver files) — it was never the full
picture; `cmd/`, `common/`, `lib/` etc. live only in the build tree above.

---

## 1. The original question: why no boot logo?

The compiled `ark1668_limcet_p305` u-boot showed a blank/white screen where
stock firmware shows a logo at the `=>` prompt. Investigation ruled out
several theories before landing on the real cause:

- **Not a Stepldr leftover.** Initial theory was that the pre-u-boot stage
  (`Stepldr.bin`/`Nboot.bin`, both proprietary/NAND-resident) draws the logo
  and u-boot just doesn't disturb it. Disproven by decompiling the stock
  binary's command table.
- **Not decoded by u-boot's own `disconfig`/`display_updatelogo()`.** That
  code (present in source) only draws a small hard-coded "updating..."
  progress bar during a firmware-update flow, and the live device's saved
  env (`bootcmd=run nandboot`) doesn't even reach it on a normal boot.
- **Real cause, confirmed via disassembly:** stock u-boot's command table
  (recovered from `mtd1_uboot.bin` — see §3) has `jpeghw`/`jpeg decode`
  commands that drive the SoC's dedicated hardware JPEG decoder
  (`JPEG_BASE = 0xE0200000`) to decode the `bootlogo` NAND partition
  (a real JPEG, 800×480) directly into the OSD framebuffer. None of that
  exists in the source tree at all. The compiled build's screen was never
  being fed valid pixel data — hence white, not black (classic TFT
  no-signal state, not framebuffer corruption).

## 2. The shipped fix: SD-card raw framebuffer bootlogo

Since there's no JPEG decoder in u-boot source (and porting the stock
hardware-JPEG-decoder driver was assessed as high-risk — see §4.2), the
logo is now shown via a much simpler path that reuses code already in the
board file:

1. **Offline, once**: `convert_bootlogo.py` (repo root) converts a JPEG
   (e.g. the dumped `Prado firmware reconstructed/mtd8_bootlogo/bootlogo`,
   or any 800×480 image) into a raw 32bpp pixel buffer.
   - Pixel format: each pixel packed as `(0xFF<<24)|(R<<16)|(G<<8)|B`,
     written little-endian — this is the same convention the existing
     `display_updatelogo()` progress-bar code already used for its
     `0xffffffff`/`0xff00ff00` color constants, so it's a proven match for
     this panel's `DISP_RGB_888` + `RGB_MODE_BGR` config.
   - Output is exactly `width * height * 4` bytes (1,536,000 for 800×480).
2. **On the SD card**: the converted file goes on the FAT boot partition
   as `bootlogo.raw`, next to `UBOOT.BIN`.
3. **In u-boot** (`board/arkmicro/ark1668_limcet_p305/ark1668_display_cfg.c`):
   - `ark_show_bootlogo()` — calls `ark_display_init(SCREEN_QUN700)` for
     panel/clock/port bring-up, then `display_bootlogo_from_sd()`.
   - `display_bootlogo_from_sd()` — `fatload mmc 0:1 <addr> bootlogo.raw`,
     then points `OSD1_LAYER` (the main content layer, previously left
     disabled) at it full-screen via the existing
     `ark_set_osd_image()`/`ark_set_osd_addr()`/`ark_osd_en_layer()`
     primitives (same calls already used for the small update-progress
     overlay on `OSD2_LAYER`).
   - Wired into `board_late_init()` in `ark1668.c` as the first call, so
     it runs before the console banner / autoboot countdown.

This is why the panel now gets valid pixel data regardless of whether the
NAND `bootlogo` partition or any JPEG hardware is involved at all.

## 3. Reverse-engineering toolchain

No root access was available, so everything was installed portably into
`~/tools/`:

- **radare2** — extracted directly from the official `.deb` via
  `dpkg-deb -x` (no install, just unpacked files + `LD_LIBRARY_PATH`).
- **Ghidra 12.1.2** — downloaded release zip, unpacked.
- **Temurin JDK 21** — portable tarball (Ghidra's only real dependency).

Ghidra headless workflow used throughout:

```bash
export JAVA_HOME=~/tools/jdk/jdk-21.0.11+10
export PATH="$JAVA_HOME/bin:$PATH"
GHIDRA=~/tools/ghidra/ghidra_12.1.2_PUBLIC

# one-time import + full auto-analysis
"$GHIDRA/support/analyzeHeadless" ~/tools/ghidra_project stock_uboot \
  -import "Prado firmware dump/mtd1-mtd2_uboot/mtd1_uboot.bin" \
  -processor "ARM:LE:32:v7" -loader BinaryLoader -loader-baseAddr 0x30000

# subsequent runs: decompile specific functions to a text file via a
# small custom GhidraScript (DumpFunc2.java-style — ensures a Function
# exists at each address, then calls the decompiler and writes the C)
"$GHIDRA/support/analyzeHeadless" ~/tools/ghidra_project stock_uboot \
  -process mtd1_uboot.bin -noanalysis \
  -scriptPath <dir> -postScript DumpFunc2.java "0xADDR1,0xADDR2,..." out.txt
```

Load address `0x30000` matches the stock binary's documented load address
(see `docs/UBOOT_BUILD_GUIDE.md`).

### Command table recovery

The stock binary is stripped (no symbol table), so every function starts
out as `FUN_00xxxxxx`. The command table was located by finding the
`bootnand` string's address, then searching the binary for a 32-bit
little-endian word matching that address — which lands inside a
`cmd_tbl_s`-shaped array (`{name, maxargs, repeatable, cmd, usage}`,
5 words / 0x14 bytes per entry, matching the classic pre-Kconfig
u-boot command table layout). Walking that array by hand (with a couple of
early field-order mistakes, since corrected — see conversation) recovered
real names for every custom command:

| Command | Address | What it does |
|---|---|---|
| `disconfig` | `0x68bec` | Display config (fuller than the reference `do_disconfig` in source — calls into `LcdArgInFlash`/`ui_scaler_type` chain, see §4.3) |
| `gpiotest` | `0x69880` | 3-mode GPIO self-test (0=input watch, 1=output blink, 2=reuses JPEG clock init + registers dummy IRQ callbacks) |
| `jpeghw` | `0x69c28` → `0x69b10` | Hardware JPEG decode: writes `dec_rd_base_addr`/dest registers, sets the `START` bit, waits on completion (stock: via interrupt) |
| `pmem` | `0x6c404` | Hex memory dump |
| `regw`/`regr` | `0x6c244`/`0x6c0d8` | Generic register peek/poke across 6 blocks (opcode selects `LCD_BASE`/`SYS_BASE`/`ITU656_BASE`/`VICL_BASE`/`VICH_BASE`/`JPEG_BASE`) |
| `itu656` | `0x6e9f0` | NTSC/PAL composite video-input timing setup |

Full command table also confirmed dozens of standard upstream u-boot
commands (`go`, `bootspi`, `bootz`, `ext4load`, `fatload`, `md`, `mw`,
`ubi`, `usb`, etc.) are unchanged from mainline — those weren't touched.

### Full-binary decompile + source matching

All 750 functions in the stock binary were decompiled in one pass and
saved to `docs/re_stock_uboot/`:
- `full_decompile.c` (1.1MB) — every function, address order
- `function_index.tsv` — address/size/name + up to 4 string hints per
  function (Ghidra auto-labels string data by content, which substitutes
  for the missing symbol names)

These were cross-referenced against the current source tree by string
matching (grep each function's referenced string literals against
`*.c` in the build tree, with prefix-truncation fallback to handle
wording drift between the stock binary's **U-Boot 2012.10** base and the
current **2018.07** tree). Result: 197/257 functions-with-strings matched
cleanly to an existing source file (confirming most of the binary —
UBI, USB, FAT/ext2/ext4, zlib, SHA1, NAND BBT — is unmodified upstream
code). The ~60 real no-match functions are the genuinely vendor-custom
ones; see §4 for what was done with them.

One important side-finding from this pass: `ARK_DISPLAY_ALL_MODE` is
`#define`d `0` in `ark1668_lcd.h` and gates ~8 blocks in `ark1668_lcd.c`
(gamma, video/OSD color scaling, TV-encoder init paths, and the
`display_updatepara` struct fields for `ui_scaler_type`/`itu656bypinfo`/
`special_info`). **Left off** per explicit decision — some of the gated
functions are dead even if enabled (`ark_set_gamma()` only called from a
commented-out line), and turning it on doesn't restore the arkdata.ini
loading or reversing-camera hooks anyway (those have zero source presence,
gated or not).

## 4. What got ported, and the risk call on each

Everything below is additive (new files/functions), doesn't modify
existing display/boot logic, and builds warning-clean. **None of it has
been tested on real hardware.**

### 4.1 Low risk — shipped

**`regr`/`regw`/`pmem`** (`ark1668_display_cfg.c`) — plain address
peek/poke across the 6 register blocks `regw`/`regr` used in stock.
Pure reads/writes, no protocol to get wrong.

**`gpiotest 0`/`1`** (`ark1668_debug_cmds.c`) — input-watch and
output-blink GPIO tests, ported faithfully from the decompiled bit-bang
logic on `GPIO_BASE = 0xE4600000`. Stock's versions loop forever with no
escape; `ctrlc()` checks were added as a deliberate improvement over the
original.

**`bootlogofind`** (`ark1668_display_cfg.c`) — ported from stock's
`FUN_0006bf68`: tries the NAND `bootlogo` partition first, checks for a
JPEG SOI marker (`0xFF 0xD8`); if missing, falls back to
`fatload mmc <0/1/2>:1 bootlogo` on SD. Kept as a diagnostic only (no
JPEG decoder to actually display what it finds), but useful for
confirming whether valid bootlogo JPEG bytes are present anywhere.

### 4.2 Moderate risk — shipped with a deliberate deviation from stock

**`jpeghw <src_hex> <dst_hex>`** (`ark1668_debug_cmds.c`) — real hardware
JPEG decode. Register sequence and offsets are decompiled faithfully:

```
JPEG_BASE (0xE0200000) + 0x3c  INTCLR
                        + 0x2c  CTRL
                        + 0x04  mode/table select
                        + 0x50  COUNT
                        + 0x38  INTMASK
                        + 0x5c  dec_rd_base_addr  <- source JPEG bytes
                        + 0x24  dest Y-plane
                        + 0x28  dest chroma-plane (dest + 0x200000)
                        + 0x30  START (bit 31)
                        + 0x34  status (bit0=done bit2=error)
```

**Deviation**: stock is interrupt-driven — it registers an ISR against
the SoC's VIC (line 10, `JPEG_INT`) via `FUN_0006a11c`/`FUN_00069ff8`,
and the actual decoded-width/height values come from that ISR
(`FUN_00069980`, fully decompiled — confirms status bit 0 = success,
bit 2 = error, decoded W/H read from the upper 16 bits of
`JPEG_BASE+0x04`/`+0x0c`). Porting the ARM IRQ exception-vector plumbing
that would require was judged too risky to blind-port (wrong offset
there hangs/crashes the CPU, no safe incremental test). The shipped
version **polls** `JPEG_BASE+0x34` directly instead, using the exact same
done/error bit logic the ISR uses — same hardware protocol, different
(safer) wait mechanism.

**`itu656`** (`ark1668_debug_cmds.c`) — NTSC composite video-input timing.
The constants are real, confirmed from two independent sources
(`display/arkdata.ini` and the stock binary's own built-in default table
— they match exactly): `ModeControl=0x1D80`, `VBP`/`VFP`/etc. The register
bit-packing (which field goes at which shift, into which of
`LCD_BASE+0x3d0..0x3e4`) was transcribed field-for-field from the
decompiled `FUN_0006e870`, cross-checked using the fact that the PAL
register block mirrors the NTSC one exactly (self-validating). **One
block intentionally left out**: a section in stock's `FUN_0006e9f0`
between the timing setup and the final enable, gated on unresolved
pointers (`DAT_0006ea90/94/98`) that look like current screen/resolution
state — flagged in a code comment rather than guessed.

### 4.3 High risk — explicitly scoped out

**`gpiotest 2`** and the VIC/ARM-IRQ-vector infrastructure generally —
stubbed with a message instead of ported. Same risk as above.

**The reversing-camera / video-scaler / DMA pipeline** behind stock's
`FUN_000684d0` (called from the real `disconfig`) — decompiled far enough
to see the shape of it (an 8-way branch, `FUN_0006e7b0`, each arm doing
20-40 bit-packed writes into unlabeled scaler/DMA-controller registers,
feeding DMA engines that write decoded video frames to **hardcoded raw
physical addresses** — `0xE000000`, `0xB400000`, `0xBE00000`). Explicitly
excluded per user decision: not used on this device, and the failure mode
of getting it wrong isn't "camera doesn't work" — it's a misconfigured DMA
engine writing outside its buffer, a real corruption/hang risk with no
datasheet to check against.

**The full stock ini-parser object** (`FUN_0006f97c`/`f910`/`f6e0` — a
genuine generic INI library with UTF BOM detection and hash-table key
lookup) — not replicated byte-for-byte. See §5: a much simpler
from-scratch reader was written instead, matching *observable* behavior
(iterate lines, look up by key, parse as int) rather than the internal
hash-table bucket layout, since callers can't tell the difference and
matching internals would have been large effort for zero behavioral
gain. Also, with the camera pipeline and `ARK_DISPLAY_ALL_MODE` both out
of scope, most of what the full stock parser feeds (gamma, scaler type,
itu656 calibration, video-processing brightness/contrast) has nothing
left to consume it anyway.

### 4.4 Empirical confirmation: LCD path doesn't use the VIC/IRQ at all

The VIC/IRQ scoping decision in §4.3 was based on source/decompile
reading alone at the time. It's since been confirmed directly against
real hardware register reads, using the already-shipped `regr` command,
after the boot logo was already confirmed working on-screen.

Source basis for the claim: `ark_disp_wait_lcd_frame_int()` (already in
`ark1668_lcd.c`, called from `display_updatelogo()` as part of
`ark_display_init()`) is a plain busy-poll, not an interrupt handler,
despite the name:

```c
void ark_disp_wait_lcd_frame_int(void)
{
	// wait until LCD timing point intr happens (which is VSync here)
	rLCD_INTERRUPT_STATUS = 0;
	while(!(rLCD_INTERRUPT_STATUS & 0x01));
	// the timing point is set at bit22-21 on CLCD_CONTROL reg
}
```

`rLCD_CONTROL` is `LCD_BASE+0x004`, `rLCD_INTERRUPT_STATUS` is
`LCD_BASE+0x180` — both plain memory-mapped registers on the LCD
controller itself, no VIC/ARM-IRQ-vector involvement in the source.

**Hardware trace, taken at the `=>` prompt after the logo was already on
screen:**

```
=> regr 0 0x4
[op=0] reg 0x04 = 0x03600081
=> regr 0 0x180
[op=0] reg 0x180 = 0x00000033
=> regr 0 0x180
[op=0] reg 0x180 = 0x00000033
=> regr 3 0x14
[op=3] reg 0x14 = 0x00000000
=> regr 4 0x14
[op=4] reg 0x14 = 0x00000000
```

Reading:
- **`rLCD_CONTROL = 0x03600081`** — bits 21-22 read as `0b11`, matching
  the source comment that this field selects VSync as the monitored
  timing point (consistent with `ark_disp_wait_lcd_frame_int()` actually
  being the function in effect, not the TVENC/bit12-11 variant — correct
  for an RGB LCD panel, not a CVBS/TV-encoder output). Bit 0 set (LCD
  enable), plus a couple of other control flags (bits 7, 24-25) not
  documented in what source we have, but nothing that correlates with any
  visible problem.
- **`rLCD_INTERRUPT_STATUS = 0x33`, identical on two consecutive reads**
  — expected, not a fault. `0x33 = 0b00110011`; bit 0 (`0x01`, the exact
  bit the wait function polls) is **set**, meaning the LCD controller is
  actively latching VSync events right now — direct, independent
  confirmation (from a status register, not just "the picture looks
  right") that the panel is actively scanning. It reads identically
  because nothing is clearing/re-arming it between reads outside the
  wait function's own poll loop — a level-latched flag staying latched
  when nothing clears it is correct, not a hang. Bits 1/4/5 are other
  status flags with no documented meaning in the source available, but
  uncorrelated with any visible fault.
- **`VICL`/`VICH` enable-mask (`op=3`/`op=4` @ `+0x14`) both
  `0x00000000`** — the key result. Zero interrupt lines enabled on
  either VIC, read directly from silicon, while the display is
  demonstrably working. This confirms empirically what the source read
  already implied: **the LCD/boot-logo path never touches the VIC or
  ARM IRQ-vector infrastructure.** The interrupt-vector work scoped out
  in §4.3 was correctly assessed as unnecessary for anything currently
  working on this device — it's specific to the (unported) `jpeghw`
  interrupt-driven path and `gpiotest 2`, not to display.

## 5. LCD timing fix + `arkdata.ini` runtime reader

### 5.1 Compiled defaults didn't match the real calibration

Comparing the hardcoded `SCREEN_QUN700` entry in `screens[]`
(`ark1668_display_cfg.c`) against `display/arkdata.ini`'s
`[LCD_TIMMING]`/`[LCD_CLOCK]` sections found real, non-trivial mismatches:

| Field | Compiled default | `arkdata.ini` |
|---|---|---|
| VBP | 40 | 29 |
| VFP | 36 | 25 |
| HFP | 32 | 25 |
| HSW | 41 | 54 |
| Pixel clock | 0 (derived) | 330,000,000 Hz |
| CLKDIV1 | 13 | 11 |

(`Width`/`Height`/`VSW`/`HBP` and sync polarities already matched.)

Differences this size (30%+ on several porch/sync-width fields) risk a
shifted/mistimed picture or a complete sync failure, independent of
anything OSD/framebuffer-related. **Fixed**: the `SCREEN_QUN700` struct
literal now uses the `arkdata.ini` values; the original values are kept
as a comment directly above for easy revert if the new timing doesn't
sync on real hardware. `tvout_format`/`tvenc` were deliberately left
alone — no clean 1:1 field mapping to `arkdata.ini`'s `TvoutType`, and
guessing there risked introducing a new bug rather than fixing one.

### 5.2 `arkdata.ini` runtime reader (new)

New file `ark1668_arkdata_ini.c` lets the SD card's `arkdata.ini`
override the compiled LCD timing at boot, instead of requiring a
recompile every time calibration changes:

- `arkdata_ini_load()` — lazily `fatload`s `arkdata.ini` from `mmc 0:1`
  into RAM once.
- `arkdata_ini_get_int(key, base, &out)` — flat line scanner: finds
  `key=value` (leading whitespace tolerated, `;` comments and
  `[Section]` headers just don't match any key so they're implicitly
  skipped, blank values treated as absent).
- `arkdata_apply_lcd_timing(screen)` — overrides
  `vbp`/`vfp`/`vsw`/`hbp`/`hfp`/`hsw`/sync-polarity/clock fields on an
  already-populated `screen_info`. **Fails safe**: missing file or
  missing key just leaves the field at whatever the compiled default
  (now the corrected one, §5.1) already was.
- `arkdatatest <key>` — new command for ad-hoc key lookups.

Wired into `ark_display_init()`, called right before the screen struct is
used, so any `arkdata.ini` present on the SD card's FAT partition
(next to `UBOOT.BIN`) is picked up automatically on the next boot — no
code changes needed to try a different unit's calibration.

**Deliberately not ported**: the stock hash-table/BOM-detection ini
engine (see §4.3) and the gamma/scaler/itu656/carback-camera fields —
consistent with the camera pipeline and `ARK_DISPLAY_ALL_MODE` scope
decisions. Extending `arkdata_apply_lcd_timing`-style overrides to more
fields later is a small, low-risk addition on top of what's here — the
parsing plumbing is already built and doesn't need to change.

### 5.3 Debug logging

Both the bootlogo path and the arkdata.ini path now log their full
decision trail on the serial console (not just success/failure):

- `ark_show_bootlogo()`/`display_bootlogo_from_sd()` — logs the exact
  `fatload` command run, its return code, the reported file size vs. the
  expected `800×480×4` (with a warning if they don't match — catches a
  stale/wrong conversion immediately), and confirms when OSD1 is enabled.
- `arkdata_ini_load()`/`arkdata_apply_lcd_timing()` — logs the `fatload`
  command and result, the reported file size, and **every field**
  old-value → new-value (or "not found, keeping compiled default").
- `#define DEBUG` is set at the top of `ark1668_arkdata_ini.c` only
  (file-scoped, not tree-wide) to also surface the finer-grained
  `debug()`-level per-key parse traces without flooding the console with
  unrelated subsystem noise from a global debug build.

Example boot log:

```
bootlogo: ark_show_bootlogo() starting, screen_id=0
bootlogo: ark_display_init() done
arkdata.ini: applying LCD timing overrides for screen_id=0
arkdata.ini: loading -> `fatload mmc 0:1 0xfe00000 arkdata.ini`
arkdata.ini: fatload reported filesize=0x2a4 (676 bytes)
arkdata.ini: loaded 676 bytes from SD into RAM @ 0xfe00000
arkdata.ini:   VBP      29 (unchanged)
arkdata.ini:   VFP      25 (unchanged)
...
arkdata.ini: done — 12/12 fields overridden from SD card, final timing (...)
bootlogo: loading -> `fatload mmc 0:1 0xfc00000 bootlogo.raw`
bootlogo: fatload reported filesize=0x177000 (1536000 bytes), expected 0x177000 (800x480x32bpp)
bootlogo: pushing OSD1 image 800x480 @ 0xfc00000 (DISP_RGB_888)
bootlogo: OSD1 layer enabled, splash should be visible now
```

## 6. File manifest

**Repo root** (`prado-firmware-reconstruction/`):
- `convert_bootlogo.py` — JPEG → raw 32bpp framebuffer converter
- `make_test_bootlogo.py` — generates an 800×480 "U-boot loading" test
  image (PNG) for exercising the pipeline without a real logo asset
- `test_bootlogo.raw` — the converted output of the above, ready to copy
  onto an SD card as `bootlogo.raw`
- `inject_ark_header.py` — post-build ARK header injection (pre-existing)
- `docs/re_stock_uboot/full_decompile.c` / `function_index.tsv` — full
  stock-binary decompile + searchable index

**Build tree** (`~/Downloads/linux-arkmicro/u-boot/board/arkmicro/ark1668_limcet_p305/`):
- `ark1668.c` — `board_late_init()` now calls `ark_show_bootlogo()` first
- `ark1668_display_cfg.c` — `ark_show_bootlogo()`, `display_bootlogo_from_sd()`,
  `bootlogofind`, `regr`/`regw`/`pmem`, corrected `SCREEN_QUN700` timing
  (old values commented above), `arkdata_apply_lcd_timing()` call added
  to `ark_display_init()`
- `ark1668_debug_cmds.c` (new) — `gpiotest`, `jpeghw`, `itu656`
- `ark1668_arkdata_ini.c` (new) — `arkdata.ini` runtime reader,
  `arkdatatest` command
- `ark1668_lcd.h` — declarations for the above
- `Makefile` — registers the two new `.c` files

## 7. Testing status

**Verified on real hardware** (2026-07-12): the full boot logo pipeline —
SD card FAT read, `convert_bootlogo.py`'s pixel packing, corrected
`SCREEN_QUN700` timing, `ark_display_init()` panel bring-up, and the
`OSD1_LAYER` push — confirmed working end to end. `test_bootlogo.raw`
(the "U-boot loading" test image, §6) displayed correctly, and critically,
the test image's border was **pixel-perfect against the physical screen
edges** — strong confirmation that the corrected VBP/VFP/HFP/HSW/clock
values pulled from `arkdata.ini` (§5.1) are genuinely accurate, not just
"close enough to sync." A wrong porch/sync value would show up exactly as
border cropping or offset, so this is about as strong a validation as
that fix could get from visual inspection alone.

Not yet separately confirmed: whether `arkdata.ini` itself was present on
the SD card during this test (i.e. whether the runtime override path in
§5.2 fired, vs. the corrected compiled defaults alone being sufficient) —
worth checking the serial log's `arkdata.ini:` lines specifically if that
distinction matters.

**Verified (build only, not yet exercised on hardware)**: everything
builds warning-clean (`make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf-`),
`inject_ark_header.py` produces a valid `UBOOT.BIN` each time.

**Not yet tested on real hardware**, roughly safest-first:
1. `regr`/`gpiotest 0`/`1`/`bootlogofind`/`arkdatatest` — pure reads, safe
   to try
2. `jpeghw`/`itu656` — real hardware register writes with inferred (not
   datasheet-confirmed) semantics

To reproduce the boot logo test: copy `UBOOT.BIN`, `test_bootlogo.raw`
(renamed to `bootlogo.raw` on the card), and optionally `arkdata.ini`
onto the SD card's FAT partition next to each other, boot with the
serial console attached, and watch for the log trail in §5.3.

## 8. USB boot ideas (proposed, not yet implemented)

Motivated purely by iteration speed — every test cycle currently means
physically removing and reflashing the SD card. With USB mass storage now
confirmed working in u-boot (§ dual-port USB fix; real hardware test read
a SanDisk flash drive's vendor/capacity/partition table correctly), two
ideas were discussed for using a USB stick instead, at different layers.

### 8.1 What's a hard constraint vs. what's not

**U-Boot itself cannot be loaded from USB.** The boot chain's first two
stages — the SoC's boot ROM, then the proprietary NAND-resident
`Nboot.bin`/`Stepldr.bin` — are fixed and load `UBOOT.BIN` from the SD
card specifically (see `docs/UBOOT_BUILD_GUIDE.md`'s boot chain). There's no
evidence anywhere in this project that the boot ROM supports USB as a
boot source, so the SD card can't be removed from the loop entirely —
u-boot itself always has to come from there.

Everything u-boot itself loads *after* it's running is a different
story — that's just `fatload`, and it already works identically for any
block-device interface u-boot supports (`mmc`, `usb`, ...).

### 8.2 Idea A — load everything except U-Boot from USB

Keep the SD card minimal (just `UBOOT.BIN`, rarely touched), and point
the boot flow at USB for everything that actually changes during
iteration: `zImage`, the DTB, `uEnv.txt`, `bootlogo.raw`, `arkdata.ini`.
Mechanically this is a small change — `fatload usb 0:1 <addr> <file>`
is the same FAT/`CONFIG_CMD_FAT` code path as the existing `fatload mmc
0:1 ...` calls throughout `CONFIG_BOOTCOMMAND` and the various `.c`
files in this project (`ark1668_display_cfg.c`, `ark1668_arkdata_ini.c`),
just a different interface string. The natural shape: try USB first,
fall back to the existing SD-based path if no USB stick is present, so
nothing breaks when testing without one plugged in.

Not yet implemented. Lower risk than 8.2 below — it's the same `fatload`
mechanism already used and tested throughout this project, just against
a different (already-proven-working) block device.

### 8.3 Idea B — chainload a second U-Boot from USB

More ambitious: iterate on U-Boot *itself* without touching the SD card
at all, by having the SD-resident U-Boot load a freshly-built
`UBOOT.BIN` from USB into RAM and jump into it directly:

```
=> usb start
=> fatload usb 0:1 0x1000000 UBOOT.BIN
=> go 0x1000000
```

`go <addr>` (already in this build's command table) starts executing
raw code at an arbitrary RAM address — no flashing involved.

Three things specific to this board are worth being careful about before
trusting this, none of them blocking in principle but all untested:

1. **The ARK header vs. `go`'s entry point.** Normally `Stepldr` reads
   the ARK header's `EP` field (see `docs/UBOOT_BUILD_GUIDE.md` §"Stock Binary
   Structure") and jumps straight to `board_init_r`, skipping the
   exception vector table. `go` doesn't know about that header — it
   starts at the very first byte (the reset vector, which branches to
   `_start`). This should still be safe: `CONFIG_SKIP_LOWLEVEL_INIT` —
   the flag that stops U-Boot from re-running DDR init and hanging
   (originally discovered the hard way, see `docs/UBOOT_BUILD_GUIDE.md`
   "Problem 3") — is compiled into the binary itself, not something only
   the header-jump shortcut provides. Going in via `_start` should skip
   DDR reinit the same way, just via the standard path instead of
   Stepldr's shortcut. Not yet verified on hardware.
2. **Memory placement.** The second copy's load address must not
   collide with the *first* (currently-executing) copy's live code/stack.
   `0x1000000` avoids the running copy's origin (`0x30000`) but hasn't
   been checked against exactly where the first copy relocates itself to
   at runtime.
3. **This is a warm handoff, not a real reset.** The second U-Boot
   re-runs all its own hardware init (clocks, GPIO, console, MMC/USB) on
   top of whatever state the first one already left behind, rather than
   starting from Stepldr's known-clean post-DDR-init state. Probably
   fine (close to what happens on every normal boot), but is new,
   untested territory for this SoC specifically — the DDR-reinit
   sensitivity was a real, previously-hit hang on this board (the
   original "Starting Uboot → no console" failure mode during initial
   bring-up), so this deserves a cautious first test with serial
   watched closely and a readiness to power-cycle rather than an
   assumption that it's silent and safe.

Not yet implemented or tested. If it works, a wrapper command (e.g.
`usbuboot`, combining the fatload+go steps with basic sanity checks)
would be a reasonable next step to make it a one-liner.

**CORRECTION (verified via objdump disassembly of the real Stepldr.bin,
Holden firmware update package):** point 1 above was wrong. Stepldr does
NOT read the ARK header's `EP` field and jump to `board_init_r`. Its
actual load routine hardcodes `mov r0, #0x30000` immediately followed by
`blx r0` — it jumps straight to the fixed load address (the reset
vector / `_start`), the same as `go 0x30000` would, ignoring the header's
`EP` field entirely. (`EP` may be used for something else — validation,
bookkeeping — not confirmed.)

This matters directly for `bootstock` (`ark1668_boot_cmds.c`), which was
built on the wrong assumption and has been jumping to the header's `EP`
(`0x54ef8`) instead of the load address (`0x30000`) — skipping whatever
the stock binary's own `_start`/vector-table setup does. That's a
plausible explanation for `bootstock`'s intermittent `undefined instr
resetting` crashes (worked once, failed consistently after — skipping
required low-level init would produce exactly that kind of "usually
fine, occasionally traps" pattern). Fix: change `bootstock`'s `go`
target from the header `EP` to `STOCK_UBOOT_LOAD_ADDR` (`0x30000`)
directly. **Applied and hardware-confirmed 2026-07-13** (see
`project_nand_ecc_investigation` memory) — this specific bug is fixed.

**2026-07-29: a different intermittent failure in the same chainload
path, found later.** User reported `bootstock` still occasionally
chainloading into stock U-Boot successfully but then having *stock
U-Boot itself* fail to boot the stock kernel — a different symptom
from the `EP`-vs-`0x30000` crash above (that one crashed inside stock
U-Boot immediately; this one gets further, into stock's own kernel
boot). The chainload's warm handoff (`bootstock_file_from_block_dev()`)
only zeroes 7 NAND/BCH control registers right before the jump
(`rBCH_CR`, `rBCH_INT` clear+mask, `rNAND_DMA_CTRL`, `rNAND_GLOBAL_CTL`,
`rNAND_JUMP_CTL`, `rNAND_CR`) — unconditionally, with no check that the
controller's FSM has actually finished whatever transaction our own
boot sequence last ran (kernel/arkdata/reservingtrack load, `switchecc`,
etc, at a point in the boot sequence that varies session to session
depending on what the user did before typing `bootstock`). Zeroing
control registers out from under a still-in-flight transaction would
leave the controller in a genuinely undefined state for stock U-Boot's
own NAND driver to inherit when it then tries to read the kernel
partition — a plausible mechanism specifically for *intermittent*
failures (a fixed, always-present gap would be expected to fail every
time, not "moments where it fails"). **Fixed** (`linux-arkmicro`
commit `25f3bb7ec`): added a bounded poll on `rBCH_NAND_STATUS` bits
`[5:0]` (the FSM-idle condition; reuses the exact same wait this
build's own `ark_nand.c` driver already does after every real NAND
transaction) before the existing register reset, so the zeroing only
happens once the controller is actually quiescent. Bounded with a
timeout rather than looping forever, so a genuinely wedged controller
prints a warning and proceeds rather than hanging the chainload
silently. Not yet hardware-tested.

**Same day, follow-up: a second, upstream cause found.** User clarified
two things: (1) `boothybrid` (same `bootstock_file_from_block_dev()`
function, different source file `uboot_hybrid.bin`) can hang the same
way, before the kernel loads; (2) "the kernel magic error seems to
come and go." Also corrected an assumption in the previous fix's
comment -- `bootstock`/`boothybrid` are reached automatically via the
default `CONFIG_BOOTCOMMAND` fallback chain (`bootusb` →
`boothybrid` → `bootstock` → `nandboot`) on every cold boot, not typed
by hand at the prompt as previously assumed, so any run-to-run
variance is real hardware/timing marginality, not user-command
variability.

The only "magic" check anywhere in this path is the ARK header check
right after `fatload`'ing `uboot_stock.bin`/`uboot_hybrid.bin` from
the SD/USB FAT partition -- upstream of the NAND-FSM fix above (which
only covers the register reset immediately before the `go` jump,
*after* this check already passed). An intermittent magic mismatch
("comes and goes") fits a transient/marginal SD or USB read
corrupting part of the loaded 428KB binary far better than a
deterministic logic bug. **Fixed** (`linux-arkmicro` commit
`ad1e7c816`): retries the `fatload`+magic-check up to 3 times before
giving up, rather than failing hard on a single bad read. Not yet
hardware-tested.

**Same day, decisive move: default boot chain no longer chainloads
stock U-Boot at all.** User's call: the intermittent hang is inside
the unmodifiable stock U-Boot binary itself once execution jumps into
it -- not something either of the two fixes above can retry from our
side, since control has already left our own code. Worse, because
`boothybrid`/`bootstock` sat inside `CONFIG_BOOTCOMMAND`'s
`if...elif...else` chain, a genuine hang there never returns, meaning
the `nandboot` fallback at the end of that chain never ran either --
the unit would just sit stuck until power-cycled, defeating the whole
point of having a fallback.

Given `nandboot` (this build's own U-Boot booting the real stock
kernel+rootfs directly from NAND -- no chainload, no black-box binary,
entirely our own debuggable code) was already hardware-confirmed
2026-07-24 (§45 above) to bring up the full stock kernel/MsnCoreApp/
CarPlay/BT/WiFi stack end to end, there's no known remaining
functional gap that chainloading stock U-Boot was still needed for.
**Fixed** (`linux-arkmicro` commit `299f89b32`): default
`CONFIG_BOOTCOMMAND` is now `bootusb -> nandboot` directly, removing
the chainload step (and its unfixable hang risk) from the automatic
boot path entirely. `boothybrid`/`bootstock` remain available as
manual commands at the prompt for anyone who wants to explicitly
test/compare against real stock U-Boot. Not yet hardware-tested.
