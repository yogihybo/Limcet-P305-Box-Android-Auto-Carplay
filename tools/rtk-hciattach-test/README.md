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

## Status: H5 handshake confirmed real, chip auto-detect patched (2026-08-19)

Run once already against real hardware. Result: the H5 sync/config
handshake completed cleanly (`H5 init finished`) -- the module
genuinely speaks standard Realtek 3-wire HCI over the wire, settling
the one question this tool exists to answer. It then failed to
initialize because the real chip reports `LMP Subversion 0x434d`
("CM" in ASCII) and `HCI Revision 0x6ca9`, neither of which match
`rtkbt`'s stock `RTL8761BTV` table entry (`0x8761`/`0x000b`) -- almost
certainly Feasycom customizing the reported ID on an otherwise-genuine
RTL8761BTV die (see `src/rtb_fwc.c`'s own comment on the patch below
for the full reasoning). `src/` now carries a small, documented patch
adding a table entry that matches this exact `lmp_subver` and reuses
the confirmed-correct `rtl8761b_fw`/`rtl8761b_config` filenames.
**Not yet re-tested against hardware** -- next real-hardware run is
what confirms or refutes this.

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
