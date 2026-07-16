# less

Stock GNU less 668, cross-compiled fully static for the target (ARM
EABI5, armhf). Busybox on this rootfs only provides a bare `more`
(no backward scrolling, no search) — `less` is a proper pager for
paging through long `dmesg`/log output at the shell.

Linked against the same static `ncurses` build used by `tools/nano`
(see `linux-arkmicro/buildroot-external/arm-static-libs/README.md`),
with compiled-in terminfo fallbacks so it needs no terminfo database
on the target.

## Build

```sh
tar xzf less-668.tar.gz && cd less-668
NCURSES_PREFIX=/path/to/arm-static-libs/ncurses-install
./configure --host=arm-linux-gnueabihf --build=x86_64-linux-gnu \
  --prefix=/tmp/less-install \
  CPPFLAGS="-I${NCURSES_PREFIX}/include -I${NCURSES_PREFIX}/include/ncurses" \
  LDFLAGS="-L${NCURSES_PREFIX}/lib -static" \
  LIBS="-lncurses"
make -j$(nproc)
arm-linux-gnueabihf-strip -o less less
```

## Usage

```
/ # dmesg | less
/ # less /msnprofile/MsnProductInfo.ini
```

Standard less keybindings (`/` search, `q` quit, arrow keys/`space`
to scroll).
