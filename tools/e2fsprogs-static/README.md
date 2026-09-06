# e2fsprogs (static): e2fsck, mke2fs

e2fsprogs 1.47.4's `e2fsck` and `mke2fs`, cross-compiled fully static
for the target (ARM EABI5, armhf). Neither `/sbin/e2fsck` nor
`mkfs.ext4` exist anywhere on this rootfs otherwise -- Buildroot's own
`ark1668_ft_dyn_defconfig` never selects `BR2_PACKAGE_E2FSPROGS`, and
busybox's own bundled `mke2fs`/`fsck` applets don't implement real
ext2/3/4 checking at all (`fsck` just dispatches to `fsck.<type>`
binaries busybox doesn't provide; its own `mke2fs`/`mkfs.ext2` is a
minimal from-scratch formatter, not this project). This exists
specifically for `/data` (p3, userdata)'s own resilience plan --
pre-mount `e2fsck -p`/`-y`, with `mkfs.ext4` as the last-resort
reformat-and-recover fallback if it's unrecoverably corrupt -- see the
rcS comment at the actual call site.

Installed via the same generic mechanism every other `tools/*/` binary
uses (`build_bootable_sdcard_dyn.sh`'s tools-copy loop) -- every file
in this directory lands flatly in `/usr/bin/`. `mkfs.ext4`/`fsck.ext4`
are real symlinks to `mke2fs`/`e2fsck` (not copied as separate builds):
e2fsprogs' own `mke2fs.c`/`e2fsck` derive their real ext4 behavior from
`argv[0]`'s basename (confirmed by reading `mke2fs.c` directly -- a
`mkfs.` prefix is stripped and the remainder used as the target fs
type), the same mechanism a real `mke2fs`/`fsck` install always uses --
this is not a project-specific trick. The generic copy loop dereferences
symlinks (plain `cp`, not `cp -P`), so the deployed image actually
carries 4 independent files, not shared inodes -- a small, accepted
size tradeoff for correctness/simplicity over the loop's own existing
behavior, not worth a special case.

## Why these two are safe to ship static, unlike `tune2fs`/`dumpe2fs`

This project's own `tools/nss-stub/README.md` documents a real,
hardware-confirmed crash class: any statically linked binary that so
much as *references* `getpwnam`/`getpwuid`/`getgrnam`/`getgrgid`/
`dlopen` on this toolchain (glibc >= 2.34, confirmed via this build's
own `arm-linux-gnueabihf-gcc` targeting glibc 2.36) crashes at process
startup regardless of whether that code path is ever exercised.

Checked directly against this build's own link log, not assumed either
way: `tune2fs` and `dumpe2fs` DO reference `getpwnam`/`getgrnam`/
`getpwuid`/`getgrgid` (via `lib/e2p/ls.c`'s user/group-name display in
`-l`-style output) and are NOT included here for that reason -- they'd
need the same `--wrap=` treatment as `nano`/`htop` before they'd be
safe to run. `e2fsck` and `mke2fs` reference neither symbol at all
(confirmed via `nm <binary> | grep -E ' T (getpwnam|getpwuid|getgrnam|
getgrgid|dlopen)'` on both real built binaries -- empty on both), so
they need no `nss-stub` wrapping and are shipped as a plain `-static`
build.

## Build

```sh
curl -sL -o e2fsprogs-1.47.4.tar.xz \
  https://mirrors.edge.kernel.org/pub/linux/kernel/people/tytso/e2fsprogs/v1.47.4/e2fsprogs-1.47.4.tar.xz
tar xf e2fsprogs-1.47.4.tar.xz && cd e2fsprogs-1.47.4
mkdir build-arm && cd build-arm

../configure --host=arm-linux-gnueabihf --build=x86_64-linux-gnu \
  --prefix=/tmp/e2fsprogs-install \
  --disable-nls --disable-rpath --disable-uuidd \
  --enable-libuuid --enable-libblkid --disable-fuse2fs \
  LDFLAGS="-static"

make -j"$(nproc)"

arm-linux-gnueabihf-strip -o e2fsck ./e2fsck/e2fsck
arm-linux-gnueabihf-strip -o mke2fs ./misc/mke2fs
```

`--enable-libuuid --enable-libblkid` are load-bearing, not optional --
without them `configure` fails outright (`external uuid library not
found`): there is no system `libuuid`/`libblkid` for this ARM target,
and the flag names are inverted from what they sound like -- they mean
"build and use e2fsprogs' OWN bundled uuid/blkid" (its private
`lib/uuid`/`lib/blkid` trees, already vendored in the source release),
not "use an external one." No other flags/patches were needed --
unlike `tools/dmesg`'s `util-linux` build (which needed a hand-edited
`Makefile` to force `-all-static` past its own `configure`-time
compiler check), e2fsprogs' `LDFLAGS="-static"` alone produced a real,
fully static ARM ELF binary for both targets on the first attempt --
confirmed via `file` (`statically linked`) and a real link-time
`checking whether we can link with -static... yes` in `configure`'s
own output, not just assumed from the flag being passed.

## Verification

Confirmed without patching a single line of upstream source (a plain
cross-compile of the real 1.47.4 release):
- `file e2fsck mke2fs`: both genuine `ELF 32-bit LSB executable, ARM,
  EABI5 ... statically linked`.
- `nm` on both: zero `getpwnam`/`getpwuid`/`getgrnam`/`getgrgid`/
  `dlopen` symbols defined -- see the nss-stub section above.

**Not yet verified**: no `qemu-user-static` (or similar ARM emulation)
was available in the environment this was built in, so neither binary
has actually been *run* yet, only statically inspected. Real
functional verification (format a scratch loopback image, deliberately
corrupt it, confirm `e2fsck -p`/`-y` and the `mkfs.ext4` reformat
fallback both behave as expected) still needs to happen on real
hardware or under real ARM emulation before relying on this for the
`/data` resilience plan in production.
