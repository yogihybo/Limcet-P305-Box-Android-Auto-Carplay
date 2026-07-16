# htop

Stock htop 3.3.0, cross-compiled fully static for the target (ARM
EABI5, armhf). Easier to read live than busybox's bare `top` --
sortable columns, per-core CPU bars, tree view.

Configured with `--disable-unicode` (this rootfs has no locale data)
and no `libnl`/`libcap`/`libsensors`/`libunwind` (none present on this
target, and none of those features matter for a plain embedded Linux
process list). Linked against the same static `ncurses` build used by
`tools/nano` (see
`linux-arkmicro/buildroot-external/arm-static-libs/README.md`).

## Build

```sh
tar xf htop-3.3.0.tar.xz && cd htop-3.3.0
NCURSES_PREFIX=/path/to/arm-static-libs/ncurses-install
./configure --host=arm-linux-gnueabihf --build=x86_64-linux-gnu \
  --prefix=/tmp/htop-install \
  --disable-unicode \
  CPPFLAGS="-I${NCURSES_PREFIX}/include -I${NCURSES_PREFIX}/include/ncurses" \
  LDFLAGS="-L${NCURSES_PREFIX}/lib -static" \
  LIBS="-lncurses"
make -j$(nproc)
arm-linux-gnueabihf-strip -o htop htop
```

The `dlopen`/`getpwnam`/`getpwuid` glibc-static-linking warnings at
link time are expected and harmless -- the (optional) systemd meter's
`dlopen` just fails gracefully at runtime since this target has no
systemd, and user lookups only ever hit this rootfs's local flat-file
`/etc/passwd`.

## Usage

```
/ # htop
```

Standard htop keybindings (`F10`/`q` quit, arrow keys to navigate,
`F9` to send a signal to the selected process).
