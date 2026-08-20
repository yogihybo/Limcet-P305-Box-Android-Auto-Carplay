# bluetoothctl (Static ARM build)

Stand-alone, fully statically-linked ARM binary of BlueZ's interactive `bluetoothctl` control client (version 5.66).

## Purpose

The target rootfs does not ship with interactive BlueZ control tools. This binary allows full interactive and scriptable control of the BlueZ Bluetooth daemon (`bluetoothd`) over D-Bus (`show`, `scan on/off`, `pair <mac>`, `trust <mac>`, `connect <mac>`, `info <mac>`, `devices`, etc.).

## Source Code

The source code in `src/` is extracted from upstream BlueZ 5.66:
- `src/client/` — Core `bluetoothctl` command dispatch, menu structures, BLE/GATT and player controllers.
- `src/gdbus/` — BlueZ D-Bus object manager client and proxy framework.
- `src/src/shared/` — Shell readline wrapper, event loop handlers (`mainloop-glib.c`, `io-glib.c`, `timeout-glib.c`), and logging.
- `src/lib/` — Core Bluetooth protocol constants and address parsers.

## Key Changes & Settings

1. **Vendor D-Bus Socket Auto-Detection**:
   - The stock rootfs has a vendor artifact where `dbus-daemon` listens on `unix:path=/var/run/run/dbus/system_bus_socket` (doubled `run/`).
   - `client/main.c` was patched to auto-detect both `/var/run/run/dbus/system_bus_socket` and standard `/var/run/dbus/system_bus_socket`, setting `DBUS_SYSTEM_BUS_ADDRESS` before establishing the connection.
   - Added null pointer safety checks in `client/main.c` so missing daemons fail cleanly with an informative error rather than asserting.

2. **GLib Mainloop & GDBus Event Loop Bridging**:
   - `gdbus` requires the GLib `GMainLoop` / `GMainContext` to dispatch incoming D-Bus method returns and property changed signals.
   - Linked with `src/src/shared/mainloop-glib.c`, `src/src/shared/io-glib.c`, and `src/src/shared/timeout-glib.c` to multiplex readline user input and D-Bus IPC on the same GLib event loop.

3. **NSS / dlopen Stub Wrapping**:
   - glibc 2.34+ incorporates dynamic NSS loading into `libc.a`. Calling or referencing `getpwnam`/`getpwuid`/`getaddrinfo`/`dlopen` in a static binary causes startup assertion failures on this toolchain.
   - Linked against `tools/nss-stub/nss_stub.o` with linker wrapping (`-Wl,--wrap=getpwnam,...`) to ensure 100% crash-free static execution.

4. **Static Dependencies**:
   - Linked statically with `libreadline.a`, `libtinfow.a`, `libdbus-1.a`, `libglib-2.0.a`, and `libpcre2-8.a`.

## Building

```sh
make
```

Cross-compiles with `arm-linux-gnueabihf-gcc` using the staged dependencies and strips the executable (`bluetoothctl`, ~1.9 MB).
