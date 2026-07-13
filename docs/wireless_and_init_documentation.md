# Wireless Interface & Boot Init Documentation

This document explains the physical pin mapping, modules, and commands required to initialize WiFi and Bluetooth on the Prado Limcet P305 board running the 4.19.192 kernel.

---

## 1. Hardware Architecture & Interfaces

| Component | Chip Model | Bus / Interface | Control GPIO |
|---|---|---|---|
| **WiFi** | Realtek RTL8811CU | USB 2.0 (Bus 2, Port 1) | None (USB bus-powered) |
| **Bluetooth** | Realtek RTL8762BTV | HS-UART (`/dev/ttyHS1`) | **GPIO 91** (Enable pin) |

---

## 2. Boot Script Setup (`/etc/rc.d/rcS`)

During boot, the following steps are performed:
1. **Modules Path Redirect:** The legacy path `/lib/modules/3.4.0` is symlinked to the running `/lib/modules/4.19.192` kernel directory.
2. **Symlinks for Wireless Drivers:** A symlink for `wlan_rtl8811cu.ko` -> `rtl8811cu.ko` is created dynamically so userspace scripts expecting the legacy name don't break.
3. **Module Auto-Load:** The `rtl8811cu` WiFi driver is automatically loaded on boot.

---

## 3a. Default boot mode changed 2026-07-14 -- client (STA), not AP

`rc.d/rcS` now calls `etc/wifi_client.sh` instead of `etc/wifi_ap.sh` at
boot, so the device joins a real local WiFi network automatically
(easier SSH/testing access) instead of hosting the `carplay_wifi` AP
described below. This was a deliberate choice — a single WiFi radio
generally can't be an AP and a client at once, so it replaces rather
than runs alongside the AP. **Edit `etc/wifi_client.conf` with your real
SSID/password before relying on this** — it ships with placeholder
values that will never connect. The AP script below is untouched and
still works if run manually (`/etc/wifi_ap.sh &`).

## 3. WiFi Access Point Setup Commands (no longer runs automatically -- see 3a)

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
