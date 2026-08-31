# Engineering Handoff: Realtek RTL8761B & BlueZ Subsystem on Linux 4.19

## Target Audience
This document is prepared for the **Kernel & Systems Engineer Agent** maintaining and compiling the **Linux 4.19** kernel tree for the **ARK1680 (ARM Cortex-A5)** head unit platform.

---

## 1. Hardware Topology & System Specs

| Parameter | Value / Implementation |
|---|---|
| **SoC** | ARK1680 (ARM Cortex-A5), Little Endian |
| **Kernel Target** | Linux 4.19.x LTS |
| **Bluetooth Module** | Feasycom BW121 |
| **Bluetooth Silicon** | **Realtek RTL8761BT** (Dual-mode Bluetooth 5.0 baseband) |
| **Serial Bus** | `/dev/ttyHS1` (`ark1680_hsuart`) |
| **UART Protocol** | **H5 (3-Wire UART)** with hardware RTS/CTS flow control |
| **Operating Baud Rate** | **1,500,000 baud (1.5 Mbps)** |
| **Hardware Reset Pin** | **GPIO 91** (`/sys/class/gpio/gpio91`) |
| **Firmware Blobs on Disk**| `/etc/rtl8761bt_fw` (43,980 bytes), `/etc/rtl8761bt_config` |

```
+───────────────────────────────────────────────────────────────────────────────────+
|                                HARDWARE TOPOLOGY                                  |
|                                                                                   |
|  [ ARK1680 SoC (ARM Cortex-A5, Linux 4.19) ]                                      |
|         │                                                                         |
|         ├── GPIO 91 (Output) ───────────────> [ Hardware Reset (Active Low) ]     |
|         └── /dev/ttyHS1 (ark1680_hsuart) ───> [ 1.5 Mbps 3-Wire UART (H5) ]       |
|                                                     │                             |
|                                                     ▼                             |
|                                     [ Realtek RTL8761BT Silicon ]                 |
|                                     [ Feasycom BW121 Module ]                     |
+───────────────────────────────────────────────────────────────────────────────────+
```

---

## 2. Kernel Source Analysis (`btrtl.c` vs `rtk_hciattach`)

### Why Stock Linux 4.19 `btrtl.c` Only Lists RTL8761A
* In mainline Linux, `drivers/bluetooth/btrtl.c` support for **RTL8761B** was upstreamed in **Linux 5.8** (Commit [`9d38f887b47b`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=9d38f887b47b): *"Bluetooth: btrtl: Add support for RTL8761B"*).
* Stock Linux 4.19 only contains tables for older chips (`RTL8723A/B/D`, `RTL8821A`, `RTL8822B`, `RTL8761A`).

### The Two Solutions for Linux 4.19

#### Approach A: User-Space Attachment via `radxa/rtkbt` (RECOMMENDED — Zero Kernel Driver Patches)
Realtek designed its embedded UART Bluetooth chips to be driven by a user-space patcher tool called **`rtk_hciattach`** (available open-source at [github.com/radxa/rtkbt](https://github.com/radxa/rtkbt) in the `uart/` directory).
1. `rtk_hciattach` opens `/dev/ttyHS1`, speaks Realtek vendor HCI commands over 3-Wire H5, uploads `/etc/rtl8761bt_fw`, switches the baud to 1.5 Mbps, and calls `ioctl(fd, HCIUARTSETPROTO, HCI_UART_3WIRE)`.
2. The standard Linux kernel H5 line discipline registers the interface as **`hci0`**.
3. **No patches to `drivers/bluetooth/btrtl.c` are required.**

#### Approach B: In-Kernel Driver Backport (Optional)
If you prefer in-kernel firmware loading:
1. Backport commit `9d38f887b47b` from Linux 5.8 into `drivers/bluetooth/btrtl.c` and `hci_h5.c`.
2. Place `/etc/rtl8761bt_fw` into `/lib/firmware/rtl_bt/rtl8761b_fw.bin` and config into `/lib/firmware/rtl_bt/rtl8761b_config.bin`.
3. **Important Note (Bazzite #3339 Bug):** Ensure the driver requests `rtl8761b_fw.bin` (UART version), **not** `rtl8761bu_fw.bin` (USB version). Loading the `bu` firmware causes L2CAP socket creation to fail with `br-connection-create-socket`.

---

## 3. Required Kernel Configuration (`.config`)

To support native BlueZ, Bluetooth PAN (internet routing), and external USB peripherals, ensure the following kernel symbols are enabled in the 4.19 `.config`:

```ini
# Core Bluetooth Subsystem
CONFIG_BT=y
CONFIG_BT_BREDR=y
CONFIG_BT_RFCOMM=y
CONFIG_BT_RFCOMM_TTY=y
CONFIG_BT_BNEP=y
CONFIG_BT_BNEP_MC_FILTER=y
CONFIG_BT_BNEP_PROTO_FILTER=y
CONFIG_BT_HIDP=y

# Bluetooth HCI Drivers
CONFIG_BT_HCIUART=y
CONFIG_BT_HCIUART_H4=y
CONFIG_BT_HCIUART_3WIRE=y
CONFIG_BT_HCIUART_RTL=y

# Networking & Bridging for Internet Tethering (PAN)
CONFIG_NET=y
CONFIG_INET=y
CONFIG_BRIDGE=y
CONFIG_VLAN_8021Q=y

# Optional USB Support (for USB RTC / USB GPS / USB LTE dongles)
CONFIG_USB_SERIAL=y
CONFIG_USB_SERIAL_GENERIC=y
CONFIG_USB_SERIAL_CP210X=y
CONFIG_USB_SERIAL_FTDI_SIO=y
CONFIG_USB_SERIAL_PL2303=y
CONFIG_USB_SERIAL_CH341=y
CONFIG_USB_ACM=y
```

---

## 4. Hardware Reset & BlueZ Startup Sequence

Before attaching `hci0` or loading firmware, the Realtek chip must undergo a clean hardware power cycle via GPIO 91.

### Initialization Script Flow

```sh
#!/bin/sh
set -e

# 1. Hardware reset RTL8761BT
if [ ! -d /sys/class/gpio/gpio91 ]; then
    echo 91 > /sys/class/gpio/export
fi
echo out > /sys/class/gpio/gpio91/direction
echo 0 > /sys/class/gpio/gpio91/value
usleep 100000   # 100ms low pulse
echo 1 > /sys/class/gpio/gpio91/value
usleep 200000   # 200ms stabilization delay

# 2. Ensure D-Bus system bus daemon is running
if ! pidof dbus-daemon >/dev/null; then
    mkdir -p /var/run/dbus
    dbus-daemon --system --fork
fi
# NOTE: plain `dbus-daemon` here resolves via PATH to this device's
# STOCK rootfs binary -- confirmed via `strings` to be real D-Bus
# **1.0.2**, which predates NEGOTIATE_UNIX_FD entirely (added in
# 1.3.1). This silently breaks org.bluez.Profile1.NewConnection (the
# call BlueZ uses to hand a connected RFCOMM socket -- a Unix fd -- to
# a registered profile, exactly what wireless Android Auto's RFCOMM
# pairing needs): bluetoothd's own src/profile.c:send_new_connection()
# fails every attempt, logged in gdbus/object.c's
# g_dbus_send_message_with_reply() as "Unable to send message (passing
# fd blocked?)". See §8 below -- this repo now vendors and statically
# cross-builds a real dbus-daemon 1.14.10 specifically to fix this,
# with its own real-hardware NSS gotchas (this device's /etc/passwd
# only has "root", and dbus-daemon's config parser genuinely calls
# getpwnam_r("root", ...) at startup -- not stubbable as a no-op the
# way every other static tool in this repo's NSS shim is).

# 3. Attach RTL8761BT to BlueZ HCI stack
# (rtk_hciattach compiled from radxa/rtkbt/uart)
rtk_hciattach -n -s 1500000 /dev/ttyHS1 rtk_h5 &

# 4. Wait for hci0 to appear and bring it up
TIMEOUT=10
while [ $TIMEOUT -gt 0 ]; do
    if hciconfig hci0 2>/dev/null | grep -q "UP"; then
        break
    fi
    if hciconfig hci0 2>/dev/null | grep -q "DOWN"; then
        hciconfig hci0 up
        break
    fi
    sleep 1
    TIMEOUT=$((TIMEOUT - 1))
done

# 5. Start BlueZ daemon
if ! pidof bluetoothd >/dev/null; then
    /usr/libexec/bluetooth/bluetoothd -n &
fi
```

---

## 5. Clock Synchronization & Internet Routing via Bluetooth PAN (BNEP)

With `CONFIG_BT_BNEP=y` and BlueZ running on `hci0`, the head unit can connect to the phone's **Bluetooth Tethering / Personal Area Network (PAN)**:

```mermaid
sequenceDiagram
    participant HeadUnit as Head Unit (Linux 4.19)
    participant BlueZ as BlueZ (hci0 / BNEP)
    participant Phone as Android / iOS Phone (PAN NAP)
    participant Internet as Internet NTP Server

    HeadUnit->>BlueZ: Connect PAN to Phone MAC
    BlueZ->>Phone: BNEP Connection Request (NAP profile)
    Phone-->>BlueZ: BNEP Link Established -> creates network interface bnep0
    HeadUnit->>HeadUnit: udhcpc -i bnep0 (Obtain IP 192.168.44.x)
    HeadUnit->>Internet: ntpd -q -p pool.ntp.org (over bnep0)
    Internet-->>HeadUnit: Return accurate UTC time
    HeadUnit->>HeadUnit: settimeofday() -> System wall clock updated!
```

### Steps to Test:
1. Enable **Bluetooth Tethering** in phone Settings.
2. Pair with the phone: `bluetoothctl pair <PHONE_MAC>` and `bluetoothctl trust <PHONE_MAC>`.
3. Connect network: `bt-network -c <PHONE_MAC> nap` (or via D-Bus Network1 interface).
4. Run DHCP client: `udhcpc -i bnep0 -n -q -s /etc/udhcpc.script` — the `-s` script argument is
   **required**; `udhcpc`'s exit code 0 only means "a DHCP server replied", not that the lease was
   ever applied (no compiled-in default script on this device's busybox build). Omitting it was a
   real bug here: `bnep0` never got an IP/route at all, which also silently broke DNS (nothing to
   do with `/etc/resolv.conf`, which already has `nameserver 8.8.8.8` baked in — there was just
   nowhere to route the query).
5. Sync time: `ntpd -n -q -p 216.239.35.0` (a literal IP, not a hostname — avoids depending on DNS
   resolution succeeding at all) or `sntp -s 216.239.35.0`.

**Real, hardware-confirmed alternative that avoids PAN/tethering entirely**: BLE GATT Current Time
Service (CTS, UUID `0x1805`/characteristic `0x2A2B`) — a standard BLE SIG profile many phones
implement as a GATT server. `custom_ui/src/hal/ble_cts.cpp` reads it directly over the already-paired
BLE link via `GetManagedObjects`/`GattCharacteristic1.ReadValue`, no tethering toggle or PAN
connection needed. Tried first in `hal::bluetooth.cpp`'s `sync_clock_from_phone()`, falling back to
the PAN path above only if BLE CTS isn't available on the paired device.

---

## 6. Mutual Exclusion with Stock `blueware`

* **Stock Mode**: `/usr/bin/blueware /etc/blueware-bw121.properties` opens `/dev/ttyHS1` and creates `/dev/bw_serial`.
* **BlueZ Mode**: `rtk_hciattach` opens `/dev/ttyHS1` and creates `hci0`.
* **Rule**: `/dev/ttyHS1` can only be opened by **one process at a time**. If switching between them, kill the active process, pulse GPIO 91 to clear the chip's internal SRAM, and start the desired stack.

---

## 7. Application-Layer Integration (`custom_ui` / `androidauto-sidecar`)

This document covers the kernel/HCI/`bluetoothd` layer only. The application layer built on top of
it — `custom_ui`'s own RFCOMM `Profile1` registration for wireless Android Auto pairing, the
static-`dbus-daemon` rebuild that fixes the fd-passing bug in §4 above, and the migration status of
`custom_ui`'s Bluetooth screen off the legacy `blueware` AT stack — is tracked separately:

* [`custom_ui/docs/BLUEZ_MIGRATION_AND_BLUEWARE_DEPRECATION_HANDOFF.md`](../custom_ui/docs/handoffs/BLUEZ_MIGRATION_AND_BLUEWARE_DEPRECATION_HANDOFF.md)
  — architecture/migration-plan doc for moving `custom_ui`'s Bluetooth screen and HAL off
  `blueware` AT commands onto `org.bluez` D-Bus calls; `custom_ui`'s `hal/bluez_aa_profile.cpp` is
  the one piece of this already implemented and hardware-confirmed (RFCOMM Profile1, channel 1,
  role server, AutoConnect, `ConnectProfile` — see this repo's own commit history).
* [`tools/bluetoothd-test/README.md`](../tools/bluetoothd-test/README.md) — the full real-hardware
  bring-up log for `bluetoothd` 5.66 + the statically cross-built `dbus-daemon` 1.14.10 referenced
  in §4 above, including the exact rebuild recipe (`third_party/dbus-1.14.10.tar.xz`, not the git
  submodule — the submodule alone can't be rebuilt on this machine, see that file's own "Not yet
  hardware-tested" → "FULL SUCCESS" run log for why) and every gotcha hit getting it to link and
  actually start on-device (NSS stub requirements, `-all-static` vs `-static`, `expat.pc`'s baked-in
  prefix, the `root`-username lookup that can't be a no-op).
* [`tools/nss-stub/`](../tools/nss-stub/) — the static-linking NSS shim family this whole static-Bluetooth
  toolchain depends on; `nss_stub_dbus_daemon.c` is the `dbus-daemon`-specific provider (see its own
  file comment for why it can't be a blanket no-op like every sibling provider).
* [`tools/rtk-hciattach-test/`](../tools/rtk-hciattach-test/) — isolates the raw HCI-attach step
  (§2/§4 above) from BlueZ/`bluetoothd` itself; the hardware-confirmed baseline `bluetoothd-test`
  builds on.

---

## 8. Reference Links
* **Realtek Linux Bluetooth UART Driver & `rtk_hciattach` Source**: [radxa/rtkbt (GitHub)](https://github.com/radxa/rtkbt)
* **Linux Kernel Mainline Commit for RTL8761B**: [Commit 9d38f887b47b](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=9d38f887b47b)
* **Firmware Mismatch Diagnostic (Bazzite #3339)**: [ublue-os/bazzite #3339](https://github.com/ublue-os/bazzite/issues/3339)
