# tools/

Diagnostic and on-device utility tools — background doc for [README §12.0 Device
Access](../README.md#120-device-access).

`tools/` holds static ARM binaries (and a few POSIX shell wrappers), each with its own
`README.md`. Synced into `firmware_overlay/prado/usr/bin/`, so they're unconditionally part of
every build's rootfs (see that directory's `README.md`) — no separate install step or toggle. All
statically linked, no dependency on anything else in the rootfs — they work even while chasing a
boot/crash problem elsewhere in the system.

## I2C / GPIO / pinmux

| Tool | Purpose |
|------|---------|
| `i2c-scan` | Scan I2C buses for ACKing devices |
| `i2c-dump` | Dump registers off a specific I2C address |
| `i2c-write` | Raw single-register I2C write, bypassing any kernel driver bound to that address |
| `i2c-read-raw` | Plain multi-byte I2C read — no register-address write phase, no repeated start |
| `i2c-gpio-bruteforce` | Bit-bangs every candidate pin pair as SCL/SDA to find a chip's real wiring when the DTS assignment is wrong |
| `gpio-i2c-probe` | Bit-bang GPIO/I2C probing, independent of the kernel's own i2c-gpio driver |
| `pin-dump` | Live SoC pinmux register dump, cross-checked against the pinctrl driver's table |
| `pin-force` | Forces an ARK1668 LCD RGB888 data pin into GPIO mode at a specific level (or restores it), via raw register writes |
| `pinmux-watch` | Tight-loop poller for the LCD RGB888 pad-mux registers, to catch a live function-select change in the act |

## Display / video

| Tool | Purpose |
|------|---------|
| `lcd-test` | Raw `/dev/fb0` framebuffer test — info dump, then cycles fills/bars/gradient |
| `fb-scan` | Locates solid-color rectangles in the live framebuffer and predicts the correct color for near-black cells |
| `fb-alpha-test` | Paints labeled bands into `/dev/fb0` to determine the LCDC OSD1 layer's real alpha-blend/channel-order behavior |
| `lcdc-regdump` | Dumps every named ARK1668 LCDC register by name, for stock-vs-build diffing |
| `hx170-test` | Standalone test of the Hantro `hx170dec` H.264 decoder via `libmfc.so`, bypassing `sink`/Android Auto/network |
| `mem-dump` | Hex-dumps physical memory via `mmap()`'d `/dev/mem` — reaches DMA carve-out regions `dd`/`/dev/mem` reads can't |
| `mem-fill` | Write-side companion to `mem-dump` — fills a physical memory range with a repeating pattern |

## Touch / MCU / misc hardware

| Tool | Purpose |
|------|---------|
| `ark1680-ts-test` | ARK1680 resistive-ADC touchscreen driver diagnostic — register dump + evdev event watcher |
| `mcu-handshake` | Native C reimplementation of the MCU UART handshake protocol |
| `dmesg` | Static util-linux `dmesg` — timestamps/facility-decoding/color that BusyBox's built-in applet lacks |
| `strace` | Upstream syscall tracer (static build) |
| `audio-test` / `touch-selftest` / `uart-test` / `bt-test` / `usb-test` / `mmc-test` | Automated pass/fail wrapper scripts, one per subsystem |
| `rtk-hciattach-test` | One-shot manual diagnostic (not auto-run, not a stack switcher): attempts kernel-HCI `hci0` bring-up against the real Bluetooth module via `rtk_hciattach`, as an alternative to stock `blueware` — see its own `README.md` |

## General shell utilities

Not diagnostic-specific, but this rootfs's busybox lacks them:

| Tool | Purpose |
|------|---------|
| `nano` | Text editor, for editing config/log files directly on the device |
| `less` | Proper pager (busybox only has a bare `more`) |
| `htop` | Interactive process/CPU/memory viewer |
| `tmux` | Terminal multiplexer — sessions survive a dropped serial/telnet connection |
| `gdbserver` | Live remote debugging — attach a host `gdb`/`gdb-multiarch` over TCP for real register/stack/memory state, instead of reconstructing it from a post-mortem minidump and disassembly (see `docs/1.5_AUDIO_SUBSYSTEM_INVESTIGATION.md` for exactly the kind of investigation this replaces) |
| `nss-stub` | Static-linkage NSS stub object linked into `nano`/`htop`/`tmux`/`gdbserver` and busybox itself — see [Static ARM+glibc NSS crash workaround](nss-stub/README.md) |

`nano`/`less`/`htop`/`tmux` are linked against a static `ncurses` build with
`vt100`/`linux`/`xterm`/`ansi` terminal descriptions compiled directly in, since this rootfs has
no terminfo database — the serial console's `TERM=vt100` (`/etc/inittab`) is covered. `tmux`
additionally links a static `libevent`. Both persisted in the separate `linux-arkmicro` repo
(`buildroot-external/arm-static-libs/`) so future tool builds don't need to rebuild them from
source.

## Host-side scripts

Run on a dev machine, not on the device:

| Tool | Purpose |
|------|---------|
| `msncore_analyze.py` | Deconstructs the `MsnCoreApp` Qt UI binary using its unstripped sibling build's symbol table, for targeted patching |
| `rcc_extract.py` | Extracts a Qt binary resource bundle (`.rcc`) to a directory |
