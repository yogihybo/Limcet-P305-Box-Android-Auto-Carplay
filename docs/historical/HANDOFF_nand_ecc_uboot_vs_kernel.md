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

**Headline results (2026-07-13/14, confirmed on real hardware):**
- `bootstock` works end-to-end -- this build (booted from SD) chainloads
  the stock U-Boot binary, which then successfully boots the full stock
  kernel + rootfs + UI from NAND. This is now a completely reliable path
  to the original stock system from a custom-U-Boot-on-SD setup, and it
  sidesteps the `bootnand` kernel-entry hang (section 5) entirely -- that
  issue no longer blocks getting a working NAND boot, it only blocks
  doing it via *this* fork's own `bootz` directly.
- `bootusb` with `usbroot` works end-to-end and is now confirmed safe --
  kernel, DTB, *and* root filesystem all loading and booting entirely
  from a USB stick, no SD card or NAND involved at all for the running
  system. Needed three real fixes to get fully working and safe (see
  section 6): a kernel driver VBUS-settle-delay fix, disabling an `rcS`
  workaround that unbound the live root device, and hardening a
  pre-existing vendor USB recovery watchdog so it no longer disconnects
  an already-working port. All three confirmed together on real
  hardware -- a full boot cycle with no disconnects, I/O errors, or
  filesystem corruption.

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

**Loose end resolved (2026-07-15):** The best working theory in this section was confirmed to be correct. Because the warm jump does not reset the SoC, any peripheral register state modified by custom U-Boot's NAND driver (specifically `rBCH_CR` configuration register) remains active. Stock U-Boot's OR-based initialization logic in `ark_hwecc_nand_init_param()` then ORs its configuration bits on top of this polluted baseline instead of a clean `0` POR baseline. This results in a wrong composite ECC config, causing all stock U-Boot NAND page reads to fail with ECC errors (surfacing as "kernel magic doesn't match" on the kernel read). 

Explicitly zero-initializing `rBCH_CR` and other relevant controller registers right before executing the warm jump (emulating a Power-On Reset) solves this issue completely. See section 4 for the implementation details.

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
testing -- see `docs/historical/HANDOFF_touch_and_bootargs_fix.md` "Fix C", the ~417
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
  `Prado firmware dump/mtd1-mtd2_uboot/extracted/uboot.bin`). Three real bugs
  found and fixed in this command:
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
  3. Stale NAND ECC controller state left over from the warm handoff (fixed 2026-07-15).
     Since the warm jump does not reset the SoC, any registers modified by the custom U-Boot's
     NAND driver (specifically `rBCH_CR`) remain populated. Stock U-Boot's OR-based
     initialization logic in `ark_hwecc_nand_init_param()` then ORs its configuration bits on top
     of this polluted baseline, leading to corrupted ECC configs and "kernel magic doesn't match" errors.
     Fixed: `bootstock` now zero-initializes the BCH and NAND configuration/control/status registers
     (`rBCH_CR`, `rBCH_INT`, `rBCH_INT_MASK`, `rNAND_DMA_CTRL`, `rNAND_GLOBAL_CTL`, `rNAND_JUMP_CTL`, and `rNAND_CR`)
     immediately before performing the warm jump, emulating a cold-boot POR state.
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

## 6. `bootusb` USB rootfs -- FIXED and confirmed working end-to-end, one bug found and disabled along the way

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
userdata correctly follows onto `/dev/sda3` when booted via USB.

### USB storage not detected on port 0 -- root-caused and FIXED (kernel driver)
Initially: the USB stick was not detected as a storage device, neither by
this U-Boot's own `usb start` (`0 Storage Device(s) found`) nor by the
kernel (`dmesg`/`lsmod`/`/sys/bus/usb/devices/` all agreed nothing
enumerated on bus 1), even with the stick physically plugged in at boot.
Ruled out along the way: missing kernel module (both `usb-storage` and the
MUSB host driver are compiled directly into the kernel, confirmed via
`lsmod` and the kernel config); a broken/dead port (stock U-Boot
successfully read a kernel file off this exact port); a fundamentally
broken hotplug mechanism (the onboard WiFi module on port 1 enumerates
fine via the identical code path -- Linux's hub driver treats "already
connected when polling starts" the same as a live hotplug event, so if
that path were broken, the WiFi module wouldn't work either).

**Root cause, found by reading the actual kernel driver source**
(`linux/drivers/usb/musb/musb_ark.c`, `ark_musb_set_mode()`, the
`MUSB_OTG` case): port 0 runs in OTG dual-role mode, and every driver
init power-cycles the port's VBUS (`gpio_pwr`) as part of setup -- with
only a **10ms** gap between turning it off and back on, and **no
wait-for-connect check at all** afterward (unlike `ark_musb_set_vbus()`,
a few dozen lines below in the same file, which already has the right
pattern -- poll a status bit with a timeout -- for a similar problem). If
a device is already plugged in when this runs, it loses power and needs
real time to fully re-initialize internally before it can respond to
enumeration again. Confirmed via the **stock application's own log**
(`MsnCoreApp`, a genuine real-hardware capture): even stock shows several
seconds between "new device" and the device being usable, plus explicit
`open()` retry logic on the userspace side for exactly this reason.

**Fix**: bumped the post-restore-VBUS delay from 10ms to 300ms
(`mdelay(300)`) in `ark_musb_set_mode()`. **Confirmed working on real
hardware**: after this fix, `bootusb` with `usbroot` successfully
detected the stick (via the driver's own existing `musb_recovery_usb_proc`
retry logic, which now had enough settle time to succeed within a couple
of automatic retries), mounted `/dev/sda2` as root, and reached
`Run /sbin/init as init process`. **This is the headline result of this
section: booting the kernel, DTB, and root filesystem entirely from a USB
stick now works.**

### A second, more serious bug found in the same test -- since disabled
The same boot log that proved the kernel fix works also caught a real,
actively harmful bug in the *other* fix from this session (the `rcS`
unbind/rebind workaround, section 6 as originally written, meant to force
a fresh USB probe after boot for the SD/NAND-root case). That workaround
ran unconditionally, including when root itself was already mounted from
USB on that exact same controller (`musb-hdrc.0`) -- unbinding the driver
serving your *live root filesystem* yanks it out from under the running
system. The log shows exactly this cascade: `musb-hdrc.0: remove` →
`USB disconnect` → I/O errors on `sda` → `Aborting journal on device
sda2` → `EXT4-fs (sda2): Remounting filesystem read-only` → every
subsequent `rcS` command failing with `Input/output error` (`ln`, `cat`,
`sed`, `chmod`, `mkdir`, `modprobe`, `sshd` all failed).

**Fix applied**: the unbind/rebind commands in the generated `rcS` block
are now commented out (see `fix_usb_port0_otg_race()` in
`build_bootable_sdcard.sh`), and the corresponding `CONFIG_ITEMS` toggle
default flipped to `OFF`. The kernel-level VBUS-delay fix above has been
confirmed sufficient on its own for the boot-from-USB case, so this
workaround is not currently needed for that scenario -- and doing it
safely for the *other* scenario (root on SD/NAND, want to also detect a
separate USB drive at runtime) would need the workaround to first check
whether root is already on this same USB bus before touching it, which
has not been implemented. Don't re-enable this toggle without adding that
check first.

### A third bug, found in the *same* log, after disabling the second one
Disabling the `rcS` workaround didn't fully stop the disconnect-and-corrupt
pattern -- the same log also shows it happening again, later, well into
normal runtime (after `crng init done`, long past boot), with no `rcS`
script involved at all this time:
```
musb_reset_timer_handler
musb_recovery_usb_proc reset otg.
musb-hdrc musb-hdrc.0: +Switch peripheral 76  126===
musb-hdrc musb-hdrc.0: +++Switch OTG 76  126===+++
usb 1-1: USB disconnect, device number 5
sd 0:0:0:0: [sda] tag#0 UNKNOWN(0x2003) ...
print_req_error: I/O error, dev sda, ...
```

**Root cause**: `musb_recovery_usb_proc()`/`musb_reset_timer_handler()`
(`drivers/usb/musb/musb_core.c`) are a **pre-existing, vendor-written
watchdog**, unrelated to anything from this session's own changes. It's
armed by `musb_reset_usb_controller()` (`musb_host.c`), which is a
`hc_driver.reset_usb_controller` callback invoked from **generic upstream
Linux USB core code** (`drivers/usb/core/hub.c`'s `hub_port_init()` retry
loop) whenever a port-enable attempt fails for *any* reason -- including a
transient signal glitch during otherwise-normal runtime, not just at
initial connect. When it fires, the recovery work unconditionally forces
a `MUSB_PERIPHERAL` → wait 1s → `MUSB_OTG` mode-switch cycle, which routes
through the *same* `ark_musb_set_mode()` VBUS power-cycle patched above --
turning a possibly-minor, possibly-self-recovering glitch into a real,
physical disconnect of whatever's currently attached. If that's the
device serving as root, same corruption cascade as before, just from a
different, harder-to-predict trigger (can fire at any time during normal
operation, not just at boot).

**Fix applied** (`musb_recovery_usb_proc()`, `musb_core.c`): check
`musb->port1_status & USB_PORT_STAT_ENABLE` before doing the disruptive
reset -- skip it if the port already has a working, enabled connection.
A genuinely stuck/dead port has nothing to lose from the reset; an
already-working one has everything to lose.

**CONFIRMED WORKING on real hardware (`990417616`)**: booted `bootusb`+
`usbroot` again on this kernel. Root mounted from `/dev/sda2` successfully
(after the normal, legitimate port-enable retries -- nothing was attached
yet at that point, so those retries were fine). Well into normal runtime,
after `crng init done`, the watchdog fired again exactly like before --
but this time logged `musb_recovery_usb_proc port already enabled,
skipping disruptive VBUS reset` instead of forcing the VBUS cycle. No
disconnect, no I/O errors, no journal abort -- boot continued straight
through cleanly (`init`, the `mtd8-11` symlinks, `galcore`/`rtl8821cu`
loading normally). All three USB fixes from this session (VBUS settle
delay, the disabled `rcS` workaround, and this recovery-watchdog guard)
are now confirmed working together. Boot-from-USB is no longer known to
be at risk of this corruption pattern.

The underlying *trigger* for `hub_port_init()` failing well into normal
runtime (not just at boot) is still not identified -- the watchdog now
just handles it safely instead of making it worse. Worth investigating
further only if the recurring "Cannot enable" retries themselves become a
practical annoyance (e.g. noticeable USB throughput hiccups); not urgent
given the corruption risk is now closed off.

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
