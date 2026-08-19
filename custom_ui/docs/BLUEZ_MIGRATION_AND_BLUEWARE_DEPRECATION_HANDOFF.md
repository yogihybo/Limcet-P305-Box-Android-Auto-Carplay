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
2. **`custom_ui` Application Layer (Not Yet Migrated)**:
   - `custom_ui/src/hal/bluetooth.cpp` and `custom_ui/src/ui/bluetooth_screen.cpp` are currently still implemented against the **legacy proprietary Feasycom `blueware` AT daemon** (`/dev/bw_serial`).
   - When `custom_ui` starts, `ensure_bluetooth_daemon_running()` spawns `blueware` and opens `/dev/bw_serial`.
   - **The migration to BlueZ is a work in progress**: `blueware` AT commands must now be formally deprecated and replaced with standard D-Bus calls to `org.bluez`.

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

### 3.3 `androidauto-sidecar` Wireless Handshake Migration
- **Legacy**: Opened `/dev/bw_aap` (proprietary Feasycom local socket proxying RFCOMM).
- **BlueZ Native**:
  1. Register the Android Auto RFCOMM SDP service record with `bluetoothd`:
     - Service UUID: `4de17a00-52cb-11e6-bdf4-0800200c9a66` (Wireless Android Auto RFCOMM).
  2. Open a standard Linux kernel Bluetooth socket in C++:
     ```cpp
     #include <bluetooth/bluetooth.h>
     #include <bluetooth/rfcomm.h>

     int fd = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
     struct sockaddr_rc addr = {};
     addr.rc_family = AF_BLUETOOTH;
     bacpy(&addr.rc_bdaddr, BDADDR_ANY);
     addr.rc_channel = 1; // Or allocated RFCOMM channel
     bind(fd, (struct sockaddr *)&addr, sizeof(addr));
     listen(fd, 1);
     ```
  3. Phone connects directly over Bluetooth RFCOMM to negotiate the 5GHz WiFi credentials.

---

## 4. Migration Action Plan

1. **Step 1: System Boot Integration**:
   - Add `rtk_hciattach` and `bluetoothd` to system startup scripts.
   - Verify `hci0` initializes automatically at boot.
2. **Step 2: Implement `BlueZClient` in `custom_ui`**:
   - Create `custom_ui/src/hal/bluez_client.h` and `bluez_client.cpp` using `libdbus-1`.
   - Implement `get_adapter()`, `list_devices()`, `start_discovery()`, `connect_device()`, and `get_telemetry()`.
3. **Step 3: Update UI Screens**:
   - Point [`ui/bluetooth_screen.cpp`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/ui/bluetooth_screen.cpp) and [`ui/status_bar.cpp`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/ui/status_bar.cpp) to `BlueZClient`.
4. **Step 4: Update `androidauto-sidecar`**:
   - Replace `/dev/bw_aap` polling with a native `BTPROTO_RFCOMM` listener.
5. **Step 5: Deprecate `blueware`**:
   - Remove `/dev/bw_serial`, `/dev/bw_aap`, `/dev/bw_iap`, and `blueware` properties files.
