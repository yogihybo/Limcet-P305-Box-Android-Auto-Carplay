# nano

Stock GNU nano 7.2, cross-compiled fully static for the target (ARM
EABI5, armhf) so it drops straight into `/usr/bin` alongside this
project's other diagnostic tools with no shared-library dependencies to
worry about.

## Why static, and why it needed a custom ncurses build

`nano` needs curses. A normal static build against a normal ncurses
would still fail at runtime on this rootfs, because ncurses looks up
terminal capabilities from a terminfo database under
`/usr/share/terminfo` -- which this rootfs doesn't have. `/etc/inittab`
sets the serial console up as `getty -L ttyS0 115200 vt100`, i.e.
`TERM=vt100`.

So `ncurses` was built with `--with-fallbacks=linux,vt100,vt102,xterm,
xterm-color,ansi` -- this compiles those terminal descriptions directly
into `libncurses.a`, so `nano` works correctly with `TERM=vt100` (or any
of the others) even with zero terminfo files present on the target.

## Build

Not part of the normal `tools/*.c` static-gcc pattern -- this one needs
a full ncurses build first. Reproduce with:

```sh
# 1. ncurses, static, with compiled-in terminfo fallbacks (no /usr/share/terminfo needed on target)
tar xzf ncurses-6.1.tar.gz && cd ncurses-6.1
./configure --host=arm-linux-gnueabihf --build=x86_64-linux-gnu \
  --prefix=/tmp/ncurses-install \
  --without-shared --with-normal --without-debug --without-cxx --without-cxx-binding \
  --without-ada --without-tests --without-manpages --without-progs \
  --disable-db-install --enable-termcap \
  --with-fallbacks=linux,vt100,vt102,xterm,xterm-color,ansi \
  --with-terminfo-dirs=/usr/share/terminfo
make -j$(nproc) && make install

# 2. nano, static, linked against the ncurses build above, with NSS calls
#    stubbed out -- see tools/nss-stub/README.md for why this is required,
#    not optional (confirmed on real hardware, see below)
arm-linux-gnueabihf-gcc -c -O2 -o nss_stub.o ../nss-stub/nss_stub.c
tar xf nano-7.2.tar.xz && cd nano-7.2
WRAP="-Wl,--wrap=getpwnam,--wrap=getpwuid,--wrap=getpwnam_r,--wrap=getpwuid_r,--wrap=getpwent,--wrap=setpwent,--wrap=endpwent"
./configure --host=arm-linux-gnueabihf --build=x86_64-linux-gnu \
  --prefix=/tmp/nano-install \
  --disable-nls --disable-utf8 --disable-libmagic --disable-speller \
  CPPFLAGS="-I/tmp/ncurses-install/include -I/tmp/ncurses-install/include/ncurses" \
  LDFLAGS="-L/tmp/ncurses-install/lib -static $WRAP ../nss_stub.o" \
  LIBS="-lncurses"
make -j$(nproc)
arm-linux-gnueabihf-strip -o nano src/nano
```

**Corrected 2026-07-16 -- this NSS stubbing is required, not just
noise-reduction.** An earlier version of this README claimed the
glibc-static-linking warnings about `getpwent`/`getpwuid`/`getpwnam_r`
at link time ("Using 'X' in statically linked applications requires...")
were harmless. **That was wrong, confirmed on real hardware**: without
the `--wrap` treatment above, `nano` crashed on *every* invocation with
`dl-call-libc-early-init.c:37: _dl_call_libc_early_init: Assertion
'sym != NULL' failed` -- glibc >= 2.34's dlopen-based static NSS
machinery genuinely doesn't work on this toolchain/target combination.
See `tools/nss-stub/README.md` for the full root cause and why merely
not *calling* these functions isn't enough (the crash happens at
process startup regardless of whether the code path is ever
exercised -- it's about what gets linked in, not what runs).

`--disable-utf8` and `--disable-speller` keep the build simpler/smaller
since this rootfs has no locale/spell-check data anyway; nothing here
needs them for basic config/log/script editing at the shell.

## Usage

```
/ # nano /msnprofile/MsnProductInfo.ini
```

Standard nano keybindings (`^O` save, `^X` exit, `^W` search, etc.) --
the on-screen shortcut bar at the bottom is the built-in help, no man
page needed (none was built).
