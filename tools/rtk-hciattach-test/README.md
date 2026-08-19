# rtk-hciattach-test

One-shot diagnostic: attempts to bring `hci0` up against the real
onboard Bluetooth module (Feasycom BW121, Realtek RTL8761BTV silicon)
via the standard Linux kernel HCI UART driver, instead of the stock
`/usr/bin/blueware` daemon this project's own Bluetooth stack normally
uses. Answers one specific open question from
`docs/BLUEZ_AND_KERNEL_BLUETOOTH_HANDOFF.md`: does this exact module
actually negotiate standard Bluetooth HCI over `/dev/ttyHS1` under the
kernel's H5 3-wire line discipline, or is `blueware`'s AT-command
protocol load-bearing at the wire level (not just a host-side API
choice)?

**Not a stack switcher.** Doesn't touch any UI, doesn't run
automatically, doesn't modify anything permanent. Run it manually, look
at the result, decide what's next.

## GPIO91 handling now lives inside `rtk_hciattach` itself (2026-08-19)

Used to be a shell-side pulse in `bt-hci-probe.sh` before exec'ing
`rtk_hciattach`, but that can't replicate two real properties of
`blueware`'s own working sequence (`/usr/bin/blueware`, decompiled via
Ghidra against the real binary):

1. `blueware` opens `/dev/ttyHS1` **while GPIO91 is still low** from
   its first reset pulse, and never closes/reopens that fd through to
   real vendor init -- a separate shell pulse followed by a fresh
   process opening its own new fd can't replicate "already listening,"
   and risks losing anything the chip emits on the wire around a
   pulse.
2. `blueware`'s real log shows genuine elapsed wall-clock time
   (`audio_control_init`, 4x `lib_serial_port_init`, several AT status
   broadcasts, 2 NVM writes) between its first reset pulse and the
   real pulse pair immediately before vendor init -- not a tight loop.

`src/hciattach.c` now does this itself, in the same process that goes
on to do H5 sync: `bt_gpio91_init_and_first_pulse()` (called before
`init_uart()` opens the fd) then `bt_gpio91_settle_and_final_pulse()`
(called from inside `init_uart()`, right after `open()` succeeds,
before the fd is ever touched again) -- an 800ms settle delay
approximates blueware's real intervening work (a labeled estimate, not
a datasheet-confirmed number -- blueware's own log has no per-line
timestamps to measure it exactly), then the real low/high/low/high
pulse pair at 150ms each (the real, sourced RTL8761ATT datasheet
minimum is `>100ms`, section 3.3.3).

Not yet hardware-tested. Next run is what tells us whether either or
both of these (fd held open across the pulse, real settle time between
pulses) actually matter, versus the `fc17`/`fc20` download-phase race
being a separate, unrelated bug.

## Firmware A/B test / H4 vs H5 protocol test

```sh
bt-hci-probe.sh                     # default: real firmware, H5 protocol
bt-hci-probe.sh device rtk_h5       # same, explicit
bt-hci-probe.sh reference           # rtkbt's own bundled generic firmware instead
bt-hci-probe.sh device rtk_h4       # real firmware, simpler H4 protocol instead of H5
```

H4 has no SYNC/CONFIG handshake -- useful for telling apart "the wire/
baud is fundamentally dead" (H4 would also get nothing) from
"something specific to H5's own handshake is wrong" (H4 might get a
response where H5 doesn't). Not a flow-control question either way --
checked `ark1668-pinctrl.dtsi`'s `pinctrl_uart5` group directly: this
board's UART5 only muxes RX/TX, no RTS/CTS pins exist on it at all,
confirming `UART_FLOWCTL=0` matches real board wiring, not just
software preference.

Both firmware sources are bundled directly in this directory --
`device-firmware/` (a copy of `firmware_source/mtd6_rootfs/etc/
rtl8761bt_fw`+`rtl8761bt_config`) and `reference-firmware/` (`rtkbt`'s
stock `rtl8761b_fw`/`rtl8761b_config`, github.com/radxa/rtkbt, GPLv2,
unmodified) -- so this whole tool is self-contained and doesn't depend
on `/etc/rtl8761bt_fw` still being present/unmodified on whatever
device it's copied to. Byte-for-byte different from each other
(41,328 vs 43,980 bytes; different bytes immediately after the shared
`"Realtech"` header signature). Useful for telling apart "our specific
firmware blob has an issue" from "this is a `rtk_hciattach`/board-level
issue a known-community-tested generic firmware would also hit" --
chip identification goes through the same patched `lmp_subver=0x434d`
table entry either way, only the staged file content differs.

## GDB debugging

The `fc17`/`fc20` baud-switch race (`h5_post_hci_cc: Received
unexpected cc for cmd fc17, fc20 of cc`) reproduces consistently --
confirmed twice with two different firmware files (`device` and
`reference`, both hit the identical error at the identical point).
Static reading traced the *shape* of the bug but hit a real
contradiction: `h5_post_hci_cc()` is only reachable when
`rtb_cfg.link_estab_state == H5_HCI_RESET` (checked directly in
`hci_recv_frame()`'s dispatch table), yet `link_estab_state = H5_PATCH`
is set unconditionally before any download packet is ever sent
(`src/hciattach_rtk.c` around `start_download:`). By the code's own
logic `h5_post_hci_cc()` shouldn't be reachable at that point at all --
worth settling with real state, not another guess.

```sh
bt-hci-probe.sh device rtk_h5 gdb   # launches rtk_hciattach under gdbserver :2345 instead
```

On your host machine (needs `gdb-multiarch`: `sudo apt-get install -y
gdb-multiarch`; needs network reachability to the device, not this
sandbox -- this step has to run on your own machine):

```sh
gdb-multiarch /path/to/local/copy/of/rtk_hciattach-debug
(gdb) target remote <device-ip>:2345
(gdb) b h5_post_hci_cc
(gdb) continue
```

`rtk_hciattach-debug` (unstripped, `with debug_info`, built from the
exact same object files as the shipped `rtk_hciattach` -- only the
`-s` strip flag differs, so code/data addresses match) is bundled here
for exactly this -- copy it to your host for symbols; the *device*
still runs the normal stripped `rtk_hciattach` under `gdbserver`.

When the breakpoint hits:

```
(gdb) print rtb_cfg.link_estab_state
(gdb) print rtb_cfg.cmd_state
```

If `link_estab_state` really is `H5_HCI_RESET` at that point, the real
bug is upstream of `h5_post_hci_cc()` -- something is setting/leaving
that state wrong before download starts, not a missing opcode
assignment. If it's genuinely `H5_PATCH`, that's even more surprising
and means `hci_recv_frame()`'s dispatch itself isn't matching the
static read -- either way, this settles it with real data instead of
another static-analysis guess.
Notably, our real firmware's header bytes right after `"Realtech"` are
literally `4d 43` -- ASCII `"MC"`, which as a little-endian 16-bit
value is `0x434d`, exactly the customized LMP Subversion our real chip
reports on every run. `rtkbt`'s generic firmware has different,
non-matching bytes there. Strong circumstantial evidence this 2-byte
field is a build/target-chip-ID tag, and our real firmware was
specifically built for a chip reporting `0x434d` -- not a generic
target. Worth watching whether the `reference` firmware behaves
differently as a result.

## Status: two hardware runs so far, chip ID recognized, one gate left (2026-08-19)

**Run 1**: H5 sync/config handshake completed cleanly (`H5 init
finished`) -- confirms the module genuinely speaks standard Realtek
3-wire HCI over the wire, settling the one question this tool exists
to answer. Then failed to initialize: the real chip reports
`LMP Subversion 0x434d` ("CM" in ASCII) and `HCI Revision 0x6ca9`,
neither matching `rtkbt`'s stock `RTL8761BTV` table entry
(`0x8761`/`0x000b`) -- almost certainly Feasycom customizing the
reported ID on an otherwise-genuine RTL8761BTV die. Patched
`src/rtb_fwc.c`'s chip-matching table to recognize `lmp_subver=0x434d`.

**Run 2** (same patched binary): chip ID now recognized
(`IC: RTL8761BTV (Feasycom BW121)`), firmware/config loaded correctly
(exact byte count, config bytes match this project's real
`etc/rtl8761bt_config` verbatim). Hit a second, separate gate: the
firmware blob's own internal "project ID" byte (`proj_id=14`) maps to
`project_id[14]=0x8761` in a hardcoded table, and `rtb_get_final_patch()`
compares that directly against the chip's real (customized) `lmp_subver`
-- same root cause as the first gate, just a second place it shows up.
Patched that one comparison to also accept `lmp_subver=0x434d` as a
valid alias for `0x8761` (see `src/rtb_fwc.c`'s own comment right at
that check) -- every other chip's check is unaffected.

**Run 3** (fresh power cycle, this script's old low-then-high GPIO91
pulse): zero H5 SYNC responses at all -- totally silent chip,
retransmission exhausted before ever getting past the very first
stage.

**Run 4** (same script, but run after `custom_ui`/`blueware` had
already brought the module up once earlier in that boot, then
`blueware` killed): got three full stages further than Run 3 -- H5
sync, chip ID, firmware/config load all succeeded, then hit a
timing-sensitive failure during the live baud-rate switch right as
patch download started (`h5_post_hci_cc` mismatch, `fc17`/`fc20`).

The Run 3 vs. Run 4 contrast pointed at GPIO91 handling as the real
variable, but the follow-up chase down that path (documented here
originally, now corrected) went the wrong direction twice: first
removing the pulse entirely (matching a misread of `blueware`'s own
static-enable doc excerpt, `docs/1.4_WIRELESS_AND_INIT.md:129-134`),
then restoring it as a **double** low-then-high cycle at 100ms
(matching `blueware`'s decompiled `bpio_reset`/`bpio_set`/`bpio_reset`/
`bpio_set` sequence), then bumping that to 150ms (matching a real
Realtek RTL8761ATT datasheet's `>100ms` EN_CHIP minimum, section
3.3.3). All of that was reasonable *theory*, but **neither of the two
double-pulse versions has ever once succeeded on real hardware** -- two
separate tests (one cold, one warm/after-blueware) both went
completely silent, zero H5 SYNC responses.

**Reverted to the single pulse from this tool's very first commit**
(one low->high cycle, 100ms) -- the only sequence that has actually
worked on this hardware, twice (Run 2 and Run 4, both warm/after-
blueware). Evidence beats theory here: whatever `blueware`'s own
second reset cycle is actually for, replicating it broke something
about getting the chip to respond at all. The real `>100ms` EN_CHIP
minimum from the datasheet is kept in mind, but 100ms is left as-is
since it's the exact value already proven twice, not bumped further
without hardware evidence backing a change.

**Not yet re-tested against hardware since this revert** -- next run
(both cold and warm/after-`blueware`) is what confirms whether this
restores the Run 4 baud-switch-race result, or whether something else
has also changed in the meantime.

## Run 5 (2026-08-19): full userspace HCI handshake succeeds end-to-end

After moving GPIO91 handling into `rtk_hciattach` itself (fd held open
across the pulse, settle delay before the final pulse pair -- see the
section above) and, critically, **dropping the `-s 1500000` override**
(`bt-hci-probe.sh` was forcing the *initial* UART open/H5 SYNC to
1.5Mbps on every run all session; `rtk_h5`'s own table default is
115200, matching what a genuinely cold chip's ROM/bootloader actually
listens at -- see that commit's message for the full theory), a real
run succeeded completely:

- H5 SYNC/CONFIG handshake clean.
- Chip ID this time reported as **standard** `LMP Subversion 0x8761`,
  `HCI Revision 0x000b` -- NOT the customized `0x434d`/`0x6ca9` seen on
  every single prior run this session. Strong evidence that the
  `0x434d` "customized ID" was never real -- it was an artifact of
  probing the chip's boot ROM at the wrong (1.5M) baud, which this
  particular ROM/firmware combination apparently still partially
  responds to but misidentifies through. The `rtb_fwc.c` chip-table
  patch for `0x434d` may turn out to be unnecessary now; not yet
  reverted since it's harmless (an additional accepted alias, not a
  replacement) and removing it isn't worth the risk before more runs
  confirm `0x8761` is now consistently what's reported.
- Firmware (43,980 bytes) and config (33 bytes) loaded, byte-exact.
- Baud switch to 1,500,000 completed (`fc17` CC received cleanly).
- Full patch download: all 122 packets sent (`end_idx 117` + 5
  additional + final), no `h5_post_hci_cc` race this time.
- `h5_hci_reset` issued, its CC (`0x0c03`) received correctly.
- `Init Process finished.` -- the entire userspace Realtek HCI
  handshake, start to finish, worked.

The run then failed at the very last step, and it's purely kernel-side:

```
ERROR: Can't set line discipline 22, Invalid argument
ERROR: Can't initialize device 22, Invalid argument
```

`errno 22` is `EINVAL` from `ioctl(fd, TIOCSETD, &i)` with `i = N_HCI`
(`src/hciattach.c`, right after `init_uart()`'s UART-side work is
done) -- the *currently running* kernel has no `N_HCI` line discipline
registered at all, because it doesn't have `CONFIG_BT_HCIUART` built
in. This is exactly the kernel already built earlier this session
(`hardware/kernel_dot_config`, commit `ed37ab2`: `CONFIG_BT=y`,
`CONFIG_BT_HCIUART=y`, `CONFIG_BT_HCIUART_3WIRE=y` plus their real
`CONFIG_SERIAL_DEV_BUS`/`CONFIG_SERIAL_DEV_CTRL_TTYPORT` prerequisites)
-- built successfully (`zImage.w_dtb`) but never flashed/booted. That's
the next and, as far as this tool's own scope goes, likely final step:
flash that kernel and re-run this script. If `TIOCSETD`/`HCIUARTSETPROTO`
succeed, `hci0` should come up under the standard kernel Bluetooth
stack for the first time on this hardware.

## What's here

- `rtk_hciattach` — static ARM binary, built from `src/` (patched copy
  of [radxa/rtkbt](https://github.com/radxa/rtkbt)'s
  `uart/rtk_hciattach`, GPLv2 — see `src/rtb_fwc.c`'s own comment for
  the one deliberate change from upstream).
- `src/` — the patched source, so the chip-ID patch above is reviewable
  and rebuildable, not just baked into an opaque binary. Rebuild with
  `make CROSS_COMPILE=arm-linux-gnueabihf-` then
  `arm-linux-gnueabihf-gcc -static -s -o rtk_hciattach *.o`.
- `bt-hci-probe.sh` — wires it up against this device's real config:
  `/dev/ttyHS1` at 1,500,000 baud, GPIO 91 hardware reset (matches
  `etc/blueware-bw121.properties`'s own `BTEN_INTERFACE=gpio91`), and
  stages this device's real firmware blobs (`etc/rtl8761bt_fw` /
  `etc/rtl8761bt_config`) into `/lib/firmware/rtlbt/rtl8761b_fw` /
  `rtl8761b_config` -- the exact filenames `rtkbt`'s own chip table
  expects for its `RTL8761BTV` entry (`rtb_fwc.c`), a near-exact match
  to this project's own file naming.

## Why `RTL8761BTV`, not `RTL8762BTV`

`docs/1.4_WIRELESS_AND_INIT.md`'s own hardware table calls this chip
`RTL8762BTV`. `rtkbt`'s chip-matching table has no such chip, but does
have an exact `RTL8761BTV` entry whose expected firmware filenames
(`rtl8761b_fw`/`rtl8761b_config`) are a near-exact match for this
project's real, already-extracted files (`rtl8761bt_fw`/
`rtl8761bt_config` -- note the extra `t`). Strong circumstantial
evidence the "2" in the existing doc is a digit transcription error,
not a different chip -- not yet independently confirmed against the
real silicon markings/datasheet.

## Prerequisites (not yet done)

The running kernel needs `CONFIG_BT=y`, `CONFIG_BT_HCIUART=y`,
`CONFIG_BT_HCIUART_3WIRE=y` at minimum -- `hardware/kernel_dot_config`
currently has `CONFIG_BT=y`/`CONFIG_BT_HCIUART=y` but none of the
UART-protocol driver options (`H4`, `3WIRE`, `RTL` aren't even
Kconfig-visible without 3WIRE support built). **Deliberately not
touched yet** -- no kernel rebuild until this tool has actually been
run against real hardware and shown whether the H5 handshake completes
at all. A kernel change that can't be exercised is wasted risk.

## Usage

On the device, with a serial/SSH shell:

```sh
pidof blueware      # find it
kill <pid>           # stop it -- it owns /dev/ttyHS1, only one owner at a time
/usr/bin/bt-hci-probe.sh
```

Watch the output for the H5 sync/config/firmware-download sequence
completing without error, then in a second shell:

```sh
ls /sys/class/bluetooth/     # hci0 present?
cat /sys/class/bluetooth/hci0/address   # real BT MAC if it worked
```

Ctrl-C stops `rtk_hciattach` and releases `/dev/ttyHS1` (Bluetooth
stays down until `blueware` is restarted manually -- reboot is the
simplest way back to normal, or re-run whatever launches `blueware`).

## How to read the result

- **`hci0` appears, real MAC readable**: the module genuinely supports
  kernel HCI mode -- the BlueZ path is real and worth the kernel work
  (§3 of the handoff doc) plus adding `bluetoothd`/BlueZ userspace to
  the rootfs.
- **`rtk_hciattach` hangs or errors during the vendor sync/firmware
  upload**: the module's actual runtime behavior doesn't match a
  standard `RTL8761BTV`, or the firmware blob needs different framing
  than `rtkbt` expects -- the BlueZ path is likely dead regardless of
  further kernel config work.
