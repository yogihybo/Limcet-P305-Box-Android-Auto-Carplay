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
arm-linux-gnueabihf-gcc -c -O2 -o /tmp/nss_stub.o ../nss-stub/nss_stub.c
WRAP="-Wl,--wrap=getpwnam,--wrap=getpwuid,--wrap=getpwnam_r,--wrap=getpwuid_r,--wrap=getpwent,--wrap=setpwent,--wrap=endpwent,--wrap=getgrgid,--wrap=getgrgid_r,--wrap=dlopen,--wrap=dlerror,--wrap=dlsym,--wrap=dlclose"
./configure --host=arm-linux-gnueabihf --build=x86_64-linux-gnu \
  --prefix=/tmp/htop-install \
  --disable-unicode \
  CPPFLAGS="-I${NCURSES_PREFIX}/include -I${NCURSES_PREFIX}/include/ncurses" \
  LDFLAGS="-L${NCURSES_PREFIX}/lib -static $WRAP /tmp/nss_stub.o" \
  LIBS="-lncurses"
make -j$(nproc)
arm-linux-gnueabihf-strip -o htop htop
```

**Corrected 2026-07-17 -- the `dlopen`/`getpwnam`/`getpwuid`
glibc-static-linking warnings at link time are NOT harmless.** An
earlier version of this README claimed they were (same wrong
assumption `tools/nano/README.md` made before it was corrected on
2026-07-16). Without the `--wrap` treatment above, this binary
crashed with `A signal 6 (Aborted) was received` on real hardware --
glibc >= 2.34's dlopen-based static NSS machinery genuinely doesn't
work on this toolchain/target combination, regardless of whether the
lookup code path is ever exercised at runtime (it's about what gets
*linked in*, not what runs). See `tools/nss-stub/README.md` for the
full root cause. htop needs the fuller symbol list nano didn't
(`getgrgid`/`getgrgid_r` for the process-owner group column, and
`dlopen`/`dlerror`/`dlsym`/`dlclose` for the optional systemd meter)
-- check a fresh build's log for "Using 'X' in statically linked
applications requires..." warnings to confirm the wrap list is
complete.

With the stub in place, `getpwuid`/`getgrgid` lookups always return
NULL, so the process list shows raw numeric UID/GID instead of
resolved names -- expected and harmless, since this rootfs's user
database was never actually being consulted successfully anyway.

**Confirmed fixed on real hardware (2026-07-17)**: crashed with `A
signal 6 (Aborted) was received` on every invocation before the
`--wrap` treatment, runs correctly after.

## Usage

```
/ # htop
```

Standard htop keybindings (`F10`/`q` quit, arrow keys to navigate,
`F9` to send a signal to the selected process).

`htop-debug` alongside it is the same source/config but unstripped
with `-g -O0 -rdynamic` -- kept in case of a future crash, since a
stripped binary's crash screen has no symbols to resolve. If it ever
aborts, resolve backtrace addresses with:
```sh
arm-linux-gnueabihf-addr2line -e htop-debug -f -C -p <address>
```
