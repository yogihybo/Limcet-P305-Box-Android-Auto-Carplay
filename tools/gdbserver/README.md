# gdbserver

GDB 13.2's `gdbserver`, cross-compiled fully static for the target
(ARM EABI5, armhf). Lets a `gdb` running on a host machine attach to a
live process on the device (or launch one directly) over TCP and get
**real** register/stack/memory state -- directly replacing the
disassemble-the-binary-and-infer-what-the-stack-must-contain approach
this project has relied on throughout `AUDIO_SUBSYSTEM_INVESTIGATION.md`
(e.g. the `sendSoundData()` uninitialized-stack crash, root-caused via
static analysis + a minidump register dump, when a live `gdbserver`
session would have shown the actual stack contents directly).

## Build

Needs `arm-linux-gnueabihf-g++` (the C++ cross compiler) -- not
installed by default alongside this system's C-only
`arm-linux-gnueabihf-gcc`:

```sh
sudo apt-get install -y g++-arm-linux-gnueabihf
```

`gdbserver` is built from the `gdbserver/` subdirectory of the full GDB
source tree, but needs `gnulib` (a support library shared across the
binutils-gdb tree) built first. Two build-system quirks hit during this
build, both worked around below:

1. **`__NR_sigreturn` undeclared** (`linux-arm-low.cc`): this is an ARM
   OABI-only syscall number, absent from modern EABI kernel headers.
   The code path that uses it only detects a legacy OABI
   signal-trampoline PC pattern that doesn't apply on this EABI target
   anyway, so defining a fallback value is safe.
2. **`AR = ar` (host archiver) picked up for `gdbsupport`**, instead of
   the cross `ar` -- silently produces an x86-64 object archive that the
   cross linker then correctly refuses to read ("archive has no
   index"). Force `AR`/`RANLIB` explicitly on the `make` command line.

```sh
tar xf gdb-13.2.tar.xz
mkdir gdb-build && cd gdb-build
../gdb-13.2/configure --host=arm-linux-gnueabihf --build=x86_64-linux-gnu \
  --prefix=/tmp/gdbserver-install \
  --disable-gdb --disable-sim --disable-binutils --disable-gas --disable-ld \
  --disable-gprof \
  CFLAGS="-g -O2 -D__NR_sigreturn=119" CXXFLAGS="-g -O2 -D__NR_sigreturn=119" \
  LDFLAGS="-static"

make -j$(nproc) all-gnulib
make -j$(nproc) all-gdbserver AR=arm-linux-gnueabihf-ar RANLIB=arm-linux-gnueabihf-ranlib

arm-linux-gnueabihf-strip -o gdbserver gdbserver/gdbserver
```

## Usage

On the target, either launch a program directly under gdbserver:

```
/ # gdbserver :2345 /usr/lib/libMsnSound.so-loading-app-or-whatever-cmd
```

or attach to something already running (e.g. to catch the
`sendSoundData()` crash live instead of from a post-mortem minidump):

```
/ # gdbserver :2345 --attach $(pidof MsnCoreApp)
```

On the host, connect with an ARM-aware gdb (this system doesn't have
one installed by default -- `sudo apt-get install -y gdb-multiarch`):

```sh
gdb-multiarch /path/to/local/copy/of/libSetting.so   # or the binary being debugged, for symbols
(gdb) set sysroot /path/to/local/copy/of/rootfs       # optional, helps resolve shared libs
(gdb) target remote <device-ip>:2345
(gdb) continue
```

Once connected, standard gdb commands apply: `bt` for a real
backtrace, `info registers`, `x/20xw $sp` to actually see what's on the
stack (rather than inferring it from a register dump + disassembly),
breakpoints, single-stepping, etc.
