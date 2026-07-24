# directfb-fbdev-fix

Rebuild of `usr/lib/directfb-1.7-4/systems/libdirectfb_fbdev.so` (DirectFB's
`fbdev` *system* module — owns the primary/screen surface and mode-setting,
distinct from the closed-source `gal` *gfxdriver* module that provides GPU
acceleration) with one patch: force the primary layer's surface onto
`fb0`'s own memory instead of letting it default into whichever pool wins
DirectFB's priority-based allocation (which is GAL's GPU pool on this
board).

See `docs/DEVICE_TEST_CHECKLIST_2026-07-18.md` (DirectFB black-screen
investigation, "root cause" section) for the full trace of why this is
needed — short version: `dfb_layer_context_allocate_surface()` defaults
an unset `surface_caps` to `DSCAPS_VIDEOONLY`, which only requires
`CSTF_EXTERNAL` — a requirement both the `fbdev` pool and the GAL pool
satisfy, so GAL wins on priority (`CSPP_PREFERED` vs `fbdev`'s own
`CSPP_DEFAULT`). GAL's pool is backed by galcore's GPU memory, a
physically different DRAM region from `/dev/fb0`'s own dedicated 16MB
carveout, so the primary surface's pan/flip offset math ends up wrong —
valid ioctl, meaningless address, black/scrambled screen. Forcing
`DSCAPS_SYSTEMONLY` (which requires `CSTF_INTERNAL`, a flag GAL's pool
doesn't declare) excludes GAL from the primary surface specifically,
while leaving window/backing-store surfaces free to use GPU acceleration
normally.

## Why a source rebuild, not a binary patch

Confirmed via decompile that this board's `libdirectfb_fbdev.so` is
genuinely vanilla upstream DirectFB — not an ARK-specific fork. The only
difference found between the deployed binary's logic and real upstream
`DIRECTFB_1_7_4` source (`git tag DIRECTFB_1_7_4`,
`github.com/deniskropp/DirectFB`) is this project's own missing
`surface_caps` line. That makes a clean source patch + rebuild both
possible and preferable to editing compiled bytes directly.

## Confirming the exact deployed version

The deployed module directory is `directfb-1.7-4`. DirectFB's own
`configure.in` computes this name as
`directfb-$MAJOR.$MINOR-$(MICRO - BINARY_AGE)`. For `DIRECTFB_1_7_4`,
`MICRO=4` and `BINARY_AGE=0`, giving `directfb-1.7-4` — an exact match.
(`DIRECTFB_1_7_7`, the only release Buildroot has cached locally, would
build as `directfb-1.7-7` instead — a different, incompatible module
directory name.)

## Why building from the 1.7.7 tarball instead of the 1.7.4 git checkout

The 1.7.4 git checkout has no pre-generated `configure`/`Makefile.in`
(only `configure.in`), and this environment has no `autoconf`/
`automake`/`libtool` installed to regenerate them (no root access to
install them either). The `DirectFB-1.7.7.tar.gz` release tarball
Buildroot already has cached (`buildroot/dl/directfb/`) ships a
pre-generated build system and needs no `autoreconf`.

Diffing `systems/fbdev/fbdev.c` between the real `DIRECTFB_1_7_4` tag and
the `1.7.7` tarball shows exactly one unrelated difference (a
`buf[512]` → `buf[512+1]` off-by-one safety fix) — confirmed by literal
`diff`, not assumption. Building from 1.7.7 and applying `0001-*.patch`
there is functionally identical to patching genuine 1.7.4, and DirectFB's
own `BINARY_AGE=0` in both versions' `configure.in` confirms the
maintainers considered every 1.7.x point release ABI-stable within the
module-loading interface this fix touches.

## Reproducing the build

```sh
# 1. Get genuine DirectFB 1.7.4 source (used only to generate/verify the
#    patch above -- not built directly, see "why 1.7.7" above).
git clone --depth 1 --branch DIRECTFB_1_7_4 \
    https://github.com/deniskropp/DirectFB.git directfb-1.7.4-src

# 2. Extract the buildable 1.7.7 tarball (already cached in this repo's
#    sibling linux-arkmicro tree) and apply the patch there instead.
tar xzf /path/to/linux-arkmicro/buildroot/dl/directfb/DirectFB-1.7.7.tar.gz
cd DirectFB-1.7.7
patch -p1 < .../0001-primary-layer-force-systemonly-surface-caps.patch

# 3. Also needed: buildroot's own upstream-tarball-gap patch (unrelated
#    to this fix -- DirectFB-1.7.7.tar.gz is missing four header files
#    present in the real git tree; without it, gfxdrivers/davinci and
#    tests/voodoo fail to compile). Not needed if you skip building
#    those directories (see --with-gfxdrivers=none below).
patch -p1 < /path/to/linux-arkmicro/buildroot/package/directfb/0001-fix-missing-davinci-voodoo-header.patch

# 4. Cross-compile with the SAME toolchain the kernel/rootfs use
#    (Linaro GCC 7.3.1-2018.05 -- matching "gcc version 7.3.1 20180425
#    [linaro-7.3-2018.05 ...]" in every boot log this project has).
export PATH=/path/to/linux-arkmicro/buildroot-external/toolchain/gcc-linaro-7.3.1-2018.05-x86_64_arm-linux-gnueabihf/bin:$PATH
export CC=arm-linux-gnueabihf-gcc CXX=arm-linux-gnueabihf-g++

./configure \
  --host=arm-linux-gnueabihf --prefix=/usr \
  --enable-fbdev --disable-sdl --disable-vnc --disable-osx \
  --disable-video4linux --disable-video4linux2 --without-tools \
  --disable-x11 --disable-multi --disable-multi-kernel \
  --disable-zlib --disable-freetype --disable-png --disable-jpeg \
  --disable-gif --disable-static \
  --with-gfxdrivers=none --with-inputdrivers=none
  # zlib/freetype/png/jpeg and every gfx/input driver are disabled
  # because we're only producing systems/fbdev/libdirectfb_fbdev.so,
  # which needs none of them -- confirmed no undefined symbols from
  # any of those libraries in the built .so (nm -D --defined-only).

make -j"$(nproc)"
```

Result: `systems/fbdev/.libs/libdirectfb_fbdev.so`.

## Post-build fixup: SONAME versions

Building from the 1.7.7 tree links against `libdirect-1.7.so.7`/
`libfusion-1.7.so.7`/`libdirectfb-1.7.so.7` (matching 1.7.7's own
soname), but the deployed rootfs only has the real `.so.4` versions
(matching genuine 1.7.4, already confirmed ABI-compatible per the
`BINARY_AGE=0` reasoning above). Both version substrings are the exact
same length (`1.7.so.7` / `1.7.so.4`, 8 bytes each), so this is a safe,
direct same-length byte replacement in the built `.so` -- no ELF
structure changes, no tool like `patchelf` needed (not available in this
environment anyway, no root to install it):

```python
path = "systems/fbdev/.libs/libdirectfb_fbdev.so"
with open(path, 'rb') as f:
    data = bytearray(f.read())
for old, new in [
    (b"libdirect-1.7.so.7\x00", b"libdirect-1.7.so.4\x00"),
    (b"libfusion-1.7.so.7\x00", b"libfusion-1.7.so.4\x00"),
    (b"libdirectfb-1.7.so.7\x00", b"libdirectfb-1.7.so.4\x00"),
]:
    assert len(old) == len(new)
    data = data.replace(old, new)
with open(path, 'wb') as f:
    f.write(data)
```

Verified afterward with `readelf -d` (`NEEDED` entries all read `.so.4`,
matching the deployed rootfs's real libraries exactly) and
`nm -D --defined-only` (exported symbol set is byte-identical, 28/28, to
the stock deployed `libdirectfb_fbdev.so` -- confirms this is a faithful
drop-in replacement, not just "close enough").

`.la` libtool files are **not** deployed alongside the `.so` -- checked
the genuine stock `.la` file's own `dlname=` field, which confirms
DirectFB loads modules via a plain `dlopen("libdirectfb_fbdev.so")` at
runtime; `.la` metadata is only consulted by other packages linking
against this library at *their* build time, not by DirectFB's own module
loader.

## Result

Deployed to `firmware_overlay/usr/lib/directfb-1.7-4/systems/libdirectfb_fbdev.so`.
Not yet hardware-tested at time of writing.
