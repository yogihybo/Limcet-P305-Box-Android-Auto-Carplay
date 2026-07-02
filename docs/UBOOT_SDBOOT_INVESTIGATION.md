# U-Boot SD-Boot Patch — Corruption Investigation

**Status: working auto-boot patch found (§8), statically verified, not yet
tested on hardware.** A new technique — a minimal compiled-in `bootcmd` that
loads and `source`s a boot script from SD — fits the raw/Holden-derived
`uboot.bin`'s tiny safe capacity and needs zero NAND writes. Generated at
`experimental_sdboot/uboot_selfcontained.bin`; do not treat as verified until
tested on real hardware. `uboot_sdboot.bin` and `uboot_final.bin` (the
earlier, corrupted attempt) remain quarantined under `corrupted/`. The manual
U-Boot-prompt command (README §4.0 "Manual SD Card Boot", §5.0 "USB boot")
remains the confirmed-working fallback. This document records how the
corruption was found, why the obvious workarounds (§2, §6) don't fix the
underlying env-space problem, and what it took to get a real (if untested)
patched auto-boot U-Boot (§5, §7, §8). Written so this doesn't need to be
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

---

## 5. Paths forward

1. **Real BSP-compiled source** — `docs/SD_BOOT_PLAN.md` (superseded, but
   still useful here) references `~/Downloads/linux-arkmicro` as containing
   "the exact U-Boot source for this SoC," with the two config changes needed
   (`CONFIG_ENV_OFFSET=0x120000`, `CONFIG_BOOTCOMMAND`/`CONFIG_EXTRA_ENV_SETTINGS`
   for sdboot) documented in full there (§"Option 2 — Build U-Boot from
   source"). That path was explicitly called "preferred" over binary patching
   at the time, but was never taken — and the BSP source tree isn't in this
   repo (was on the original author's machine only, under their `~/Downloads`).
   **This is the correct path if the BSP source can be located again.**
2. **ARM relocation patch** (§4) — possible in principle, but needs real
   disassembly tooling and carries meaningful brick risk. Not recommended
   without that tooling in hand.

## 6. Initramfs — considered, doesn't help

Idea considered: use an initramfs to work around the env-space limit.

`docs/SD_BOOT_PLAN.md` Phase 4 documents an initramfs, but for a *different*
problem: if `ark_dw_mmc` (the SD/MMC driver) is a loadable module rather than
built into the kernel, the kernel can't mount an SD rootfs, because the
driver needed to read the SD card is itself sitting on the SD card. The fix
there is a small initramfs that `insmod`s the module first, then pivots to
the real root.

**Whether that problem even applies here is unresolved.** `docs/KERNEL.md`
states "no loadable modules — the entire driver set is compiled
monolithically," explicitly listing `drivers/mmc/` as built-in. But a real,
29 KB `ark_dw_mmc.ko` module file does exist in the shipped rootfs
(`.../rootfs/lib/modules/3.4.0/kernel/drivers/ark/sdmmc/ark_dw_mmc.ko`) —
exactly what `SD_BOOT_PLAN.md` was concerned about. Tried to settle it by
searching the kernel image for MMC-related symbol strings, but `zImage` is a
compressed image — a raw string search can't see inside it, so this remains
unconfirmed either way without real decompression/extraction tooling.

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
