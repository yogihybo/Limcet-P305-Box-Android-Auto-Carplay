# Wireless And Init

**Status:** Reference
**Last Updated:** 2026-07-16

## Overview

This document explains the physical pin mapping, modules, and commands required to initialize WiFi and Bluetooth on the Prado Limcet P305 board running the 4.19.192 kernel.

---

## 1. Hardware Architecture & Interfaces

| Component | Chip Model | Bus / Interface | Control GPIO |
|---|---|---|---|
| **WiFi** | Realtek RTL8811CU | USB 2.0 (Bus 2, Port 1) | None (USB bus-powered) |
| **Bluetooth** | Realtek RTL8762BTV | HS-UART (`/dev/ttyHS1`) | **GPIO 91** (Enable pin) |

---

## 1a. WiFi never enumerated during `bootusb` -- root cause found and fixed 2026-07-17

`musb-hdrc.0`'s hub reports exactly **one** downstream port
(`hub 1-0:1.0: 1 port detected`), and every `docs/logs/new kernel
bootlog*.txt` capture showed a 100% consistent correlation: WiFi only
ever came up (`rtw_ndev_init(wlan0)`, `hostapd ... AP-ENABLED`) on boots
using `root=/dev/mmcblk0p2` (`bootmmc`, SD card); it never enumerated at
all on boots using `root=/dev/sda2` (`bootusb`, USB stick). Initial
theory was that the boot stick and the onboard WiFi module were
contending for the same single physical port -- true as far as it went,
but didn't explain how **stock** manages both a USB stick and wireless
CarPlay at once.

Real root cause: this board has **two** separate USB controllers,
`usb0` (0xE0100000, external-facing port) and `usb1` (0xE0400000, almost
certainly the onboard WiFi module's dedicated controller) -- confirmed
because `musb-ark e0400000.usb: Failed to get irq.` /
`probe of e0400000.usb failed with error -22` appeared in **every**
boot log, working or not, meaning `usb1` never successfully probed at
all, regardless of boot medium. Traced to a bad device-tree override in
`ark1668_limcet_p305.dts` (`interrupts = <40>, <39>`, added 2026-07-16,
bundled into an unrelated I2S commit) -- `usb1`'s interrupt-parent
(`vich`) is a single `arm,pl192-vic` with only 32 lines (valid range
0-31); 40 and 39 are out of range for that domain, and exactly equal
`ark1668.dtsi`'s original, correct values (8, 7) plus 32 -- a global-vs-
VIC-local interrupt numbering mix-up. Fixed by dropping the override.

**CONFIRMED on real hardware (2026-07-17)**: `bootusb` with a physical
USB stick plugged in, `wlan0: AP-ENABLED` (`carplay_wifi`, same MAC as
every prior known-good capture), and `usb-test.sh` shows **2 USB hubs
enumerated** (`musb-hdrc host0 + host1` -- both controllers now
probing successfully) with the WiFi driver bound and `wlan0` present.
`usb1`/WiFi and `usb0`/boot-stick now work at the same time, matching
stock -- the SD-card-boot workaround is no longer needed.

---

## 2. Boot Script Setup (`/etc/rc.d/rcS`)

During boot, the following steps are performed:
1. **Modules Path Redirect:** The legacy path `/lib/modules/3.4.0` is symlinked to the running `/lib/modules/4.19.192` kernel directory.
2. **Symlinks for Wireless Drivers:** A symlink for `wlan_rtl8811cu.ko` -> `rtl8811cu.ko` is created dynamically so userspace scripts expecting the legacy name don't break.
3. **Module Auto-Load:** The `rtl8811cu` WiFi driver is automatically loaded on boot.

---

## 3a. Boot mode: back to stock default (AP) as of 2026-07-17

`rc.d/rcS` calls `etc/wifi_ap.sh` at boot again, hosting the
`carplay_wifi` AP for wireless CarPlay — matching real device behavior.
Between 2026-07-14 and 2026-07-17 this called `etc/wifi_client.sh`
instead (client/STA mode, joining a real local network for easier
SSH/testing access during development) — reverted once that testing
period was done. A single WiFi radio generally can't be an AP and a
client at once, so only one of the two runs at boot; the other stays
available to run by hand (`/etc/wifi_client.sh &`, after editing
`etc/wifi_client.conf` with real SSID/password first — it ships with
placeholder values that will never connect).

## 3. WiFi Access Point Setup Commands (runs automatically at boot -- see 3a)

To manually bring up the wireless access point or verify the configuration:

```bash
# 1. Load the compiled Realtek USB WiFi driver
modprobe rtl8811cu

# 2. Check that wlan0 and wlan1 interfaces are registered
ifconfig -a

# 3. Create the DHCP lease directory and file
mkdir -p /data
touch /data/udhcpd.leases

# 4. Bring up the interfaces and start hostapd
/etc/wifi_ap.sh
```

---

## 4. Bluetooth Setup Configuration

The Bluetooth stack runs via the Feasycom properties configuration in `/etc/blueware-bw121.properties`. 

```ini
# gpio for BT_ENABLE hardware pin control
BTEN_INTERFACE=gpio91

# uart interface & baudrate between host platform and module
UART_INTERFACE=/dev/ttyHS1
UART_BAUDRATE=1500000
```
To enable Bluetooth, the daemon toggles the enable pin via `sysfs`:
```bash
echo 91 > /sys/class/gpio/export
echo out > /sys/class/gpio/gpio91/direction
echo 1 > /sys/class/gpio/gpio91/value
```

---

## 5. Application-side Bluetooth protocol: `libBlueTooth.so` (2026-07-16)

**Why:** section 4 documents the `/usr/bin/blueware` daemon's own
config; this section covers how `MsnCoreApp`'s Qt UI (`libBlueTooth.so`,
`usr/lib/libBlueTooth.so`, 450 KB ARM32, **stripped** — recovered via
mangled C++ names still present in `.dynsym`/`.dynstr`, so
`arm-linux-gnueabihf-objdump -d` + `c++filt` were enough without a
`.symtab`) actually talks to that daemon, and what the wire commands
look like. Unlike `input.so` this is genuine vendor C++/Qt code (classes
`BtMainWindow`, `BtSettingPanel`, `BtDialPanel`, `BtContactPanel`,
`BtMusicPanel`, `BtHistoryPanel`, `CallingDialog`, `ContactDialog`, plus
the panel adapter classes below), not a stock open-source library.

### Adapter class hierarchy — confirmed active variant

`BlueToothAdapter` is an abstract base with three concrete
implementations, each with an identical 67-method vtable shape:
`BlueToothAdapter_SD851`, `BlueToothAdapter_HD6956`,
`BlueToothAdapter_Blueware`. A singleton factory,
`BlueToothAdapter::getBlueToothAdapter(BlueToothAdapterType)` (`0x360a0`),
switches on the enum via a bitmask test (`1 << (type-2)`):

| Enum value(s) | Mask bit(s) | Class instantiated |
|---|---|---|
| 2, 3, 4, 7 | `0x27` | `BlueToothAdapter_SD851` |
| 5, 6 | `0x18` | `BlueToothAdapter_Blueware` |
| 8 | `0x40` | `BlueToothAdapter_HD6956` |

`.rodata` ties types 5/6 directly to the two config variants seen in
section 4: `blueware /etc/blueware-bw121.properties &` and
`blueware /etc/blueware-bw123.properties &` — i.e. **this board's
config (`bw121`) selects `BlueToothAdapter_Blueware`**, the class
documented below. `SD851`/`HD6956` are alternate chip profiles for other
product variants sharing this same binary; not exercised on this unit.

### Transport chain — not a direct UART open

`libBlueTooth.so` does **not** open `/dev/ttyHS1` itself. Startup
strings in `BlueToothAdapter_Blueware::onStartupConfig()` (`0x472ec`)
reveal the real chain:

```
mkdir /dev/socket/ && ln -s /dev/bw_iap /dev/socket/goc_rfcom
```

- `/usr/bin/blueware` (the daemon from section 4) owns the physical
  `/dev/ttyHS1` UART to the RTL8762BTV module and exposes two virtual
  nodes: `/dev/bw_serial` (AT-command channel) and `/dev/bw_iap`
  (raw RFCOMM/SPP passthrough, symlinked to `/dev/socket/goc_rfcom` for
  phone-side accessory protocols — an `"AppleInc."` string and
  `getPhoneTypeFromUUID(QString&)` nearby suggest this is used for
  Apple iAP as well as generic SPP).
- `BlueToothAdapter_Blueware` opens `/dev/bw_serial` via a
  `MsnSerialPort` wrapper (`isOpen()`/`write()`) and speaks a plain
  line-based **AT command / `+PREFIX=` response** protocol over it —
  the same style already known from the MCU-side AT strings documented
  in `docs/UI_AND_APP_ANALYSIS.md:481`, just now confirmed from the app
  side too.

### Who actually launches `/usr/bin/blueware`, and a diagnostic dead-end this creates (2026-07-17)

Neither `rcS`, `/etc/profile`, nor `inittab` in either rootfs (Prado
reconstructed or CSTech) ever invokes `blueware` — grepped both trees
for the literal string, zero hits outside `libBlueTooth.so` itself and
its two `.properties` config files. It's launched entirely from app
code: `BlueToothAdapter_Blueware::initBlueToothAdapter()` (`0x47728`,
**not** `onStartupConfig()` — that one only runs the `mkdir`/`ln -s`
`/dev/socket/goc_rfcom` setup shown above) contains two `system()`
calls, gated by a flag byte at `object+31` presumably selecting
board/config variant:

```
0x47824: system("blueware /etc/blueware-bw121.properties > /dev/null 2>&1 &")
0x47a04: system("blueware /etc/blueware-bw123.properties > /dev/null 2>&1 &")
```

(Both string addresses hand-computed and confirmed from the PC-relative
literal loads feeding each `system@plt` call — `0x47828 + 0x171e8 =
0x5ea10` and `0x47a08 + 0x17070 = 0x5ea78`, matching the `.rodata`
strings exactly.) This board's `bw121` config (section 4) means the
first of these two is the one that actually runs here.

**The `> /dev/null 2>&1` matters a lot.** `/usr/bin/blueware` itself
turns out to be far better instrumented than the `libMsnCommons.so`
GPIO helper documented for the BD37033's GPIO34 enable line
(`docs/BD37033.md` §2) — `strings`/`.rodata` on `blueware` show a
generic `bpio_init`-prefixed GPIO helper with a distinct, specific
error message for *every* step: `"bpio_init open(%s) for export
failed: %s (%d)"`, `"...for dir failed..."`, `"...for val
failed..."`, plus matching `"...success: fd (%d)"` messages — i.e. if
the `BTEN_INTERFACE=gpio91` sequence (`export`/`direction`/`value`,
section 4) fails at any step, `blueware` **does** print a clear,
specific, `errno`-annotated message about it. But because
`initBlueToothAdapter()` launches it with `> /dev/null 2>&1`, none of
that ever reaches anywhere visible — not a log file, not dmesg, nothing.
If GPIO91 is failing (pinmux, permissions, a sysfs export race, or
anything else in that same family of issues we already confirmed for
GPIO34), there is currently **no way to observe it** short of
intercepting the daemon's own output directly.

**Concrete on-device test, no code changes needed:**
```
/ # killall blueware 2>/dev/null
/ # /usr/bin/blueware /etc/blueware-bw121.properties    # no redirect -- watch stdout live
```
Also worth checking directly, same pattern as the GPIO34 checks in
`docs/BD37033.md`:
```
/ # ls /sys/class/gpio/ | grep gpio91
/ # cat /sys/class/gpio/gpio91/direction
/ # cat /sys/class/gpio/gpio91/value
```
If `blueware` run this way prints one of the `bpio_init ... failed`
lines, that's the root cause, directly confirmed rather than inferred.
If it starts cleanly and `gpio91` reads back `out`/`1` as expected, the
GPIO path is very likely fine and the "no response" issue is downstream
of it — the `/dev/bw_serial` AT-command layer or the RTL8762BTV module
itself.

### `writeCommand(const QString&)` (`0x45304`) — outgoing frame format

Every outgoing command is built as the literal template `"AT+%1\r\n"`
(recovered from `.rodata @ 0x5e7e8`) with the caller-supplied token
substituted for `%1`, then written to `/dev/bw_serial` via
`MsnSerialPort::write()` (only if `isOpen()` first). No further framing,
checksum, or length prefix — a bare newline-terminated AT-command line.

### Recovered command/response vocabulary (from `.rodata`, `0x5e7e0`–`0x5f200`)

Outgoing (`AT+<token>` sent by `libBlueTooth.so`):

```
DEVSTAT          ADDR             REBOOT           HFPSTAT
A2DPSTAT         HFPDIAL=<num>    HFPANSW          HFPCHUP
HFPMCAL=0/1/2    HFPDTMF          HFPDISC          A2DPDISC
BTEN=0 / BTEN=1  MICMUTE=%1       A2DPMUTE=%1      HFPADTS=1/2
PBDOWN=1/2/3/4   PBABORT          PLAYPAUSE        PLAY
STOP             FORWARD          BACKWARD         PLIST / PLIST=0
HFPCONN / HFPCONN=%1              VER              NAME=%1
PIN=%1           SCAN=1           HFPCFG
```

Incoming (`+PREFIX=` lines parsed by `onReadLine(QByteArray)` — 8.8 KB,
not fully hand-disassembled; prefixes below recovered from string
cross-references, exact field grammar not yet decoded):

```
+DEVSTAT=   +HFPSTAT=   +HFPDEV=    +HFPAUDIO=0 / +HFPAUDIO=1
+ADDR=      +PLIST=     +TRACKINFO= +A2DPSTAT=
+VER=       +PBDATA=  / +PBDATA=E   +HFPCFG (also seen as HFPCFG=0)
+NAME=      +PIN=       +PAIRED=0   +HFPMANU=
```

### Qt signal/slot surface (from the moc metaobject string table)

`BlueToothAdapter`'s public signals cover: `bluetoothEnableChange(bool)`,
`onConnected(QString)` / `onDisconnected()`,
`onHFPStatusChange(HFPStatus)` / `onA2DPStatusChange(A2DPStatus)`,
`onRecvContacts(QStringList,bool)` / `onRecvCallHistorys(QStringList,bool)`,
`onOutgoingCall`/`onIncomingCall`/`onTalkingCall`/`onNewIncomingCall(QString)`,
`onRecvUUID(QString)`, and `contactsDataChange()` /
`callHistoryDataChange_{Incoming,Outgoing,Missed}()` /
`favoriteDataChange()`. This is the internal API surface the `Bt*Panel`
UI classes (`BtMainWindow`, `BtDialPanel`, `BtContactPanel`,
`BtMusicPanel`, `BtHistoryPanel`) consume — useful map if a future task
needs to trace a specific UI misbehavior (e.g. contacts not syncing)
back to a specific AT command/response pair above.

**Not yet decompiled:** the 8.8 KB `onReadLine` bodies (all three
adapter classes) that parse the `+PREFIX=` lines field-by-field, and
`libBTSender.so`/`libbt_stack.so` (the latter is almost certainly a
stock Bluedroid/BlueZ HCI stack judging by its size and generic name,
not vendor code — lower priority for hand disassembly).

---

## 6. Root cause found and fixed (2026-07-18): missing RTL8761B firmware, not a module-type mismatch

Section 4/5 above left Bluetooth in a broken state: `blueware` would
crash-loop on `bt_fw_download_thread` → `firmware_config_cb:1` →
`Restart_MainThread` → repeat, seconds after boot, every single time.
The investigation went through several wrong turns before landing on
the real cause — recorded here so the mistakes aren't repeated.

### Dead end #1: switching `MsnProductInfo.ini`'s `BlueToothType` from 6 to 5

`blueware-bw121.properties` (`MODULE_TYPE=BW121`) was crash-looping, so
the working theory became "wrong module type" — the real device's About
screen shows `BlueTooth: BT825B,V5.6.6`, and `blueware-bw123.properties`
(`MODULE_TYPE=BT825`) matched that string, so `BlueToothType` was
changed `6→5` to select it. This did change `blueware`'s code path
(`bt_chipset_rtl_init`/H5 transport instead of the old vendor-command
path) but the *new* failure was worse: a live H5 `>>SYNC` handshake to
the chip timed out completely (`h5_link_synchronize_timeout` →
`BLUEWARE_HCI_TRANSPORT_PEER_RESET`), and a separate crash (`blueware`
segfaulting on a wild pointer inside its own response parser, same
fault address `0xfcee0112` recurring across both module types) kept
happening regardless of which one was active — proving that crash was
unrelated to module-type selection.

### Dead end #2 (partial): the segfault, and ruling out `ARK1668_LCDC_OSD1_CTL`/baud-rate theories

Disassembled the crash site (`0x5d47c: ldrh r3,[r0,#14]`, `r0` a
garbage pointer from a lookup at `0x30810`) — a parser indexing into
something with unvalidated data, consistent with a chip responding in a
format the active module profile's parser doesn't expect. Separately
noticed the UART never reached its configured `1500000` baud (stuck at
`115200`, matching `ark_hsuart_set_termios` log lines) — checked the
kernel driver and DT clock (`uart5clk` = 24MHz, `uartclk/16` = exactly
1,500,000, no kernel-side cap) and concluded the kernel/DTS side was
fully capable of 1.5M baud; `blueware` itself simply never requests the
speed bump because its own initial handshake never completes. Neither
of these avenues was the actual root cause — both were downstream
symptoms of the real problem below.

### Root cause: `BlueToothType=6` (BW121) was correct all along — three real files were just missing from the rootfs

A **live capture on genuine stock firmware** (`docs/logs/bluetooth log
stock.txt`) with `MODULE_TYPE=BW121` — the *original*, never-changed
setting — succeeded completely: full HCI bring-up, `firmware_config_cb:0`
(success, not the `:1` failure seen throughout this investigation),
ending in a broadcasting radio: `[BLUEWARE:ON][NAME:Limcet
Box_fc9f][MAC:DC0D3014FC9F][PROFILES:246952]`. The `/etc/` listing from
that same live session showed one file our reconstructed rootfs never
had: **`rtl8761bt_fw`**. `blueware`'s own log explained why it matters —
`openning librtkvnd.so` → `set:fwdir=/etc/` → firmware loads →
`firmware_config_cb:0`.

**The RTL8761B is a ROM+RAM chip, not a chip with persistent onboard
firmware storage.** Its silicon boot ROM only knows how to listen on
UART for a firmware patch and load it into volatile SRAM — every power
cycle (which includes every `GPIO91`/`BTEN` toggle, i.e. every boot and
every `blueware` restart) wipes that RAM, and the chip needs the patch
re-pushed from the host before it can do anything Bluetooth-related.
Without the firmware file present, *every* boot was guaranteed to fail
the exact same way, regardless of `MODULE_TYPE`/`BlueToothType`
selection — which is exactly what both dead ends above were actually
observing without realizing it.

**Files that were missing, found in `Holden firmware update/rootfs.img`
(a UBI image never previously extracted — `ubireader_extract_files`
pulled it apart), MD5-verified, and copied into
`firmware_source/prado_reconstructed/mtd6_rootfs/rootfs/`:**

| File | Size | Why it's needed |
|---|---|---|
| `etc/rtl8761bt_fw` | 43,980 bytes | The actual firmware/patch blob pushed into the chip's SRAM on every boot |
| `usr/lib/librtkvnd.so` | 153,496 bytes | The Realtek vendor library `blueware` `dlopen()`s to actually perform the download (`openning librtkvnd.so` in the log) — without this, the firmware file alone wouldn't have been enough |
| `etc/rtkbt.conf` | 799 bytes | Config file at the exact path `blueware`'s own log referenced (`cfgfile=/data/feasycom/rtkbt.conf`, seeded from this) — distinct from the *other*, already-present `etc/bluetooth/rtkbt.conf`, which belongs to the unrelated `libbt-vendor.so`/generic-Android-rtkbt stack, not `blueware` |
| `etc/RingTone.wav`, `etc/bluetooth/RingTone.wav` | 418,380 bytes each | HFP ringtone asset — `blueware-bw121.properties`'s `HFP_RING_PATH=/etc/RingTone.wav` already pointed at this (that property value was already correct in our tree, matching live stock, not the raw Holden archive's own empty default), just the file itself was missing |

All five committed in `ec03977`, "Add missing RTL8761B Bluetooth
firmware/vendor library from Holden rootfs".

**Status: fixed in the repo, not yet re-confirmed on our own
reconstructed rootfs on real hardware** (the successful capture above
was on genuine stock firmware, used to *diagnose* the gap — the fix
itself is a same-rootfs file addition, no kernel/DTS change, so it
should carry over directly, but hasn't had its own dedicated boot-log
confirmation yet). Next real-hardware session should confirm `blueware`
reaches `firmware_config_cb:0` and a broadcasting radio on our own
image, not just stock's.

### `mute`/`gpio` startup line — unrelated to this fix, tracked separately

`MsnCoreApp`'s own boot log line `Sound_BD37033::muteSpeakerAtts false
true` (seen the same session) is BD37033 sound-amp related, not
Bluetooth — see `docs/BD37033.md` and `docs/AUDIO_SUBSYSTEM_INVESTIGATION.md`,
not this document.
## 7. `usb0` `dr_mode="host"` attempt, reverted (2026-07-19)

**The problem this was trying to fix:** every single boot log this
whole project has shown `usb0` (`musb-hdrc.0`, gpio-id `76`/gpio-pwr
`126`) spending several seconds retrying at boot before the USB stick
enumerates:
```
[    5.097638] usb usb1-port1: Cannot enable. Maybe the USB cable is bad?
[    8.157583] musb-hdrc musb-hdrc.0: musb_reset_timer_handler: reset timer fired
[    8.164842] musb-hdrc musb-hdrc.0: musb_recovery_usb_proc: reset otg
[    8.171246] musb-hdrc musb-hdrc.0: +Switch peripheral 76  126===
[    9.197605] musb-hdrc musb-hdrc.0: +++Switch OTG 76  126===+++
[    9.524225] usb usb1-port1: attempt power cycle
```
(repeats 2-3x, eventually enumerating around 14s). Confirmed via
`drivers/usb/musb/musb_ark.c` that `"+Switch peripheral"`/`"+++Switch
OTG+++"` are the exact `dev_info()` strings printed by this driver's
own `ark_musb_set_mode()` — the cycling is literally the kernel's
automatic ID-pin OTG role negotiation calling this function
repeatedly, toggling the ID-pin GPIO back and forth until it settles.
`usb1` (`musb-hdrc.1`, gpio-id `1`/gpio-pwr `117`) showed a single
clean `+++Switch OTG+++` line with none of this in every log checked —
seemed to confirm the cycling was specific to whatever's plugged into
`usb0`, not a generic OTG startup cost.

### The fix tried, and why it looked right

`usb0` is this board's single external-facing USB port — the user
confirmed it's genuinely dual-use: USB-boot/storage-stick testing
(`root=/dev/sda*`) and wired CarPlay's gadget mode both use the same
physical port. `ark_musb_set_mode()`'s `MUSB_HOST` case is a total
no-op (just `break;`, no GPIO/register toggling at all), so setting
`dr_mode="host"` in the DTS should skip the ID-pin negotiation
entirely for the boot-critical path, while OTG capability gets
restored afterward via the driver's own runtime-switchable sysfs
`mode` attribute (`musb_core.c`'s `DEVICE_ATTR_RW(mode)`, which calls
this same `ark_musb_set_mode()`).

Implemented as two paired changes:
- `linux-arkmicro` `83ab185e6` — `usb0`'s DTS `dr_mode` changed
  `otg` → `host`.
- `firmware_overlay/prado/etc/switchotg.sh` (main repo `9aa0fec`) —
  new overlay override, fixing two real bugs in the stock script
  (wrong sysfs paths — `/sys/devices/platform/musb-ark1680.N/...`
  never existed on this kernel, real path is
  `e0100000.usb`/`e0400000.usb`; and no safety check before switching
  `usb0`, added one that skips the switch whenever `root=/dev/sda*` is
  the live root filesystem, since switching that port away from host
  mode while it's serving as the running system's own root would yank
  root out from under it).

### What actually happened on hardware — reverted

`usb0` itself registered **perfectly cleanly** with `dr_mode="host"`
— no negotiation messages at all, immediate:
```
[    0.787373] musb-hdrc musb-hdrc.0: MUSB HDRC host driver
[    0.792755] musb-hdrc musb-hdrc.0: new USB bus registered, assigned bus number 1
[    0.801402] hub 1-0:1.0: USB hub found
[    0.805309] hub 1-0:1.0: 1 port detected
```
No sign of a problem there at all. But the physical USB boot stick —
confirmed by the user to be on the exact same physical port every
single time this project has tested it — **stopped enumerating on
`usb0`/bus 1** (where every prior log without exception had shown it)
and instead appeared on **`usb1`/bus 2** several seconds later, going
through the identical `"Cannot enable... attempt power cycle"` cycling
`usb0` used to have:
```
[    5.717264] usb usb2-port1: Cannot enable. Maybe the USB cable is bad?
[    8.797225] musb-hdrc musb-hdrc.1: musb_reset_timer_handler: reset timer fired
...
[   14.157224] musb-hdrc musb-hdrc.1: +++Switch OTG 1  117===+++
[   14.747258] usb usb2-port1: Cannot enable. Maybe the USB cable is bad?
[   15.117232] usb 2-1: new high-speed USB device number 5 using musb-hdrc
...
[   17.757210] musb-hdrc musb-hdrc.1: musb_reset_timer_handler: reset timer fired
[   17.764488] musb-hdrc musb-hdrc.1: musb_recovery_usb_proc: port already enabled, skipping disruptive VBUS reset
```
This last line is where it ended — no further enumeration attempt, no
success line, boot hung. Worse than the original problem: instead of
a bounded ~14s delay before things worked, this run never recovered.

**Also notable, though not the reason for reverting:** `usb1`'s own
boot-time negotiation (separate from the later cycling above) settles
into a state that the `g_ncm` gadget driver binds to successfully
right afterward:
```
[    0.864685] musb-hdrc musb-hdrc.1: +++Switch OTG 1  117===+++
...
[    1.200790] g_ncm gadget: NCM Gadget
[    1.204372] g_ncm gadget: g_ncm ready
```
This suggests `usb1`, not `usb0`, may be the controller actually
providing UDC/gadget capability for CarPlay's wired networking
interface (itself confusingly *also* named `usb0` at the network-
interface level — a naming coincidence with the DTS node label, not
the same thing; see `mode_show()`/`ifconfig usb0 up` in
`switchotg.sh`). Not confirmed, and not what caused the revert, but
worth keeping in mind for any future attempt at this — the physical
port ↔ DTS node ↔ real-world-role mapping this whole investigation
has assumed (`usb0` = shared storage+CarPlay port) may not be as
clean as it looked.

### Reverted, not re-investigated

Reverted in `linux-arkmicro` `d44cce385` — both `usb0` and `usb1` back
to `dr_mode="otg"`, the exact state proven working (with the ~14s
delay) across every prior boot log. **Root cause of the regression is
not understood.** `usb0`'s own registration gave zero indication
anything was wrong when `dr_mode="host"` was set, so whatever's
actually happening — bus/device renumbering tied to `usb0`'s probe
timing changing? some shared hardware dependency between the two
controllers (a shared power rail, clock enable, or PHY reference this
DTS doesn't capture) sequenced as a side effect of `usb0`'s own OTG
negotiation, which no longer runs? — needs real investigation before
trying this again, not just flipping the DTS value back and forth.

`firmware_overlay/prado/etc/switchotg.sh`'s fix (correct sysfs paths,
root-mounted-on-`usb0` safety guard) was **not** reverted — both parts
are still independently valid regardless of `usb0`'s boot-time
`dr_mode`, since `ark_musb_set_mode()` does an unconditional GPIO/
register reset even when the requested mode matches the port's
current state, so the guard remains real protection against calling
it while root is actively mounted from that port.

### Re-attempted 2026-07-22 with diagnostic logging — root cause found

Re-enabled `usb0` `dr_mode="host"` (`linux-arkmicro` `9fa92d4b1`) with
`ark_musb_dump_cross_port_state()` added to `musb_ark.c`: logs both
ports' soft-reset registers (`0x74`/`0x78`) and both ports' id/pwr
GPIOs at init and at every `set_mode` entry/exit, to check whether the
2026-07-19 regression was cross-port register/GPIO coupling. It
wasn't — usb0's registers and GPIOs (`softrest0=0xffffffff`,
`usb0_id=0`, `usb0_pwr=1`) never moved once, for the entire boot,
regardless of what usb1 was doing (see `docs/logs/usb otg host log
v1.txt`).

The real cause, found from the same log:
- U-Boot's own boot line confirms the stick is present and working on
  `usb 0:1`, found in 172ms (`usb_lowlevel_init time is 172430 us`,
  `1 Storage Device(s) found`) — so the device is definitely
  physically on `usb0`, powered continuously through the U-Boot→kernel
  transition.
- Under Linux with `dr_mode="host"`, bus 1 (`usb0`) registers its hub
  cleanly (`hub 1-0:1.0: 1 port detected`) but **the device never
  enumerates on it at all**, for the rest of the log. `Waiting for
  root device /dev/sda2...` never resolves.
- `ark_musb_diag` confirms `ark_musb_set_mode(MUSB_HOST)` **is** called
  at boot for `usb0` — so the bug isn't that host-mode ports skip this
  callback, it's that the callback's old body (`case MUSB_HOST:
  break;`) did nothing at all, unlike `MUSB_OTG`'s case which ends
  with an explicit VBUS power-cycle (`gpio_pwr` low → high).
- That power-cycle isn't just for OTG role switching: it's what forces
  the hub controller to see a fresh disconnect/reconnect edge. A
  device that's been continuously connected since before the kernel
  booted never generates that edge on its own, so in host mode it was
  simply invisible to the hub forever.
- The activity on bus 2 (`usb1`) in that log — the extra
  "Cannot enable... attempt power cycle" retries, worse than usb1's
  usual single clean `+++Switch OTG+++` line — is a **separate, still
  not fully understood** effect on usb1's own (likely WiFi module)
  device, not the boot stick moving buses. Given usb0's registers/
  GPIOs never moved, whatever's affecting usb1's retry count isn't
  captured by this diagnostic (maybe a shared VBUS rail, PHY
  reference, or interrupt-timing effect) — worth another look if the
  fix below doesn't fully resolve boot behavior.

**Fix applied** (`linux-arkmicro` `d56d5a750`): `MUSB_HOST`'s case now
does the same controller soft-reset + VBUS power-cycle sequence as
`MUSB_OTG`, minus the ID-pin GPIO/OTG-state changes that only matter
for role negotiation. This should keep the benefit `dr_mode="host"`
was meant to provide (no ID-pin negotiation delay on `usb0`) while
still generating the connect event the already-attached device needs.

### Confirmed on hardware 2026-07-22 — fix works, boot time fixed

`docs/logs/usb otg host log v1.txt` (first hardware test of the fix
above) confirmed the enumeration problem was solved: `usb0`/bus 1
still never showed a device, but that ruled out one thing and
surfaced another — see below. The real confirmation came a few
iterations later, in `docs/logs/usb otg host log v4.txt`:

- `usb0` (`switch host (no negotiation)`) registers cleanly with no
  retry cycling, and the boot stick enumerates on bus 1 within
  ~2 seconds every time.
- Root mounts at **t≈3.4s** and `/sbin/init` starts immediately after
  — down from the ~14s ID-pin negotiation dance in the old
  `dr_mode="otg"` baseline (or indefinite hang in the first, reverted
  `dr_mode="host"` attempt from 2026-07-19).
- This is the actual fix for the original complaint ("usb boot takes
  several seconds to find the drive and toggle the mode").

### Dead end along the way: usb1 diagnostic test, and the real culprit

Between the `usb0` fix landing and it being confirmed working, a
separate, unrelated bug produced a very convincing false lead. After
flashing the `usb0`-fixed kernel, boot got as far as `Run /sbin/init as
init process` and then looped forever on garbled `can't run '/bin/...'`
console messages, never reaching a shell. Because the messages were
first seen interleaved with usb1's own OTG retry-cycling messages on
the same serial console, it initially looked like a USB/kernel
regression. It wasn't. In order, what was actually ruled out and found:

1. **Not a missing-symlink issue.** `build_tools/restore_rootfs_symlinks.sh`
   (which recreates `/bin/sh` etc. lost by a Windows-side checkout) had
   already run cleanly against the affected build (`1131 created/fixed,
   0 dangling`), and the physical stick's files were confirmed present
   when mounted directly.
2. **Not a VBUS-disconnect-corrupts-root issue**, despite
   `musb_recovery_usb_proc()` (`musb_core.c:2550-2598`) having a
   documented comment about exactly that failure mode from a past
   incident. Ruled out on timing: the first `can't run` failure
   happened immediately after `/sbin/init` started (t≈3.49s), 2.7+
   seconds *before* usb1's first reset-timer event in the log (t≈6.27s)
   — and there were no `EXT4-fs error`/`remount-ro` messages anywhere.
3. **A diagnostic build with usb1 also set to `dr_mode="host"`**
   (`linux-arkmicro` `9f678777e`, eliminating *all* OTG negotiation
   from the boot) was tried to rule out OTG activity entirely as a
   cause. This came with a known, accepted regression while in place:
   `musb_core.c`'s init switch only calls `musb_gadget_setup()` for
   `MUSB_PERIPHERAL`/`MUSB_OTG`, never for pure `MUSB_HOST`, so usb1's
   `g_ncm` gadget didn't register during this test. The failure still
   reproduced identically with zero OTG negotiation anywhere in the
   boot, which ruled out OTG entirely as the cause.
4. **Real root cause, found once the console noise from (3) stopped
   interleaving the message**: with usb1 quiet, the previously-garbled
   message resolved to `can't run '/bin/rc.d/rcS'` — not
   `/etc/rc.d/rcS` as `inittab` actually specifies. Checking the
   physical stick directly (`cat -A /etc/inittab` on the mounted
   partition) showed the `sysinit` line still had a trailing `\r`
   (`::sysinit:/etc/rc.d/rcS\r\n`). BusyBox's inittab parser takes
   everything after the last `:` up to the line terminator as the
   command, so the actual exec target was `/etc/rc.d/rcS\r` — invalid,
   `ENOENT`, and the embedded `\r` mid-message is what garbled the
   printed error into what looked like `/bin/...` when raw serial bytes
   got reinterpreted.
5. **Why the stray `\r` was there**: `build_bootable_sdcard.sh` strips
   CRLF at the end of step 8 (`sed 's/\r$//'` across `/tmp/sd_p2`), but
   `apply_overlay()` — which rsyncs `firmware_overlay/` on top —
   doesn't run until step 11, *after* that cleanup. This repo's working
   tree can carry CRLF in text files independent of git history (e.g.
   `firmware_overlay/etc/inittab` has clean LF at `HEAD`, confirmed via
   `git show`, but picks up CRLF on-disk in this environment — this
   repo lives on a Windows-fed shared folder, and the project already
   documents Windows-checkout artifacts affecting symlinks the same
   way). So a CRLF-carrying overlay file placed *after* step 8's
   cleanup silently reintroduces the exact bug step 8 already fixed.
   **Fixed** in `build_bootable_sdcard.sh` (`8d609a5`): the same CRLF
   strip now also runs at the end of step 11, after `apply_overlay` and
   its rcS/autolaunch patches have all applied. The already-affected
   stick was also fixed directly (`sed -i 's/\r$//'` on its `inittab`
   and `profile`).

None of this was actually caused by the `usb0` fix — it was a
pre-existing build-pipeline gap that this investigation happened to
trip over while testing on a freshly-reflashed stick.

### usb1 stays `dr_mode="otg"` — it's genuinely dual-role, not a fixed device

`usb1` is CarPlay's port, and it's dynamic by design, not a single
fixed internal device: at boot it tries peripheral role first
(`g_ncm` gadget registers almost immediately — this is the wired
CarPlay networking path, confirmed to be bound to `musb-hdrc.1` in
`docs/logs/usb otg host log v4.txt`, not `usb0`), and only falls back
to host role — enumerating the internal WiFi chip (`rtl8821cu`,
`usb 2-1: new high-speed USB device`) and bringing up `wlan0`/the
`carplay_wifi` AP — once that peripheral-role attempt times out with
nothing on the other end. This is real, necessary hardware detection
(is a wired CarPlay cable actually present right now?), not wasted
work, so it can't be replaced with a static `dr_mode` the way `usb0`
could — `usb0` is genuinely single-purpose (boot-stick storage), which
is why locking its mode was safe, while doing the same to `usb1` would
permanently disable whichever of {wired CarPlay, wireless
WiFi-fallback} didn't match the hardcoded mode.

A same-day test that briefly set `usb1` to `dr_mode="host"` too
(`9f678777e`, step 3 above) confirmed this concretely: `g_ncm` simply
didn't register for the duration of that test. Reverted back to `otg`
in `linux-arkmicro` `17a80acff` once the CRLF bug (not USB) was
confirmed as the actual boot-loop cause.

The ~9-18s retry cycling this produces on `usb1` (`Cannot enable...`,
`attempt power cycle`, `reset timer fired`) is expected and doesn't
block anything — the shell is already available (~t=6s, well before
this cycling even starts) since it's no longer sharing a path with the
boot-critical `usb0`. What *was* fixable: `musb_recovery_usb_proc()`,
`musb_reset_timer_handler()`, and `musb_irq_work()` (`musb_core.c`)
logged this routine, expected cycle at `dev_alert` — the loudest
kernel log level, normally reserved for real faults. Downgraded to
`dev_dbg` in `linux-arkmicro` `d6be430dd` (pure log verbosity change,
no timing/retry logic touched). The three cosmetic `dev_info()`
messages in `ark_musb_set_mode()` (`musb_ark.c`, the vendor's original
`"+Switch peripheral ===" `/`"+++Switch OTG===+++"` decoration) were
also reworded to plain text in `linux-arkmicro` `17a80acff`.
Left `drivers/usb/core/hub.c`'s own `"Cannot enable"`/`"attempt power
cycle"` messages untouched — that's generic upstream USB core code
used by every port on the system, not specific to this quirk.

Also note: the underlying OTG *role-detection* mechanism on `usb1`
(polling every few seconds via `ark_musb_otg_timer`, in `musb_ark.c`)
can't be made purely interrupt/plug-driven regardless of the above —
there's vendor commentary already in the driver (`musb_ark.c:139-141,
168`, `musb_core.c` DRVVBUS comment) explaining this ArkMicro SoC has
no ID-pin-change interrupt at all, so polling is the only way this
hardware can detect an OTG role change. That's a real hardware
limitation, not a driver design choice, and applies regardless of
which physical device is on the other end.

### Important correction: `usb0` and CarPlay's wired port are the *same physical connector*

Everything above (this section) was written assuming `usb0`'s
external-facing connector was purely a test/storage-boot port,
independent of CarPlay. That assumption was wrong, confirmed directly
by the user: **the port you plug a USB storage stick into to test
`bootusb` is the exact same physical connector CarPlay uses when
wired.** That means the `dr_mode="host"` fix above has a real cost
beyond "slower test boots": since `musb_gadget_setup()` never runs for
a `dr_mode="host"` port, **wired CarPlay can never work through this
connector again while this fix is in place** — not just during
testing, in the actual running vehicle too.

Decision made 2026-07-22 (confirmed with the user, not a unilateral
call): **keep `dr_mode="host"` on `usb0` anyway.** Reasoning: a USB
storage stick and a CarPlay cable can never physically occupy the same
connector at the same time regardless, so there's no scenario where
both are needed simultaneously — the real question was just "which
default do we want for this shared connector," and fast boot-stick
testing won concretely, while wired CarPlay's importance relative to
the wireless-fallback path (see below) wasn't treated as blocking.
**If wired CarPlay through this specific connector is needed later,
this decision needs revisiting** — it would require either reverting
`usb0` to `otg` (accepting the ~14s boot delay again) or some other
mechanism (e.g. a separate DTB selected by U-Boot depending on boot
mode) to get both.

**Revisited 2026-07-27 — see "`usb0` dr_mode made boot-command-dependent" below.** Wired CarPlay testing needed this connector back, so this tradeoff is no longer in effect: `dr_mode` is now decided per boot command instead of one hardcoded DTS value.

### `g_ncm`'s net device renamed to avoid a real naming collision

While reconciling the above, a second, unrelated point of confusion
surfaced: the `g_ncm` gadget's network interface was named `usb0` by
`gether_setup_default()`'s generic `"usb%d"` convention — byte-identical
text to the DTS node label `usb0` (a physical MUSB controller
instance) in kernel log output, despite being two completely unrelated
things (one's a net device, one's a controller instance; the working
one turned out to be bound to `usb1`/`musb-hdrc.1`, not `usb0`). This
collision directly caused a misreading during this same investigation.
Renamed in `linux-arkmicro` `69c59f33a`: `f_ncm.c` now calls
`gether_setup_name_default("carplay-ncm")`, producing `carplay-ncm0`.
Userspace references (`ifconfig usb0 up` in `switchotg.sh` and two
commented-out `rcS` lines) updated to match
(`prado-firmware-reconstruction` `d1c2ebc`).

### `usb1` forced to host mode on USB-stick test boots (2026-07-22)

Given the connector confusion above, and that `usb1`'s CarPlay
wired-then-WiFi-fallback negotiation costs up to ~15s (see the retry
cycling documented earlier in this section), a further optimization:
when `root=/dev/sda*` (i.e. this is a USB-stick test/dev boot, not the
vehicle actually running), we already know for certain no wired
CarPlay cable is in the picture for that session. `switchotg.sh`
(`prado-firmware-reconstruction` `9f2ef8d`) now detects this and writes
`host` directly to `usb1`'s sysfs `mode` attribute, skipping the wired
negotiation entirely so `wlan0` comes up immediately. Any other boot
(real vehicle deployment, `root` elsewhere) keeps the full dynamic
`otg` negotiation, since a wired cable might genuinely be present
there.

This needed one more kernel-side fix to actually stick:
`mode_store()`'s sysfs write handler (`musb_core.c`) only calls
`musb_platform_set_mode()` — it doesn't touch `musb->port_mode` or
disarm `ark_musb_otg_timer`'s periodic poll. Since `usb1`'s
`port_mode` stays `MUSB_OTG` (set once from the DTS at init, sysfs
writes don't change it), that poll timer stays armed regardless, and
its `OTG_STATE_B_IDLE` branch would try to renegotiate the role right
back on its next tick — unless the OTG state machine no longer reads
as `B_IDLE`. `MUSB_HOST`'s case in `ark_musb_set_mode()`
(`musb_ark.c`) didn't set `otg->state` at all (unlike `MUSB_OTG`'s
case, which sets `OTG_STATE_A_HOST`) — harmless for `usb0`'s
boot-time-only use (`is_otg_enabled()` is false there, so the timer
never arms in the first place), but would have silently undone this
new runtime switch on `usb1`. Fixed in `linux-arkmicro` `a2a8daac3`:
`MUSB_HOST` now sets `OTG_STATE_A_HOST` too.

### `switchotg.sh` had no caller at all — none of this ever ran until now

`docs/logs/usb otg host log v5.txt` (first hardware test after all of
the above) surfaced one more gap: `carplay-ncm0` (the kernel-side
rename) showed up fine, confirming that fix was in the build — but
none of `switchotg.sh`'s own log messages ever appeared, and `usb1`
still ran the full ~15s wired-then-WiFi negotiation regardless of
`root=/dev/sda*`. Checking `rcS`: `switchotg.sh` was only ever
referenced by the stock `etc/all.sh`, which loads `g_ncm` as a 3.4.0
kernel module (`insmod`) — inapplicable to this 4.19.192 build, where
NCM is built in. `rcS` itself never called it. **Every `switchotg.sh`
change made this session (2026-07-19 through 2026-07-22, including
today's usb0/usb1 rewrite and the new host-mode-on-test-boot logic)
had never actually executed on real hardware until this was found and
fixed.**

Fixed in `prado-firmware-reconstruction` `c1515fe`: added the call
right after `mdev -s` in `rcS` (needs `/proc`/`/sys` mounted, nothing
else) — as early as possible, since the kernel's own OTG negotiation
starts automatically at driver init, well before `rcS` runs, so every
second earlier this can override it matters. Also removed the old
commented-out carplay setup block's now-fully-redundant lines (it had
sat there dead, `ifconfig`/`hostname`/sysfs-mode-switch, including the
same already-wrong sysfs path `switchotg.sh` was written to fix, since
before this investigation started).

### Second gotcha: the sysfs path itself was also wrong

With the caller finally wired in, the next boot log showed
`switchotg.sh` running and correctly detecting `root=/dev/sda2`, but
still hit `not found/writable, skipping usb1` — the assumed sysfs path
(`/sys/devices/platform/e0400000.usb/musb-hdrc.1/mode`, guessed from
DT node names alone) was wrong. Confirmed on-device via
`find /sys/devices/platform -iname mode | grep musb`: the real path is
`/sys/devices/platform/ahb/e0400000.usb/musb-hdrc.1/mode` — the DTS's
`ahb` simple-bus container node (parent of the `usb0`/`usb1` nodes)
adds an extra path component that guessing from the DT alone missed.
Fixed in `prado-firmware-reconstruction` `d42f1b3`. **Confirmed
working on hardware 2026-07-22** — usb1 now goes straight to host
mode on a `root=/dev/sda*` boot, no wired-negotiation retry cycling.
This closes out the whole USB OTG/host-mode investigation: both usb0's
boot-speed fix and usb1's test-boot fast path are hardware-confirmed
end to end.

### Summary of commits (`linux-arkmicro`)

| Commit | What |
|---|---|
| `83ab185e6` | First `usb0 dr_mode=host` attempt (2026-07-19) |
| `d44cce385` | Reverted — unexplained bus1→bus2 renumbering regression |
| `9fa92d4b1` | Re-attempt + cross-port register/GPIO diagnostic logging (2026-07-22) |
| `d56d5a750` | Real fix: `MUSB_HOST` now does soft-reset + VBUS power-cycle |
| `9f678777e` | Diagnostic: usb1 also `host`, to rule out OTG activity as the CRLF bug's cause |
| `17a80acff` | usb1 reverted to `otg`; diagnostic logging removed; log messages tidied |
| `d6be430dd` | Routine OTG recovery messages: `dev_alert` → `dev_dbg` |
| `69c59f33a` | `g_ncm` net device renamed `usb%d` → `carplay-ncm%d` |
| `a2a8daac3` | `MUSB_HOST` case now sets `otg->state`, needed for the runtime host-mode switch below |

And in the main `prado-firmware-reconstruction` repo:

| Commit | What |
|---|---|
| `8d609a5` | `build_bootable_sdcard.sh`: re-strip CRLF after `apply_overlay`, not just before it |
| `d1c2ebc` | `switchotg.sh`/`rcS`: reflects usb0=storage-only, usb1=CarPlay; `carplay-ncm0` rename |
| `9f2ef8d` | `switchotg.sh`: force `usb1` to host mode on USB-stick test boots for fast WiFi |
| `c1515fe` | `rcS`: actually call `switchotg.sh` — it had no caller at all before this |
| `d42f1b3` | `switchotg.sh`: fix usb1 mode sysfs path — missing `ahb` DTS bus node component |

### `usb0` dr_mode made boot-command-dependent, restoring wired CarPlay on `bootmmc` (2026-07-27)

Wired-CarPlay bench testing needed to actually use `usb0` (the shared
storage/CarPlay connector), which the tradeoff above ruled out
entirely. Implemented the "boot-mode-dependent DTB" option that
section flagged but never built:

- **`linux-arkmicro`'s `ark1668.dtsi`**: `usb0`'s `dr_mode` reverted
  from the hardcoded `"host"` (since `9fa92d4b1`, 2026-07-22) back to
  `"otg"` — the DTS default now has real OTG/gadget capability again,
  since `musb_core` only calls `musb_gadget_setup()` at DTS-parse
  time, not something a runtime sysfs write can restore after the
  fact (the same constraint documented above for why `switchotg.sh`
  couldn't touch `usb0` at all before this).
- **`bootmmc`/`bootsd`** (the real vehicle boot path): get this `otg`
  default unchanged — a wired CarPlay cable can now negotiate through
  `usb0` on a normal boot.
- **`bootusb`** (U-Boot, `ark1668_boot_cmds.c`'s
  `boot_from_block_dev()`): patches the *in-RAM* DTB it just
  `fatload`'d, right before `bootz` —
  `fdt addr <dtbaddr>; fdt set /ahb/usb@E0100000 dr_mode "host"` —
  restoring the original fast host-mode boot (skips the ID-pin
  negotiation retries) for that path specifically, since a USB-stick
  test boot already occupies the one connector a CarPlay cable would
  need, so there's no tradeoff to make there.
- `firmware_overlay/etc/switchotg.sh`'s comment (previously stated
  `usb0` has *no* gadget capability at all, unconditionally) corrected
  to describe the new per-boot-command behavior — the runtime script
  still has nothing to do for `usb0` on either path, just for
  different reasons now (DTS default handles `bootmmc`, the U-Boot fdt
  patch handles `bootusb`).

Both kernel/DTB and U-Boot rebuilt clean. **Not yet hardware-tested.**
Two things the next test needs to confirm: (1) a wired phone on
`bootmmc` actually negotiates OTG/gadget through `usb0` now: check
`fdt print /ahb/usb@E0100000 dr_mode` at the U-Boot prompt, or
`cat /proc/device-tree/ahb/usb@e0100000/dr_mode` once booted, before
assuming any remaining wired-CarPlay problem is elsewhere in the stack
(`carplay-ncm`/`f_ncm.c`, `libAutoDongle.so`); (2) `bootmmc` with
*nothing* plugged into `usb0` doesn't reintroduce the original
multi-second ID-pin negotiation delay on ordinary (non-CarPlay-testing)
boots — if that turns out to matter, this may need a further
compromise (e.g. only patch `otg` back in for a `bootmmc`-carplay-test
variant command, defaulting normal `bootmmc` to `host` too).

Commit: `linux-arkmicro b04c6e59f` (DTS + boot command);
`prado-firmware-reconstruction 7942537` (`switchotg.sh` comment fix).

#### `bootusb` side hardware-tested and fixed (2026-07-27)

First hardware test (`docs/logs/new uboot new kernel baseline
v19_260727.txt`) showed the fix wasn't taking effect on `bootusb` —
`usb0` still ran the full slow OTG negotiation cycle (`switch
peripheral`/`switch otg`/`Cannot enable... attempt power cycle`,
USB stick not detected until ~14.6s, same as before `dr_mode="host"`
existed at all). The U-Boot console log itself pinpointed it exactly:

```
libfdt fdt_setprop(): FDT_ERR_NOSPACE
[bootusb] warning: failed to force usb0 dr_mode=host in DTB, keeping DTS default (otg) -- boot will be slower but should still work
```

Root cause: `fatload` gives the in-RAM DTB blob its exact on-disk
file size — zero slack space. `fdt_setprop()` growing `dr_mode` from
`"otg"` (4 bytes incl. NUL) to `"host"` (5 bytes) is only a 1-byte
grow, but even that fails outright without first padding the working
copy via `fdt resize`. The code's own fallback warning fired
correctly both times (confirms that graceful-degradation path itself
works as designed) — this was diagnosed straight from the printed
error, not a mystery.

Fixed: added `fdt resize 64` right after `fdt addr`, before
`fdt set`, in `boot_from_block_dev()`. Commit: `linux-arkmicro
3ed011608`.

**Hardware-confirmed working, same day.** Retest showed a clean
`bootusb` boot: no `FDT_ERR_NOSPACE`, no fallback warning, and
`reserving fdt memory region: addr=2000000 size=5000` present in the
log (the `fdt resize` padding taking effect). The `bootmmc`
wired-CarPlay side above still remains untested.
