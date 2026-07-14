# Handoff: NAND ECC (U-Boot vs kernel) and tonight's U-Boot patches

**Audience:** the agent/model picking this up next. Self-contained -- you
don't need the prior conversation. Covers the NAND ECC investigation (what's
fixed in U-Boot, what's still broken in the kernel, what's architecturally
unfixable) and every patch applied to the `ark1668_limcet_p305` U-Boot board
port in this session.

**Build tree:** `/home/osboxes/Downloads/linux-arkmicro/u-boot`
(separate git repo, root at `/home/osboxes/Downloads/linux-arkmicro`).
**Main repo:** `/media/sf_GitHub/prado-firmware-reconstruction`.
Committed: `1422e3411` (u-boot repo, whole board port + NAND fix) and
`5f4dcfc` (main repo, `build_bootable_sdcard.sh`).

**Headline result (2026-07-13, confirmed on real hardware):** `bootstock`
works end-to-end -- this build (booted from SD) chainloads the stock
U-Boot binary, which then successfully boots the full stock kernel +
rootfs + UI from NAND. This is now a completely reliable path to the
original stock system from a custom-U-Boot-on-SD setup, and it sidesteps
the `bootnand` kernel-entry hang (section 5) entirely -- that issue no
longer blocks getting a working NAND boot, it only blocks doing it via
*this* fork's own `bootz` directly.

---

## 1. NAND ECC -- U-Boot side: FIXED and confirmed on real hardware

### Symptom
`nand read <addr> kernel` (and any other partition read) failed on every
page:
```
!!Read Data err more than 8 bit and Group = 0 status:0x4
NAND read from offset 1a0000 failed -74
 0 bytes read: ERROR
```

### Root cause
Two independent things were wrong, both found by comparing against the
**real stock U-Boot 2012.10 binary**, live, on real hardware:

1. **Wrong OOB layout.** This chip's actual on-flash format for
   kernel/rootfs/bootloader-type partitions is a 1024-byte ECC step (2
   segments per 2048-byte page), 13-byte/7-bit BCH strength, with the ECC
   bytes starting at OOB offset 3 (not the driver's two predefined layouts:
   `nand_hw_eccoob_bootstrap` is 4x13B at offset 3-54/512B-step;
   `nand_hw_eccoob_64` is 2x23B at offset 18/1024B-step). Confirmed via
   `nandoobcheck`/`nand dump.oob` raw dumps of both the `kernel` and
   `U-boot` NAND partitions -- both show real data only at OOB offset 3-28
   (26 bytes = 2x13), everything after left erased (0xFF).

   Fix: revived a dead, `#if 0`'d-out layout that was already in the
   vendor source (`drivers/mtd/nand/ark_nand.c`, originally named
   `nand_hw_eccoob_64` before being superseded by the wrong 23-byte/offset-18
   version) -- renamed `nand_hw_eccoob_64_2seg13b`, wired in as `switchecc 2`.

2. **Wrong `BCH_CR` register value.** The bootstrap/normal modes only ever
   set `BCH_CR_BCH_ENABLE` (bit 0). The chip actually needs
   `BCH_CR_SOFT_ECC_ENABLE` (bit 1) as well/instead -- confirmed two ways:
   - Reading `BCH_CR` (`0xec00027c`) live on the working stock U-Boot
     prompt, immediately after a real successful `nand read ... kernel`:
     `0x00000182` = SECTOR_MODE(bit8) | SECTOR_LENGTH(bit7) |
     SOFT_ECC_ENABLE(bit1).
   - The Linux kernel's own NAND driver source
     (`Limcet Hardware/ark_nand_kernel.c`) names these bits explicitly --
     `BCH_CR_SOFT_ECC_ENABLE = (1<<1)`, `BCH_CR_BCH_ENABLE = (1<<0)` -- and
     its read/write trigger sequences always set them together
     (SOFT_ECC_ENABLE|BCH_ENABLE), right after a decoder/encoder reset
     pulse. Stepldr's own disassembled load routine (see section 3) does
     the exact same reset-pulse-then-OR-0x3 pattern.

   Fix: `switchecc 2`'s register write now uses `(1<<8)|(1<<7)|(1<<1)`
   instead of the original `(1<<8)|(1<<7)|(1<<0)`.

**Loose end, not fully explained:** `ark_nand_enable_hwecc()` (the driver's
per-read trigger, called from `ark_nand_read_page_syndrome()`) already ORs
in `(BCH_CR_SOFT_ECC_ENABLE|BCH_CR_BCH_ENABLE)` together on every single
read, regardless of what `switchecc` set as the persistent baseline
beforehand. On paper this means the bit-0-vs-bit-1 distinction in the
persistent baseline shouldn't matter for reads. Empirically it does --
`0x181` baseline was unreliable (intermittent errors), `0x182` baseline has
been clean and repeatable across many live tests, matching stock exactly.
Best working theory: getting the baseline's other bits right
(SECTOR_LENGTH/step-size, keeping bits [6:4] at 0 for 13-byte/7-bit
strength) is what actually matters, and `hwctl()`'s OR-in only works
correctly on top of a non-polluted baseline. Not fully traced to ground
truth -- if you have time, this is the next thing to nail down precisely.
The fix works regardless of the exact mechanism.

### Files changed
- `drivers/mtd/nand/ark_nand.c` -- revived layout (renamed
  `nand_hw_eccoob_64_2seg13b`), new `switchecc 2` mode in `do_switchecc()`.
- `include/configs/ark1668_limcet_p305.h` -- `nandboot` env command now runs
  `switchecc 2` before `nand read ... kernel` (see section 4 for the
  `machid` fix bundled in the same command).

### How to verify it's still working
```
switchecc 2
nand read 0x1000000 kernel
crc32 0x1000000 0x400000
```
Should read `4194304 bytes read: OK` with zero `!!Read Data err` lines. Run
it 2-3 times back to back -- CRC32 should be identical every time.

---

## 2. NAND ECC -- kernel side: now patched to match U-Boot's ground truth, NOT yet hardware-tested

The Linux kernel's own NAND driver (`Limcet Hardware/ark_nand_kernel.c`,
mirrored 1:1 from the live build tree at
`/home/osboxes/Downloads/linux-arkmicro/linux/drivers/mtd/nand/raw/ark_nand.c`)
has known, real ECC read problems (established from prior real-hardware
testing -- see `docs/HANDOFF_touch_and_bootargs_fix.md` "Fix C", the ~417
false-bad-block issue and the "too weak ECC" warning), even though its
`BCH_CR` trigger sequence looks textbook-correct in the source (it already
sets SOFT_ECC_ENABLE|BCH_ENABLE together, matching what U-Boot needed).

**Update:** diagnosed and patched, same session as the U-Boot fix (section 1).
Found in `ark_nand_hw_syndrome_ecc_ctrl_init()` (was `ark_nand.o`, builds
clean with `make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf-
drivers/mtd/nand/raw/ark_nand.o` -- not yet flashed/booted on real hardware):

- The kernel build tree's `ark_nand.c` was already mid-edit, uncommitted,
  from a prior (pre-U-Boot-investigation) debugging session -- `git diff`
  in that repo showed the committed vendor original used `ecc->size = 1024`
  (2 segments/page, right) but `BIT_13_ECC_BYTE` (23-byte ECC, 13-bit BCH
  strength -- wrong), while the uncommitted working copy had been changed to
  `ecc->size = 512` (4 segments/page, wrong) with `BIT_7_ECC_BYTE` (13-byte
  ECC, 7-bit strength -- right) but had dropped `BCH_CR_SECTOR_LENGTH`.
  Neither combination matches the real on-flash format established in
  section 1: 1024-byte step / 13-byte / 7-bit BCH / `SECTOR_LENGTH` bit set.
  Fixed by combining the correct half of each: `ecc->size = 1024`,
  `ecc->bytes = BIT_7_ECC_BYTE`, `ecc->strength = 7`, and
  `val = BCH_CR_SECTOR_MODE | BCH_CR_SECTOR_LENGTH | BCH_BIT_SEL(BCH_7BIT_SEL)
  | BCH_CR_BCH_ENABLE` (the `mtd->oobsize == 64` branch only -- the
  `oobsize >= 128` branch is unused/unverified for this chip and was left
  alone apart from making its previously-implicit `ecc->size = 512` default
  explicit).
- Separately, `mtd_set_ooblayout(mtd, &nand_ooblayout_lp_ops)` was wrong
  regardless of the above -- that's the generic large-page layout, which
  places ECC bytes at the *end* of the OOB region. The real layout (per
  section 1's `nandoobcheck` dumps) has ECC at OOB offset 3, 26 bytes total
  (2 segments x 13 bytes), free area at offset 32/32 bytes. Added a custom
  `mtd_ooblayout_ops` (`ark_nand_ooblayout_2seg13b_ops`,
  `ark_nand_ooblayout_ecc`/`ark_nand_ooblayout_free`) mirroring U-Boot's
  revived `nand_hw_eccoob_64_2seg13b` layout, wired in for the `oobsize ==
  64` case only.

This means the kernel's problem was very likely the same class of bug as
the U-Boot one -- an OOB layout/ECC-step-size mismatch, not the register
trigger bits, exactly as suspected. Two independent driver implementations,
two independent (now both fixed-in-source) instances of the same underlying
format mismatch.

**Not yet verified on hardware.** The fix builds clean (`.o` compiles with no
warnings) but has not been flash-tested. Verify the same way section 1 was
verified: build the kernel/module, boot it, and confirm NAND partition reads
(`kernel`, `rootfs`) succeed without `!!Read Data err`/false-bad-block
messages, ideally cross-checked against `nandoobcheck` dumps of the same
pages read via U-Boot. If it still fails, re-run the same raw-OOB-dump
comparison method described below on a live kernel (e.g. via a debug read
path or `mtd_debug`/`nanddump` from userspace) since the kernel's read path
(`ark_nand_read_page_syndrome`) walks the OOB in a slightly different order
than U-Boot's and could reveal a further offset/rounding difference not
visible from static analysis alone.

This is why this project's own `build_bootable_sdcard.sh` routes NAND
partition data (bootlogo, bootanimation, reversingtrack, Unicode font)
through SD-card symlinks (`redirect_mtd_data`) instead of trusting the
kernel to read them from NAND directly -- keep that fallback in place until
the fix above is confirmed working on real hardware.

---

## 3. The U-boot NAND partition: architecturally unreadable via any U-Boot-level tool

This is not a bug -- do not try to fix it further. Proven exhaustively:

- Our driver, all three ECC modes (`switchecc 0`, `1`, `2`) -- all fail with
  `err more than 8 bit`.
- Stock U-Boot's own native `switchecc 1`, on the real reference
  binary -- same failure, same error text.

Since even the reference stock binary's own tooling can't read it, the only
thing that has ever successfully read this partition is Stepldr (the
tiny, proprietary, pre-U-Boot loader), and it doesn't use the standard
`nand`-command machinery at all.

Confirmed via `objdump` disassembly of the real `Stepldr.bin` (from
`Holden firmware update/Stepldr.bin`; scratchpad disassembly saved at
`/tmp/.../scratchpad/stepldr_disasm.txt` if it still exists, otherwise
regenerate with `arm-linux-gnueabihf-objdump -D -b binary -marm
--adjust-vma=0 Stepldr.bin`): Stepldr's actual page-read primitive
(function at file offset `0x1af4`) reads via `rNAND_DATA` (`NAND_BASE+0x14`)
in a raw polling loop against the status register at `NAND_BASE+0x280` --
it never touches `BCH_CR` at all. No ECC configuration, no decode step.
It's a raw, uncorrected read, presumably relying on the bootloader region
being verified error-free at production/flash time rather than needing
runtime correction.

Practical consequence: `bootstock` (see section 4) sources the stock binary
from an SD file, never from NAND. Don't revisit NAND-sourcing for it.

---

## 4. Everything else patched tonight (all in the U-Boot board port)

### `ark1668_boot_cmds.c` (new file)
Four boot commands:
- **`bootnand`** -- wraps `run nandboot`. NAND ECC now fixed (section 1) and
  `machid` now set correctly (kernel accepted machine ID `0x1068` -- the
  "unrecognized/unsupported machine ID" error is gone), but kernel entry
  still hangs -- see section 5, not fixed tonight.
- **`bootmmc`** / **`bootusb`** -- kernel+DTB from SD/USB, rootfs on SD.
  Both confirmed working.
- **`bootstock`** -- chainloads the stock U-Boot binary from an SD file
  (`stockubootfile` env var, default `stock_uboot.bin`, sourced from
  `Prado firmware dump/mtd1-mtd2_uboot/extracted/uboot.bin`). Two real bugs
  found and fixed in this command tonight:
  1. Was calling `cleanup_before_linux()` before the jump -- too aggressive
     for a bootloader-to-bootloader handoff (disables MMU/interrupts/both
     caches, built for kernel handoff). Replaced with a narrower
     `flush_cache()` + `invalidate_icache_all()`.
  2. Was jumping to the wrong address. The code assumed (per the old,
     now-corrected section 8.3 of `UBOOT_BOOTLOGO_AND_RE_PORTS.md`) that
     Stepldr reads the ARK header's `EP` field and jumps there, skipping
     the reset vector. Verified false via the same Stepldr disassembly
     referenced in section 3: the actual load-and-jump routine (file
     offset `0x253c`-`0x2540`) hardcodes `mov r0, #0x30000; blx r0` -- it
     jumps to the load address (the reset vector / `_start`), completely
     ignoring the header's `EP`. Fixed: `bootstock` now does `go 0x30000`
     instead of `go <header EP>`. **Confirmed working on real hardware**
     (2026-07-13) -- the jump-target theory was correct.
- **`bootstockusb`** -- added after `bootstock` was confirmed working;
  identical logic sourced from a USB stick instead of SD (`fatload usb 0:1`
  vs `fatload mmc 0:1`). Shares `bootstock_from_block_dev(iface)`, same
  pattern as `bootmmc`/`bootusb`'s shared `boot_from_block_dev()`. Not yet
  hardware-tested (untested only in the sense of "USB vs SD as the
  source" -- the actual chainload logic is identical to the confirmed-working
  `bootstock`).

  **Important: USB/SD only supplies the stock U-Boot *binary itself* for
  that one initial chainload step.** Once stock U-Boot is running, it does
  its own completely normal boot sequence, which reads the kernel and
  rootfs from **NAND** (via its own field-proven NAND driver), exactly like
  the confirmed `bootstock`/SD test. `bootstock` and `bootstockusb` both
  end up in the identical place -- stock UI booted from NAND -- the only
  difference between them is which medium supplies the stock binary for
  the initial handoff. Neither one boots the kernel/rootfs from USB or SD.

`STOCK_UBOOT_LOAD_ADDR` = `0x30000`, safe to overwrite -- by the time any of
these commands run, this build has long since relocated itself to high RAM
(`bdinfo` shows ~63MB relocation address, confirmed no collision).

### `ark1668_debug_cmds.c`
New `nandoobcheck <offset-hex>` command -- dumps raw OOB (`MTD_OPS_RAW`,
bypasses ECC/BBT interpretation) for a page, printed alongside what the
driver's cached bad-block table believes about that block. This is the tool
that made the whole section 1 investigation possible; keep it.

### `ark1668_display_cfg.c`
`display_bootlogo_from_sd()` now `memset`s the framebuffer to black before
`fatload`ing `bootlogo.raw` -- an undersized/mismatched file used to leave
stale RAM content in the tail (bottom rows, since the framebuffer is
row-major), showing as colored-pixel artifacts. Now fails safe to black.

### `include/configs/ark1668_limcet_p305.h`
- `nandboot` env command: `run nandargs; switchecc 2; setenv machid 1068;
  nand read ${kerneladdr} kernel; bootz ${kerneladdr}`.
- `CONFIG_BOOTCOMMAND` (default, non-interrupted autoboot): try `bootusb`,
  then `bootstock`, then `run nandboot` as last resort. Deliberately does
  not fall back to the SD-card `uEnv.txt` flow.
- New env vars with compiled-in defaults, all overridable via
  `setenv`/`uEnv.txt`: `stockubootfile`, `kernelfile`, `dtbfile`, `mmcroot`,
  `bootargs_common`.

### `build_bootable_sdcard.sh` (main repo)
- `--stock-uboot PATH` / `--no-stock-uboot` -- copies the stock binary to
  `p1/stock_uboot.bin` for `bootstock`. Defaults to the dump already in the
  repo.
- (Also bundled, from earlier sessions, not tonight: `--bootlogo`,
  `--diag-tools`, two rootfs fixes -- see commit `5f4dcfc` for details.)

---

## 5. Still open -- not fixed tonight, worth knowing about before you start

### `bootnand` kernel-entry hang
With ECC and `machid` both fixed, and a CRC32-verified clean kernel image
in RAM, `bootz` still hangs silently right after printing
`machine ID r1 = 0x00001068` -- no crash, no further output. Ruled out:
- Data corruption (CRC32 consistent across repeated reads; hang persists on
  verified-good data)
- PL310/L2 cache config mismatch -- read `CTRL`/`AUX_CTRL`/tag+data latency
  live at `0x70000100` on both stock and this build's interactive prompt;
  byte-for-byte identical (`CTRL=0x00000000`, L2 disabled, on both)
- `bootz`'s appended-DTB auto-detection stealing the ATAGS pointer -- checked
  `cmd/bootz.c`: with no third arg it explicitly falls back to `bd_info`,
  not appended-DTB scanning
- Stale global `images.ft_addr`/`ft_len` leaking from an earlier
  `bootmmc`/`bootusb` call -- `bootm_start()` explicitly zeroes the whole
  struct on every invocation

Not yet tried: comparing `SCTLR`/`ACTLR` (ARM CP15 coprocessor registers,
not memory-mapped -- read via `mrc`, would need a small custom command) the
same way `BCH_CR` and PL310 were compared. Also: the actual kernel-jump code
in the stock binary was never found in the existing Ghidra decompile
(`docs/re_stock_uboot/full_decompile.c`) -- only one unrelated string hit
for "kernel". Finding it would need a fresh, targeted Ghidra pass (or more
`objdump`, same method as Stepldr/section 3), not a lookup in existing
artifacts.

This 3.4 kernel has only ever shipped paired with the stock 2012.10 U-Boot;
booting it via this 2018.07 fork's `bootz` is fundamentally unproven,
untested territory. **`bootstock` sidesteps this entirely and is confirmed
working end-to-end** (see the headline result at the top of this doc) --
handing the kernel boot to the binary it was actually built against boots
the full stock UI successfully. Given that, this hang is now a
lower-priority curiosity, not a blocker -- there's a completely reliable
path to a working NAND boot already. Worth solving only if direct
`bootz`-from-this-fork is specifically wanted for some other reason.

### GPIO button -- piggyback display switch
User confirmed this works correctly already, on real hardware, by the time
U-Boot is running (i.e., it's boot-firmware-level, not kernel/app-level).
Traced 14 `GPIO_BASE` (`0xE4600000`) references in stock `uboot.bin`
(`objdump` output saved at `/tmp/.../scratchpad/stock_uboot_disasm.txt` if
still present) down to generic GPIO driver plumbing (pin direction/mode
config, offsets `0x1c0-0x1cc`/`0x50-0x60` relative to a `GPIO_BASE` literal
pool at file offset `~0x6e1ec`), but did not trace which specific call
site/pin number is the actual button check -- that's a real board-application
function calling this generic driver code, not yet isolated. This is purely
informational curiosity (feature already works, on both stock and this
build), not a bug -- pick up only if there's a specific reason to want the
exact mechanism (e.g., porting it somewhere, or the feature breaks on a
future change).

---

## 6. `bootusb` USB rootfs -- fixed in source, real USB detection issue found separately

### root=/userdata device-following fix
`bootusb` previously always used `mmcroot` for the kernel's `root=` bootarg
regardless of which interface it was actually called with -- it loaded the
kernel from USB but still told it to mount root from the SD card. Fixed:
new `usbroot` env var (default `/dev/sda2`), `boot_from_block_dev()` now
picks `mmcroot` vs `usbroot` based on the actual interface. Also fixed a
stale status-print bug in `do_bootusb()` that kept printing the old
`mmcroot` value regardless of the actual fix (cosmetic, not functional,
but confusing to debug against). Separately, `build_bootable_sdcard.sh`'s
`rcS` patch for mounting `/data` (userdata) was hardcoded to
`/dev/mmcblk0p3` -- now derives the device at runtime from the kernel's
own `root=` bootarg (`ROOTDEV`/`USERDATADEV` in the generated `rcS`), so
userdata correctly follows onto `/dev/sda3` when booted via USB. Verified
by running the actual patch logic against the real rootfs `rcS` and
checking the result with `sh -n` -- not yet hardware-tested end-to-end.

### USB storage device not detected on port 0 -- real, unresolved, separate issue
While testing the above: the USB stick was not detected as a storage
device, neither by this U-Boot's own `usb start` (`0 Storage Device(s)
found`) nor by the kernel (`dmesg`/`lsmod`/`/sys/bus/usb/devices/` all
agree nothing enumerates on bus 1). This is a **different, deeper issue**
than the `root=` fix above and was NOT resolved tonight. What's confirmed:

- This board has two independent MUSB controllers/ports (established
  earlier this session for the U-Boot USB dual-port work): port 1
  (`musb-hdrc.1`, GPIO `ID=1`/`PWR=117`) is hardwired to the onboard WiFi
  module (`rtl8811cu`/`rtl8821cu`) and always enumerates successfully.
  Port 0 (`musb-hdrc.0`, GPIO `ID=76`/`PWR=126`) is the external,
  user-accessible port -- controller initializes cleanly
  (`musb_ark_probe succss`), but no device has ever been seen to enumerate
  on it under the kernel, even with a stick physically plugged in at boot.
- `usb-storage` and the MUSB host driver are compiled directly into the
  kernel (`CONFIG_USB_STORAGE=y`, `CONFIG_USB_MUSB_HDRC=y`, not modules --
  confirmed via `lsmod` showing neither, and the kernel config), so this
  isn't a missing-module problem.
- The port itself is known-good electrically -- stock U-Boot successfully
  read a kernel file off a USB stick on this exact port earlier in the
  session, ruling out a simple hardware/cabling fault.
- The WiFi module's successful detection proves the kernel's underlying
  USB hotplug/enumeration mechanism works correctly in general (Linux's
  hub driver treats "device already connected when the hub thread starts
  polling" identically to a genuine hotplug event -- there's no separate
  code path), which rules out "hotplug detection is fundamentally broken"
  as an explanation. The issue is specific to port 0, not the driver stack
  in general.
- Leading unconfirmed theory: something specific to port 0's runtime
  connect-detection (GPIO ID-pin sensing, VBUS sensing, or MUSB's
  software-emulated root-hub status-change reporting for that specific
  port) isn't working, even though the controller's own init/probe
  succeeds. Not yet isolated further -- would need either comparing
  `/sys/kernel/debug/gpio` (or a raw register read via `devmem`/`md.l` at
  `GPIO_BASE + (2*0x20) + 0x04 = 0xE4600044`, bit 12 for pin 76) between
  stick-plugged and stick-unplugged states, or reading the actual kernel
  MUSB driver source for how it differs between initial-probe detection
  and ongoing hotplug polling.

---

## 7. `help` command corruption -- real fix applied, did NOT resolve it, deprioritized as cosmetic

On real hardware, running `help` (no arguments) on this build reproducibly
corrupts its own printed output -- garbled `▒` characters scattered through
the list, the `md` command missing from its expected alphabetical position,
`mm`/`mmc` entries corrupted mid-description, the final `version` line
truncated mid-sentence. **100% reproducible on every fresh boot** (not
session-state-dependent), and **specific to `help`** -- other long-output
commands don't show it, which rules out a serial/UART hardware issue.

**Fix applied, ruled out as the (sole) cause:** `_do_help()`
(`common/command.c`, core U-Boot, not board-specific) built its sorted
command list in a stack-allocated VLA (`cmd_tbl_t *cmd_array[cmd_items]`)
sized by the total number of registered commands, several stack frames
deep in the command-dispatch chain, with no bound -- a real, legitimate bug
given how large this board's command table has grown across sessions.
Fixed by heap-allocating it instead (with proper `free()` on both return
paths). **Corruption persisted after this fix, on real hardware** -- so
either this wasn't the actual cause, or it's one of multiple contributing
factors. The heap-allocation fix is still correct and worth keeping
regardless (removes a genuine unbounded-stack-growth risk), just not a
complete explanation.

**Not yet tried:** U-Boot's `printf()` itself (`lib/vsprintf.c`) allocates
a `~538`-byte (`CONFIG_SYS_PBSIZE`) buffer on the stack on *every single
call* -- `_do_help()`'s print loop calls it repeatedly (once per command,
70+ times) from deep in the dispatch chain, a pattern that doesn't occur
elsewhere in this codebase. Flagged as a real candidate but not confirmed
or fixed -- would need actual stack-frame/disassembly analysis to verify,
not just static reading.

**Status: deprioritized.** User confirmed this is cosmetic only -- doesn't
affect any actual boot path or working feature (`bootstock`, the NAND ECC
fix, `bootmmc`/`bootusb` all work regardless). Left unresolved on purpose;
revisit only if it starts affecting something that matters, or if someone
wants to actually chase the `printf()` stack-buffer theory to ground truth.
