# dmesg

util-linux 2.40.2's `dmesg`, cross-compiled fully static for the target
(ARM EABI5, armhf). BusyBox's built-in `dmesg` applet only supports
`-c`/`-n LEVEL`/`-r`/`-s SIZE`/`-w` -- no human-readable timestamps
(`-T`), no facility/level decoding (`-x`), and no colorized output
(`--color`), all of which this build provides.

Installed as `/usr/bin/dmesg`. Since `/bin` (where BusyBox's `dmesg`
symlink lives) comes before `/usr/bin` in `$PATH` (see `/etc/profile`),
this binary does *not* shadow the BusyBox applet by bare name --
`/etc/profile` gets a `dmesg` alias pointing at the absolute path
instead (see `build_bootable_sdcard.sh`'s `install_diag_tools()`).

## Build

```sh
curl -sL -o util-linux-2.40.2.tar.gz \
  https://mirrors.edge.kernel.org/pub/linux/utils/util-linux/v2.40/util-linux-2.40.2.tar.gz
tar xzf util-linux-2.40.2.tar.gz && cd util-linux-2.40.2

./configure --host=arm-linux-gnueabihf --build=x86_64-linux-gnu \
  --prefix=/tmp/util-linux-install \
  --disable-shared --enable-static \
  --disable-nls --disable-widechar \
  --disable-liblastlog2 --disable-pam-lastlog2 \
  --without-python --without-systemd --without-ncurses --without-ncursesw \
  --without-tinfo --without-udev --without-selinux --without-cryptsetup \
  --without-econf \
  LDFLAGS="-static -no-pie"

make dmesg -j$(nproc)
```

Two build-system quirks hit during this build, both worked around above
(or need a manual step below):

1. **`--enable-static-programs=dmesg` doesn't exist.** util-linux's
   static-link allowlist (`UL_STATIC_PROGRAMS` in `configure.ac`) is
   hardcoded to `blkid, fdisk, losetup, mount, nsenter, sfdisk, umount,
   unshare` -- `dmesg` isn't in it, and there's no per-target
   `dmesg_LDFLAGS` in `sys-utils/Makemodule.am` for a `make
   dmesg_LDFLAGS=...` override to hook into. The only lever that
   actually reaches the final link command is the *global* `$(LDFLAGS)`
   (`dmesg_LINK` in the generated `Makefile` expands to `... $(AM_LDFLAGS)
   $(LDFLAGS) -o $@`), which must be set at `./configure` time.

2. **Plain `-static` isn't enough, but `-all-static` breaks `configure`'s
   own compiler check.** Passed to libtool (which wraps every link step
   here because of the `libcommon.la`/`libtcolors.la` convenience
   libraries), bare `-static` only controls whether libtool prefers
   static `.la` archives -- it does **not** force `libc` itself static,
   so the binary still comes out dynamically linked against
   `libc.so.6`. The actual "make everything static" libtool flag is
   `-all-static` -- but `configure`'s own `AC_PROG_CC` sanity-compile
   invokes the compiler directly (not through libtool), and raw
   `gcc -all-static` fails with "C compiler cannot create executables",
   aborting configure before it gets anywhere. Fix: configure with
   `LDFLAGS="-static -no-pie"` (which configure's compiler check
   accepts), then hand-edit the *generated* `Makefile`'s
   `LDFLAGS = -static -no-pie` line to `LDFLAGS = -all-static -no-pie`
   before running `make dmesg`:

   ```sh
   sed -i 's/^LDFLAGS = -static -no-pie/LDFLAGS = -all-static -no-pie/' Makefile
   make dmesg -j$(nproc)
   ```

`-no-pie` avoids landing in glibc's static-PIE mode, which (like the
`nano` static-NSS issue documented in `tools/nano/README.md`) has its
own set of gotchas -- unnecessary here since `dmesg` never calls
`getpwnam()`/`getgrnam()`/any other NSS-backed function, so there's no
static-NSS crash risk either way, but a plain static ET_EXEC binary is
the simplest, most predictable output.

```sh
arm-linux-gnueabihf-strip -o dmesg dmesg
```

## Usage

```
/ # dmesg --color=always | less
/ # dmesg --color=always -w   # follow new kernel log lines live
```

`/etc/profile` aliases bare `dmesg` to `/usr/bin/dmesg --color=always`
(see `firmware_overlay/prado/etc/profile`) so a plain `dmesg` at the
shell gets color by default. Deliberately no `-T` (ctime/absolute-date
timestamps) — this device has no RTC/NTP sync, so `-T` shows a
meaningless boot-epoch date instead of anything useful; the default
`[seconds.microseconds]` since-boot format is what's actually
informative here. Also no `-x` (facility:level text column) — color
alone already distinguishes severity, the extra column was just noise.
Use the absolute path (`/usr/bin/dmesg` or BusyBox's `busybox dmesg`) to
bypass the alias, or pass `-T`/`-x` explicitly if you want them back for
a one-off.
