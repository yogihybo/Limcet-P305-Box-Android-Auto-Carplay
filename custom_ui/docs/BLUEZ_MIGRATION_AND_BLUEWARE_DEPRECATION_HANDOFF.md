# Handoff: BlueZ 5.66 Migration & Blueware AT Deprecation Architecture

## 1. Executive Summary & Context

During live testing of `custom_ui`, the process attempted to spawn `blueware` and send AT commands (`AT+PLIST`, `AT+HFPCONN=E`) over `/dev/bw_serial`.

### The Core Architectural Question
> *"The stack is migrating to BlueZ, so shouldn't the blueware commands be removed? Or did the other agent miss some?"*

### Current Status
1. **BlueZ Kernel & Daemon Subsystem (Hardware-Validated)**:
   - On **2026-08-19**, upstream Linux **BlueZ 5.66** was successfully brought up on the ARK1668 platform:
     - `rtk_hciattach` initializes the RTL8761BTV controller over `/dev/ttyHS1` (3-Wire H5 @ 1.5 Mbps, GPIO 91 reset) and attaches kernel interface **`hci0`**.
     - `bluetoothd 5.66` registers on D-Bus (`org.bluez`), creating standard A2DP, AVRCP, and PAN endpoints.
   - See `../../docs/BLUEZ_AND_KERNEL_BLUETOOTH_HANDOFF.md` for the kernel/HCI-layer subsystem doc
     (Kconfig, GPIO reset sequence, `rtk_hciattach`) and `tools/bluetoothd-test/README.md` for the
     real-hardware bring-up run log.
2. **Wireless Android Auto RFCOMM (Hardware-Confirmed, 2026-08-20)**:
   - `custom_ui/src/hal/bluez_aa_profile.cpp` registers a real `org.bluez.Profile1` for the wireless
     AA RFCOMM UUID (channel 1, `Role=server`, `AutoConnect=true`), and `androidauto-sidecar`
     receives the phone's connection via `Profile1.NewConnection`'s passed file descriptor.
   - This depends on a **real `dbus-daemon`** supporting `NEGOTIATE_UNIX_FD` (added in D-Bus 1.3.1) —
     this device's stock rootfs `/usr/bin/dbus-daemon` is D-Bus **1.0.2** and silently fails every
     `NewConnection` fd-passing attempt (`gdbus/object.c`: `"Unable to send message (passing fd
     blocked?)"`). Fixed by statically cross-building a real `dbus-daemon` 1.14.10 and deploying it
     to that exact path — see `tools/bluetoothd-test/README.md`'s "`dbus-daemon` itself" section for
     the full rebuild recipe and the real-hardware gotchas (NSS stub needing a genuine
     `getpwnam_r("root", ...)` lookup, not a no-op; `-all-static` vs bare `-static`; etc.).
3. **`custom_ui` Application Layer (Partially Migrated)**:
   - `custom_ui/src/hal/bluetooth.cpp` and `custom_ui/src/ui/bluetooth_screen.cpp` are still
     implemented against the **legacy proprietary Feasycom `blueware` AT daemon** (`/dev/bw_serial`)
     for pairing/device-list/HFP UI — this part of the migration below is **not yet done**.
   - `custom_ui/src/hal/ble_cts.cpp` is a second, narrower real-BlueZ integration already
     hardware-verified-logging (not yet hardware-confirmed end-to-end): reads BLE GATT Current Time
     Service (UUID `0x1805`/`0x2A2B`) directly via `libdbus`/`GetManagedObjects`, as a
     tethering-free alternative to the PAN clock-sync path.
   - When `custom_ui` starts, `ensure_bluetooth_daemon_running()` still spawns `blueware` and opens
     `/dev/bw_serial` for everything except the two real-BlueZ paths above.
   - **The migration to BlueZ is a work in progress**: `blueware` AT commands must now be formally
     deprecated and replaced with standard D-Bus calls to `org.bluez` for the remaining UI/pairing
     surface (§4 below).

---

## 2. Diagnosis of the `HFPCONN=E` & Page Fault `[00000001]` Incident

During startup with no paired devices stored in `blueware`:
1. `custom_ui` issued `AT+PLIST\r\n`.
2. `blueware` responded with `+PLIST=E` (its internal sentinel indicating the paired list is empty).
3. `send_command` stripped `+PLIST=` and returned `["E"]`.
4. `auto_reconnect_paired_device` treated `"E"` as a device entry. When `split_plist_entry("E", ...)` returned false, it fell back to sending `AT+HFPCONN=E\r\n`.
5. `blueware`'s internal C-string parser attempted pointer arithmetic on the 1-byte string `"E"`, dereferenced address `0x00000001`, and generated the kernel page fault log:
   ```text
   [  155.446384] pgd = a2ea6ba8
   [  155.449111] [00000001] *pgd=02f3d831, *pte=00000000, *ppte=00000000
   [    7.098510] hal::bluetooth::send_command: adapter reported 'ERR002' for command 'HFPCONN=E'
   ```
6. **Architectural Takeaway**: Proprietary AT parsers over serial virtual devices are brittle and prone to memory faults. Migrating to strongly typed BlueZ D-Bus messages completely eliminates string-parsing bugs of this class.

---

## 3. BlueZ 5.66 Migration Architecture

```
+─────────────────────────────────────────────────────────────────────────────+
|                              BLUEZ ARCHITECTURE                             |
|                                                                             |
|  [ custom_ui ]                     [ androidauto-sidecar ]                  |
|        │                                     │                              |
|        │ (D-Bus API)                         │ (AF_BLUETOOTH / RFCOMM)       |
|        ▼                                     ▼                              |
|  [ dbus-daemon ] ──────────────> [ bluetoothd 5.66 ]                        |
|   (/var/run/dbus/system_bus)       (A2DP, AVRCP, SDP, GATT, PAN)            |
|                                              │                              |
|                                              ▼                              |
|                                    [ Linux Kernel 4.19 ]                    |
|                                    (AF_BLUETOOTH, hci0, hci_uart)           |
|                                              │                              |
|                                              ▼ (3-Wire H5 @ 1.5 Mbps)       |
|                                   [ /dev/ttyHS1 (ark-hsuart) ]              |
|                                              │                              |
|                                              ▼                              |
|                                  [ Realtek RTL8761BTV ]                     |
+─────────────────────────────────────────────────────────────────────────────+
```

### 3.1 Daemon Layer Lifecycle
- **Remove**: `ensure_bluetooth_daemon_running()` in `hal/bluetooth.cpp` (which launched `/usr/bin/blueware`).
- **Add**: Boot initialization script (`/etc/init.d/S30bluetooth` or `/etc/rc.d/rcS`):
  ```sh
  # 1. Start system D-Bus daemon
  dbus-daemon --system --fork

  # 2. Attach Realtek RTL8761BTV over UART H5
  rtk_hciattach -n -s 115200 /dev/ttyHS1 rtk_h5 &
  sleep 2

  # 3. Start BlueZ daemon
  bluetoothd -n &
  ```

### 3.2 `custom_ui` HAL: Replace AT Commands with `org.bluez` D-Bus
Replace `/dev/bw_serial` AT strings with standard D-Bus method calls to the `org.bluez` service:

| Operation | Legacy Blueware AT | Upstream BlueZ 5.66 D-Bus Call |
| :--- | :--- | :--- |
| **Enable Adapter** | `AT+BTEN=1` | `Set("org.bluez.Adapter1", "Powered", true)` |
| **Discoverable** | `AT+SCAN=1` | `Set("org.bluez.Adapter1", "Discoverable", true)` |
| **Set Local Name** | `AT+NAME=Prado` | `Set("org.bluez.Adapter1", "Alias", "Prado")` |
| **Scan Nearby** | `AT+INQ=...` | `Call("org.bluez.Adapter1", "StartDiscovery")` |
| **List Paired** | `AT+PLIST` | `GetManagedObjects()` filtering for `org.bluez.Device1` |
| **Connect Device** | `AT+HFPCONN=<MAC>` | `Call("org.bluez.Device1", "Connect")` |
| **Disconnect** | `AT+DISC` | `Call("org.bluez.Device1", "Disconnect")` |
| **Pairing Events** | `+AAPDEV=...` | `PropertiesChanged` on `org.bluez.Device1` |
| **Media Play/Pause**| `AT+PLAYPAUSE` | `Call("org.bluez.MediaControl1", "Play" / "Pause")`|
| **Track Metadata** | `+TRACKINFO=...` | `PropertiesChanged` on `org.bluez.MediaPlayer1` |

### 3.3 `androidauto-sidecar` Wireless Handshake Migration — **DONE, hardware-confirmed 2026-08-20**
- **Legacy**: Opened `/dev/bw_aap` (proprietary Feasycom local socket proxying RFCOMM). Removed.
- **BlueZ Native (as actually implemented)**: rather than a raw `AF_BLUETOOTH`/`BTPROTO_RFCOMM`
  socket opened directly by the sidecar, this project registers a real `org.bluez.Profile1` with
  `bluetoothd` (`custom_ui/src/hal/bluez_aa_profile.cpp`) for the Wireless Android Auto RFCOMM UUID,
  channel 1, `Role=server`, `AutoConnect=true`, and calls `ConnectProfile` — `bluetoothd` itself
  owns the RFCOMM listener and hands the connected socket's file descriptor to this process via the
  `Profile1.NewConnection` D-Bus method (fd-passing, `NEGOTIATE_UNIX_FD`). This needed the real
  `dbus-daemon` 1.14.10 rebuild described in §1 above — the stock 1.0.2 daemon has no fd-passing
  support at all, so `NewConnection` silently failed every attempt until that was fixed.
  Phone then connects over that Bluetooth RFCOMM channel to negotiate the 5GHz WiFi credentials, as
  originally planned.

---

## 4. Migration Action Plan

1. **Step 1: System Boot Integration** — ✅ **hardware-confirmed 2026-08-19**:
   - `rtk_hciattach` + `bluetoothd` bring-up confirmed via `tools/bluetoothd-test/` (not yet
     promoted into `firmware_overlay/`'s production boot path — still diagnostic-only, see that
     tool's own README "Not a stack switcher" note).
   - `hci0` initializes cleanly; not yet automatic at production boot (still manually run via the
     diagnostic tool script).
2. **Step 2: Implement a BlueZ D-Bus client in `custom_ui`** — **not started** for the general
   `BlueZClient` (adapter/device-list/pairing) described in §3.2's table. Two narrower, real
   D-Bus clients exist already and are further along: `hal/bluez_aa_profile.cpp` (Profile1/RFCOMM,
   §3.3 — done) and `hal/ble_cts.cpp` (GATT CTS clock read, real `libdbus` C API, matching
   `bluez_aa_profile.cpp`'s pattern since `dbus-send` can't express an empty `a{sv}` argument
   cleanly).
3. **Step 3: Update UI Screens** — **not started**. `ui/bluetooth_screen.cpp` still drives pairing/
   device-list/HFP UI through `blueware` AT commands.
4. **Step 4: Update `androidauto-sidecar`** — ✅ **done, hardware-confirmed 2026-08-20** (§3.3
   above) — implemented via `Profile1`/`NewConnection` fd-passing, not a raw `BTPROTO_RFCOMM`
   listener as originally sketched here.
5. **Step 5: Deprecate `blueware`** — **not started**, blocked on Steps 2–3 above (the UI/pairing
   surface still has no BlueZ-native replacement to cut over to).
