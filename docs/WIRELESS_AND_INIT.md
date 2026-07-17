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

## 1a. WiFi never enumerates when booting via `bootusb` -- not a bug, one physical port

`musb-hdrc.0`'s hub reports exactly **one** downstream port
(`hub 1-0:1.0: 1 port detected`, confirmed in every captured boot log).
The onboard WiFi module and any external USB stick used to boot via
`bootusb` share that same single port. Confirmed by comparing every
`docs/logs/new kernel bootlog*.txt` capture against its own kernel
command line: every log where WiFi came up successfully
(`rtw_ndev_init(wlan0)`, `hostapd ... AP-ENABLED`) booted with
`root=/dev/mmcblk0p2` (`bootmmc`, SD card); every log where WiFi never
enumerated at all booted with `root=/dev/sda2` (`bootusb`, USB stick) --
a 100% consistent correlation, not a code regression from any specific
commit (initially suspected to be the 2026-07-14 USB VBUS-delay/recovery-
watchdog fixes, `db1da3937`/`07db9a9c3` -- ruled out once the boot-medium
pattern became clear).

**Practical implication**: test WiFi/CarPlay specifically via SD-card
boot (`bootmmc`), not `bootusb` -- the boot stick physically occupies the
only available port for the whole session, leaving nothing for the WiFi
module to enumerate on. A powered USB hub between the board and the
stick (giving the WiFi chip a second downstream port) would resolve this
if both need to be tested at once, but hasn't been tried.

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