# bluetoothd-test

Diagnostic tool: brings up a real BlueZ `bluetoothd` (5.66) against
this device's kernel `hci0` (see `../rtk-hciattach-test/`, which gets
`hci0` itself to `UP RUNNING` first -- confirmed hardware-working
2026-08-19, `tools/rtk-hciattach-test/README.md`'s Run 6). This tool
answers the next question: does a standard BlueZ D-Bus stack actually
work against this hardware, not just the raw HCI transport.

**Not a stack switcher, not wired into any UI or the production boot
path** (`firmware_overlay/`) -- same philosophy as every other `tools/`
diagnostic here. Nothing gets promoted into `firmware_overlay/etc/profile`
(which currently starts nothing bluetooth-related beyond a *session*
`dbus-launch`, and still runs `blueware`) until this is hardware-confirmed
working end to end.

## Why static, and the NSS/dlopen wrapping

`bluetoothd`'s own `configure`/`Makefile.am` links it dynamically by
default. This device's real userland was built against glibc 2.27; this
project's cross toolchain (plain Debian `arm-linux-gnueabihf-gcc`) targets
a much newer glibc. A dynamically-linked `bluetoothd` built with this
toolchain would need `GLIBC_2.3x` symbol versions this device's runtime
`ld.so`/`libc.so` don't have -- same reasoning this whole project already
follows for every other `tools/*` binary (fully static, no target-side
`.so` dependency at all).

Relinking `-all-static` (not bare `-static` -- libtool treats that as
"prefer static libs for this build", not "force a static executable";
`-all-static` is libtool's actual dedicated flag for forcing the final
program link fully static) surfaced exactly the crash class documented in
`../nss-stub/README.md`: `getpwnam_r`/`getpwuid`/`getgrouplist` (from
libdbus's `fill_user_info()`, privilege-drop code we never exercise since
we run bluetoothd as root), `getaddrinfo` (libdbus's TCP transport, unused
-- we only ever use the unix-domain socket), and `dlopen`
(`src/plugin.c`'s external-plugin loader -- unused, since this build has
`BLUETOOTH_PLUGIN_BUILTIN` and no `.so` files are shipped to its plugin
dir). `../nss-stub/nss_stub.c` gained two new stubs this session
(`__wrap_getgrouplist`, `__wrap_getaddrinfo`) alongside the existing
passwd/group/dlopen set already used by `nano`/`htop`/`tmux`/`gdbserver` --
additive only, doesn't change behavior for any of those. Confirmed all
wraps took (`nm bluetoothd | grep __wrap_`) before shipping the binary
here. **Not yet hardware-tested for bluetoothd specifically** -- the
crash class itself is hardware-confirmed (`nano`, 2026-07-16), but this
exact binary hasn't run on the device yet.

## Cross-build dependency chain

BlueZ's `configure` unconditionally requires `glib-2.0 >= 2.28` (used by
`src/libshared-glib.la`, actually linked into `bluetoothd` -- not just a
test-only dependency) and `dbus-1 >= 1.6`, neither available via apt for
this armhf target on this machine (no multiarch, no cross `.deb`s). Built
from source, staged under `~/Downloads/bt-cross/stage` (not in this repo
-- scratch build area):

`libffi` 3.4.6 -> `pcre2` 10.43 -> `zlib` 1.3.1 -> `glib` 2.78.4 (Meson;
`pip install --user --break-system-packages meson ninja`, since neither
was installed and there's no root) -> `expat` 2.6.2 (for `dbus`'s XML
config parsing) -> `dbus` 1.14.10 (headers + `libdbus-1.a` only -- the
device's own existing `/usr/bin/dbus-daemon` + `libdbus-1.so` are used at
runtime, not a freshly-built one) -> `bluez` 5.66.

`pkg-config` itself wasn't installed either (`apt-get download
pkgconf pkgconf-bin libpkgconf3`, extracted directly with `dpkg-deb -x`,
no root needed).

Real reference for the exact dependency list and sane `configure` flags:
this machine also has `~/Downloads/ark1668ed-bsp/buildroot-2024.02/`
(a separate, never-configured/never-built Buildroot tree for this same
SoC family) with a `package/bluez5_utils/bluez5_utils.mk` recipe --
confirmed it needs exactly `dbus` + `libglib2` (matching this tool's own
chain), and gave the flag set mirrored here (`--disable-obex`,
`--disable-cups`, `--disable-manpages`, `--disable-pie`). Not used
directly (would require setting up a whole separate Buildroot build and
producing a differently-versioned rootfs, out of scope for a diagnostic
binary) -- just cross-checked against for correctness.

### Vendored sources (`third_party/`, git submodules)

2026-08-19: all six cross-compiled dependencies plus BlueZ itself are
now real git submodules here (`third_party/libffi`, `pcre2`, `zlib`,
`glib`, `expat`, `dbus`, `bluez`) -- same convention as
`custom_ui/third_party/aasdk`/`lvgl` -- pinned to the exact tags this
was actually built against (`v3.4.6`, `pcre2-10.43`, `v1.3.1`, `2.78.4`,
`R_2_6_2`, `dbus-1.14.10`, `5.66`). Previously these only existed in an
untracked scratch dir (`~/Downloads/bt-cross/`) outside any repo --
fine for one session, but the whole cross-build dependency chain (glib
requiring Meson, pkg-config not being installed at all, etc. -- see
above) would otherwise have to be rediscovered from scratch next time.

**Not vendored as submodules, deliberately -- easily reproducible without
pinning a source tree:**
- **Meson 1.12.0 / Ninja 1.13.0** (glib's build system) -- installed via
  `pip install --user --break-system-packages meson ninja` (no root, no
  system package available). Pin with `pip install --user
  --break-system-packages meson==1.12.0 ninja==1.13.0` to match exactly.
- **pkgconf 1.8.1 / libpkgconf3 1.8.1** (`pkg-config` itself, also not
  installed) -- extracted directly from Debian's `.deb`s with `dpkg-deb
  -x`, no root needed: `apt-get download pkgconf-bin libpkgconf3`, then
  copy `usr/bin/pkgconf` to somewhere on `$PATH` as `pkg-config` and
  `usr/lib/*/libpkgconf.so.3.0.0` next to it with
  `LD_LIBRARY_PATH` pointed at that directory (the extracted `.so` isn't
  on the system's normal library search path).

To rebuild from these submodules: `git submodule update --init
tools/bluetoothd-test/third_party` (each subdir is already checked out
at the exact tag above), then follow the build order in this section
-- `configure --host=arm-linux-gnueabihf --prefix=/usr
--disable-shared --enable-static` for the autotools ones (`libffi`,
`pcre2`, `zlib`, `expat`, `dbus`), `meson setup --cross-file
<your-cross-file> --default-library=static` for `glib` (see this repo's
own build log/commit messages for the exact option list used -- 
`-Dtests=false -Dman=false -Dgtk_doc=false -Dselinux=disabled
-Dlibmount=disabled -Ddtrace=false -Dsystemtap=false -Dxattr=false
-Dnls=disabled`), each installed into a common staging prefix via
`make install DESTDIR=<stage>` /
`DESTDIR=<stage> ninja install` so `PKG_CONFIG_PATH` finds them for the
next one, then `bluez`'s own `configure --host=arm-linux-gnueabihf
--prefix=/usr --disable-shared --enable-static --with-pic
--disable-obex --disable-cups --disable-manpages --disable-udev
--disable-systemd --disable-client --disable-datafiles
--with-dbusconfdir=/etc CFLAGS="-fno-pie -O2" LDFLAGS="-all-static
-no-pie -Wl,--wrap=getpwnam,--wrap=getpwuid,--wrap=getpwnam_r,--wrap=getpwuid_r,--wrap=getpwent,--wrap=setpwent,--wrap=endpwent,--wrap=getgrgid,--wrap=getgrgid_r,--wrap=getgrouplist,--wrap=dlopen,--wrap=dlerror,--wrap=dlsym,--wrap=dlclose,--wrap=getaddrinfo
<path to ../nss-stub/nss_stub.o>"`. `-all-static`, not bare `-static` --
see this file's own "Why static" section above for why that distinction
matters with libtool.

## What's disabled / not built

- **`bluetoothctl`** (`--disable-client`) -- needs `readline`, not built
  this session (scope cut to get `bluetoothd` itself working first). Use
  `dbus-send` (already on this rootfs, `/usr/bin/dbus-send`) against
  `org.bluez` for initial control/testing instead -- see
  `bt-daemon-probe.sh`'s own final echo for an example call.
- **OBEX** (`--disable-obex`) -- file transfer profile, needs `libical`,
  out of scope for now.
- **udev** (`--disable-udev`) -- no `libudev` on this device.
- **systemd** (`--disable-systemd`) -- no systemd on this device at all.
- A2DP/AVRCP/BAP/MCP/VCP (audio profiles) are all still **enabled**
  (BlueZ's own default -- `--disable-a2dp` etc. would be needed to turn
  them off, not the other way around), since this project already has a
  real, working PCM audio pipeline (`android_auto_screen`/`sink`'s audio)
  that a working BlueZ audio profile could plausibly reuse later.

## Kernel requirement

`CONFIG_BT_RFCOMM`, `CONFIG_BT_RFCOMM_TTY`, `CONFIG_BT_BNEP`,
`CONFIG_BT_BNEP_MC_FILTER`, `CONFIG_BT_BNEP_PROTO_FILTER`,
`CONFIG_BT_HIDP`, `CONFIG_RFKILL` -- all newly enabled this session
(`hardware/kernel_dot_config`), on top of `CONFIG_BT_HCIUART_3WIRE`
already flashed for `../rtk-hciattach-test/`. A2DP/GATT/LE profiles don't
need RFCOMM/HIDP/BNEP, but SPP/HID-over-Bluetooth do -- built in now so
this doesn't become a second silent gap the way `-s 1500000` was for the
HCI attach itself. **Not yet flashed as of this tool's initial commit --
same `zImage.w_dtb` needs reflashing with this newer `.config`.**

## D-Bus wiring

This device's `/etc/profile` only starts a *session* `dbus-launch`
(`eval \`dbus-launch --auto-syntax\``) -- no system bus daemon runs at
all currently. `bt-daemon-probe.sh` starts one itself
(`dbus-daemon --system`-equivalent via explicit `--config-file`/
`--address`, since the rootfs's own `/usr/etc/dbus-1/system.conf`
resolves its default listen address to a slightly malformed
`unix:path=/var/run/run/dbus/system_bus_socket` -- doubled `run/`,
apparently a real, if odd, vendor build artifact in how that file's
`@DBUS_SESSION_BUS_LISTEN_ADDRESS@`-style substitution was configured;
sidestepped entirely by passing `--address=` explicitly rather than
relying on it) and stages this tool's own `dbus-policy/bluetooth.conf`
(BlueZ's stock `org.bluez` D-Bus security policy, root-owned since
everything runs as root here) into `/usr/etc/dbus-1/system.d/` at
runtime, not via `firmware_overlay/`.

## Usage

```
./bt-daemon-probe.sh
```

Runs `bluetoothd -n -d` in the foreground. From another shell:

```
dbus-send --system --print-reply --dest=org.bluez / \
  org.freedesktop.DBus.ObjectManager.GetManagedObjects
```

should show `hci0`'s `org.bluez.Adapter1` object if everything's working.

## Status

**Run 1 (2026-08-19, real hardware)**: kernel flashed with the new
Bluetooth Kconfig options, `hci0` came up cleanly via
`../rtk-hciattach-test/` first (same clean run as that tool's own Run
6). `bluetoothd` itself started and printed `Bluetooth daemon 5.66` --
**the static-NSS/dlopen crash class this tool was built defensively
against never fired** (real hardware confirmation the `--wrap` set is
complete and correct, first time this exact binary has run on-device).

Failed at the very next step: `Failed to connect to socket
/var/run/dbus/system_bus_socket: No such file or directory`. Root
cause found and fixed same day: `bt-daemon-probe.sh` checked `pidof
dbus-daemon` to decide whether to start its own system bus daemon --
but `/etc/profile` already starts a *session* bus on every login shell
(`dbus-launch --auto-syntax`), so a `dbus-daemon` process is
essentially always already running, just not listening on the system
bus socket bluetoothd needs. `pidof` can't distinguish the two.
Multiple `dbus-daemon` processes coexist fine (independent sockets) --
fixed by checking for the actual socket file
(`[ -S "$BUS_SOCKET_DIR/system_bus_socket" ]`) instead of any process
by that name.

**Run 2 (2026-08-19, real hardware)**: the pidof fix from Run 1 wasn't
enough -- `dbus-daemon --config-file=... --address=... --nofork`
printed dbus-daemon's own usage banner and exited immediately (`$!`
still captured its PID regardless, so the script's own success message
was misleading). Root cause: `strings` on the real on-device
`/usr/bin/dbus-daemon` binary shows its embedded usage text has **no
`--address` entry at all** -- an old/reduced build that doesn't support
that flag, unlike the 1.14.10 reference this tool vendors for headers
only. Independently confirmed via `dbus-send`'s own client-side error
message, which named the exact same real default address this device's
`/usr/etc/dbus-1/system.conf` resolves to:
`unix:path=/var/run/run/dbus/system_bus_socket` (the doubled `run/run`
-- a real, if odd, vendor build artifact, not a typo). Fixed by
dropping `--address=` entirely and just using that real default path
directly instead of fighting it -- `dbus-daemon --config-file=... --nofork`
alone, with the script's own socket-existence check pointed at the
right location.

**Run 3 (2026-08-19, real hardware)**: dbus-daemon started this time
(no more usage-banner exit) but failed differently: `Failed to start
message bus: Could not get UID and GID for username "messagebus"`.
Real config's `<user>messagebus</user>` privilege-drop can never
succeed on this rootfs -- `/etc/passwd` has only `root`, no
`messagebus` system user was ever created. Fixed with
`dbus-policy/system-diagnostic.conf`, a copy of the real
`/usr/etc/dbus-1/system.conf` with only that one line changed to
`<user>root</user>` (everything else on this device already runs as
root) -- staged over the real `system.conf` path (not a separate file
elsewhere) so its own `<includedir>system.d</includedir>` still finds
`bluetooth.conf`.

**Run 4 (2026-08-19, real hardware)**: dbus-daemon rejected
`system-diagnostic.conf` outright: `Error loading config file: 'Double
hyphen within comment'`. XML comments can't contain `--` anywhere
inside them; that new file's own explanatory comment (several em-dash-
style `--` usages) violated that, not the real upstream content it was
based on. Reworded the comment and verified with `xml.dom.minidom`
that the file now parses as well-formed XML before shipping it again.

## Run 5 (2026-08-19, real hardware): FULL SUCCESS -- BlueZ working end to end

System dbus-daemon started clean, `bluetoothd` came up completely:

- All real plugins loaded (`a2dp`, `avrcp`, `network`, `input`, `hog`,
  `gap`, `scanparam`, `deviceinfo`, `battery`, `hostname`, `wiimote`,
  `autopair`, `policy`) -- only `vcp`/`mcp`/`bap` (LE Audio) declined
  with "D-Bus experimental not enabled", expected: this build didn't
  pass `--enable-experimental`, not a bug.
- `Bluetooth management interface 1.14 initialized`, adapter
  `/org/bluez/hci0` registered (`src/adapter.c:adapter_register()
  Adapter /org/bluez/hci0 registered`).
- Real SDP service records added for SPP/OBEX/PAN/HID/A2DP/AVRCP
  (handles `0x10001`-`0x10005`), device ID record, GATT Manager and LE
  Advertising Manager both registered for the adapter.
- Every single `mgmt` command in the whole bring-up sequence completed
  with status `0x00` (success) -- device class, local name (`BlueZ
  5.66`), link keys/LTKs/IRKs/connection params all loaded cleanly for
  `hci0` with zero errors.

This is the entire real BlueZ D-Bus stack working end to end against
this hardware for the first time -- settles both this tool's and
`../rtk-hciattach-test/`'s original question definitively: this device
can run a completely standard Linux Bluetooth stack, `blueware`'s
AT-command daemon was never load-bearing at any layer.

Not yet exercised: actual pairing/connection to a real phone/peripheral
(no `bluetoothctl` in this build, see "What's disabled" above -- would
need `dbus-send`-driven `Adapter1.Powered`/`Adapter1.Discoverable`
calls, or building `bluetoothctl` with readline, to test that next).
