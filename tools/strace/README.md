# strace

Upstream [strace](https://strace.io/) v7.1, cross-compiled statically for
this board's target (`arm-linux-gnueabihf`, glibc, kernel 4.19). Not a
custom tool like the others in `tools/` — this is vanilla strace, built
because the target rootfs has none and this project needed exact
syscall-level tracing to pin down the `MsnCoreApp` segfault (see
`docs/ARK1680_TS_REVERSE_ENGINEERING.md` → "`MsnCoreApp` segfault").

## Build

Source: [github.com/strace/strace releases](https://github.com/strace/strace/releases)
(the release tarball, not the git repo — it ships a pre-generated
`configure` so no autoconf/automake needed on the build host).

```
curl -sL -o strace.tar.xz https://github.com/strace/strace/releases/download/v7.1/strace-7.1.tar.xz
tar xf strace.tar.xz
cd strace-7.1
CC=arm-linux-gnueabihf-gcc ./configure --host=arm-linux-gnueabihf LDFLAGS=-static
make -j$(nproc)
arm-linux-gnueabihf-strip -o /path/to/tools/strace/strace src/strace
```

(`--enable-static`/`--disable-shared` are not recognized by strace's
`configure` — `LDFLAGS=-static` alone is what produces the static
binary; confirmed via `file` that the result has no dynamic
interpreter.)

## Usage

Same as any strace, once copied onto the target:

```sh
/ # strace -f -o /data/msncoreapp.strace.log start_msn
# or attach output straight to the console:
/ # strace -f MsnCoreApp -qws
```

For the segfault investigation specifically, `-f` (follow forks/threads)
matters since Qt/QWS spawns additional threads early — trace the exact
sequence of syscalls up to the `SIGSEGV`, then cross-reference the last
successful call/address against the symbol table in the unstripped
`Prado firmware dump/mtd6_rootfs/usr/bin/MsnCoreApp` the same way the
rest of the crash analysis in `docs/ARK1680_TS_REVERSE_ENGINEERING.md`
was done (`arm-linux-gnueabihf-objdump -d -C`).

This is a much more direct route to the exact crash point than the
`user_debug=8` kernel bootarg approach also added this session — that
one only shows the faulting *address*; `strace` shows the *syscall
sequence* leading up to it, which is usually enough to identify the
function without any disassembly at all.
