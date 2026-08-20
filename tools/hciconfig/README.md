# hciconfig (Static ARM build)

Stand-alone, fully statically-linked ARM binary of BlueZ's standard `hciconfig` diagnostic and configuration utility.

## Purpose

The target rootfs does not ship with BlueZ command-line utilities. This binary provides low-level Linux kernel Bluetooth adapter inspection and control (`hciconfig hci0`, `hciconfig hci0 up/down`, `hciconfig hci0 name`, `hciconfig hci0 piscan`, etc.) directly over the kernel's `AF_BLUETOOTH` raw socket and `ioctl` interface without any dynamic shared library dependencies.

## Source Code

The source code in `src/` is extracted from upstream BlueZ 5.66:
- `src/hciconfig.c` — Main command line parser and ioctl interface.
- `src/lib/bluetooth.c`, `src/lib/hci.c` — Core Bluetooth address formatting and raw HCI command/event packet helpers.
- `src/src/textfile.c`, `src/src/shared/util.c` — Storage helpers and string utilities.

## Key Changes & Settings

1. **No External Library Dependencies**: Built without D-Bus, GLib, or Readline dependencies.
2. **Fully Static Linking**: `-static` ensures the binary runs on any kernel/glibc version without runtime symbol mismatch.
3. **No NSS/dlopen Crash Vector**: Because `hciconfig` does not reference NSS database calls (`getpwnam`, `getpwuid`, `getaddrinfo`) or `dlopen`, it runs cleanly as a static binary without requiring stub symbol wrapping.

## Building

```sh
make
```

Cross-compiles with `arm-linux-gnueabihf-gcc` and strips the resulting executable (`hciconfig`, ~562 KB).
