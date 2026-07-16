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

# 2. nano, static, linked against the ncurses build above
tar xf nano-7.2.tar.xz && cd nano-7.2
./configure --host=arm-linux-gnueabihf --build=x86_64-linux-gnu \
  --prefix=/tmp/nano-install \
  --disable-nls --disable-utf8 --disable-libmagic --disable-speller \
  CPPFLAGS="-I/tmp/ncurses-install/include -I/tmp/ncurses-install/include/ncurses" \
  LDFLAGS="-L/tmp/ncurses-install/lib -static" \
  LIBS="-lncurses"
make -j$(nproc)
arm-linux-gnueabihf-strip -o nano src/nano
```

The usual glibc-static-linking warnings about `getpwent`/`getpwuid`/
`getpwnam_r` (NSS) at link time are expected and harmless here -- this
rootfs only ever does local flat-file `/etc/passwd` lookups, which
glibc's statically-linked "files" backend handles fine; there's no NSS
module loading involved.

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
