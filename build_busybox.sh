#!/bin/bash
# Rebuild firmware_overlay/bin/busybox (+ busybox-applets.manifest) from
# real source, for the ARK1668 target (arm-linux-gnueabihf, kernel 4.19).
#
# Why this exists / history (see project docs for the full story):
#   - Stock shipped a 1.25.0 binary whose original build config is
#     unknown/unrecoverable. Rebuilt from 1.30.1 source 2026-07-27
#     (commit dc777c5) for ipcs/ipcrm -- hardware-confirmed working,
#     including as the /sbin/init replacement.
#   - 1.30.1 predates CONFIG_FEATURE_MDEV_DAEMON entirely (added in
#     1.31+, confirmed by grepping that version's own util-linux/mdev.c
#     -- no daemon code path exists at all, not a disabled option).
#     Needed for real mdev hotplug reaction (this kernel's legacy
#     CONFIG_UEVENT_HELPER is compiled out; mdev -df's netlink listener
#     is a separate mechanism that works regardless, gated only on
#     CONFIG_NET).
#   - First 1.33.0 rebuild attempt (2026-08-04, commit 198b721) was
#     dynamically linked against the HOST cross-toolchain's glibc 2.36.
#     PANICKED THE BOARD ON BOOT ("Attempted to kill init!", GLIBC_2.29/
#     2.34 not found) -- the device's own /lib/libc.so.6 is 2.27, far
#     older. Reverted same day (a5021ec).
#   - Linking against the device's own libc-2.27.so at build time (the
#     tools/hx170-test recipe) does NOT generalize to a whole program
#     like busybox: that old glibc only exports the __xstat-style
#     wrapper interface for stat/fstat/etc, not the stat64/lstat64/
#     fcntl64 symbols modern glibc headers compile calls to reference --
#     a header/ABI mismatch, not just a missing symbol. Fails with
#     undefined references to *64 functions used all over busybox's
#     file-utility applets.
#   - REAL FIX (commit 638505a): static link (no runtime libc dependency
#     at all) + this project's nss-stub pattern (see
#     tools/nss-stub/README.md) for the handful of real glibc NSS/dlopen
#     symbols busybox still references. Busybox's own
#     CONFIG_USE_BB_PWD_GRP=y already provides getpwnam/getpwuid/
#     getgrnam/getgrgid/getpwent/etc without ever touching glibc's NSS
#     machinery -- do NOT wrap those, it collides with busybox's own
#     definitions ("multiple definition of __wrap_getpwent"). The real
#     exposure is host/service resolution (getaddrinfo/gethostbyname/
#     gethostbyaddr/getservbyname/getservbyport -- used by ipcalc/
#     netstat/inetd, confirmed via this build's own linker warnings)
#     plus dlopen defensively. See tools/nss-stub/nss_stub_busybox.c.
#   - Build-system trap: busybox's scripts/Makefile.lib filters "-Wl,%"
#     tokens out of CONFIG_EXTRA_LDFLAGS for intermediate `ld -r`
#     built-in.o targets, but NOT plain arguments -- a bare object-file
#     path gets linked into both an intermediate built-in.o and the
#     final binary, producing "multiple definition" errors that look
#     like a wrap collision but are actually just double-linking. This
#     script avoids it by folding the stub .o path into the SAME
#     "-Wl,--wrap=...,<path>" token as the wrap flags.
#
# Requires: arm-linux-gnueabihf-gcc (apt install gcc-arm-linux-gnueabihf
# on Debian/Ubuntu), and a busybox source tarball (see BUSYBOX_TARBALL
# below -- not vendored in this repo).

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUSYBOX_VERSION="1.33.0"
CROSS_COMPILE="arm-linux-gnueabihf-"
OVERLAY_DIR="$SCRIPT_DIR/firmware_overlay"
NSS_STUB_SRC="$SCRIPT_DIR/tools/nss-stub/nss_stub_busybox.c"
BUILD_DIR="$(mktemp -d)"
trap 'rm -rf "$BUILD_DIR"' EXIT

echo "==> Build dir: $BUILD_DIR"

command -v "${CROSS_COMPILE}gcc" >/dev/null 2>&1 || {
    echo "ERROR: ${CROSS_COMPILE}gcc not found. Install gcc-arm-linux-gnueabihf." >&2
    exit 1
}

# Needed to run the freshly-built ARM binary on this (x86) host, both to
# regenerate the applet manifest (--list-full) and to self-verify the
# build below. Not installable via a plain `apt-get install` without
# root on every machine -- `apt-get download` + `dpkg-deb -x` works
# without root if it isn't already present.
QEMU_ARM_STATIC="${QEMU_ARM_STATIC:-$(command -v qemu-arm-static || true)}"
if [[ -z "$QEMU_ARM_STATIC" ]]; then
    echo "==> qemu-arm-static not found in PATH, fetching without root..."
    (
        cd "$BUILD_DIR"
        apt-get download qemu-user-static >/dev/null 2>&1 || {
            echo "ERROR: qemu-arm-static not found and couldn't be fetched." >&2
            echo "       Install it (apt-get install qemu-user-static) or set" >&2
            echo "       QEMU_ARM_STATIC=/path/to/it and re-run." >&2
            exit 1
        }
        dpkg-deb -x qemu-user-static_*.deb extract
    )
    QEMU_ARM_STATIC="$BUILD_DIR/extract/usr/bin/qemu-arm-static"
fi
[[ -x "$QEMU_ARM_STATIC" ]] || { echo "ERROR: $QEMU_ARM_STATIC not executable" >&2; exit 1; }
echo "==> Using qemu-arm-static: $QEMU_ARM_STATIC"
[[ -f "$NSS_STUB_SRC" ]] || {
    echo "ERROR: $NSS_STUB_SRC not found -- needed to avoid the static" >&2
    echo "       glibc NSS/dlopen startup crash, see tools/nss-stub/README.md" >&2
    exit 1
}

# Locate a busybox-$BUSYBOX_VERSION.tar.bz2 source tarball. Not vendored in
# this repo -- these paths are where past rebuilds found it (buildroot's
# own download cache, from unrelated buildroot trees checked out next to
# this repo). Override with BUSYBOX_TARBALL=/path/to/it if yours differs.
if [[ -z "$BUSYBOX_TARBALL" ]]; then
    for candidate in \
        "$HOME/Downloads/linux-arkmicro/buildroot-2021.02.2/dl/busybox/busybox-$BUSYBOX_VERSION.tar.bz2" \
        "$HOME/Downloads/linux-arkmicro/buildroot/dl/busybox/busybox-$BUSYBOX_VERSION.tar.bz2" \
        "$SCRIPT_DIR/busybox-$BUSYBOX_VERSION.tar.bz2"; do
        [[ -f "$candidate" ]] && BUSYBOX_TARBALL="$candidate" && break
    done
fi
[[ -n "$BUSYBOX_TARBALL" && -f "$BUSYBOX_TARBALL" ]] || {
    echo "ERROR: busybox-$BUSYBOX_VERSION.tar.bz2 not found. Download it from" >&2
    echo "       https://busybox.net/downloads/busybox-$BUSYBOX_VERSION.tar.bz2" >&2
    echo "       and re-run with BUSYBOX_TARBALL=/path/to/it $0" >&2
    exit 1
}
echo "==> Source: $BUSYBOX_TARBALL"

echo "==> Extracting..."
tar xjf "$BUSYBOX_TARBALL" -C "$BUILD_DIR"
SRC_DIR="$BUILD_DIR/busybox-$BUSYBOX_VERSION"
cd "$SRC_DIR"

echo "==> make defconfig..."
make ARCH=arm CROSS_COMPILE="$CROSS_COMPILE" defconfig >/dev/null

echo "==> Compiling nss stub ($NSS_STUB_SRC)..."
"${CROSS_COMPILE}gcc" -c -O2 -o "$BUILD_DIR/nss_stub_busybox.o" "$NSS_STUB_SRC"

# Static (CONFIG_STATIC=y): no runtime libc version dependency at all --
# see the history comment above for why a dynamic link or a device-libc
# swap both fail here.
sed -i 's|# CONFIG_STATIC is not set|CONFIG_STATIC=y|' .config

# --wrap the real glibc NSS/dlopen symbols busybox still references
# (NOT getpwnam/getpwuid/getgrnam/getgrgid/etc -- CONFIG_USE_BB_PWD_GRP=y
# already replaces those). The stub .o path MUST be part of this same
# "-Wl,...," token -- see the build-system-trap comment above.
WRAP_SYMS="getaddrinfo,--wrap=freeaddrinfo,--wrap=gai_strerror,--wrap=gethostbyname,--wrap=gethostbyname2,--wrap=gethostbyaddr,--wrap=getservbyname,--wrap=getservbyport,--wrap=dlopen,--wrap=dlerror,--wrap=dlsym,--wrap=dlclose"
EXTRA_LDFLAGS="-Wl,--wrap=${WRAP_SYMS},$BUILD_DIR/nss_stub_busybox.o"
sed -i "s|CONFIG_EXTRA_LDFLAGS=\".*\"|CONFIG_EXTRA_LDFLAGS=\"$EXTRA_LDFLAGS\"|" .config

echo "==> Resolving config (oldconfig)..."
make ARCH=arm CROSS_COMPILE="$CROSS_COMPILE" oldconfig < /dev/null >/dev/null 2>&1
grep -q "^CONFIG_STATIC=y" .config || { echo "ERROR: CONFIG_STATIC didn't stick" >&2; exit 1; }
grep -q "^CONFIG_FEATURE_MDEV_DAEMON=y" .config || echo "WARNING: CONFIG_FEATURE_MDEV_DAEMON not set -- mdev -df won't work"

echo "==> Building (this takes a few minutes)..."
make ARCH=arm CROSS_COMPILE="$CROSS_COMPILE" -j"$(nproc)"

echo "==> Stripping..."
"${CROSS_COMPILE}strip" -o "$BUILD_DIR/busybox.stripped" busybox_unstripped

file "$BUILD_DIR/busybox.stripped" | grep -q "statically linked" || {
    echo "ERROR: output binary is not statically linked -- refusing to deploy" >&2
    exit 1
}

echo "==> Deploying to $OVERLAY_DIR/bin/busybox..."
cp "$BUILD_DIR/busybox.stripped" "$OVERLAY_DIR/bin/busybox"
chmod 755 "$OVERLAY_DIR/bin/busybox"

echo "==> Regenerating busybox-applets.manifest..."
# firmware_overlay lives on a VirtualBox shared folder (vboxsf), which
# cannot create real symlinks -- these are materialized for real onto the
# rootfs image at build time by build_bootable_sdcard.sh's
# install_busybox_applets() step. Format: "path target", target is the
# relative-symlink path from path's own directory back to bin/busybox.
# dmesg/less are deliberately excluded -- this project ships better
# standalone replacements for both (tools/dmesg, real GNU less) at the
# same $PATH position; a busybox-provided symlink would shadow them.
"$QEMU_ARM_STATIC" ./busybox_unstripped --list-full | grep -v -e '^bin/dmesg$' -e '^usr/bin/less$' | while read -r path; do
    depth=$(grep -o '/' <<<"$path" | wc -l)
    target="bin/busybox"
    for ((i = 0; i < depth; i++)); do target="../$target"; done
    echo "$path $target"
done | sort > "$OVERLAY_DIR/busybox-applets.manifest"

wc -l "$OVERLAY_DIR/busybox-applets.manifest"

echo "==> Self-check: mdev daemon-mode support..."
"$QEMU_ARM_STATIC" ./busybox_unstripped mdev --help 2>&1 | grep -q -- '-d.*daemon' || {
    echo "ERROR: built binary's mdev doesn't list daemon mode -- CONFIG_FEATURE_MDEV_DAEMON" >&2
    echo "       didn't take. Don't deploy this; check .config in $SRC_DIR before it's cleaned up." >&2
    exit 1
}
echo "    OK -- mdev -df is supported."

echo ""
echo "==> Done. This binary passed automated checks (static link, mdev -df"
echo "    present). It has NOT been hardware-tested -- the last time a"
echo "    busybox rebuild skipped that step (a dynamically-linked build),"
echo "    it panicked the board on boot (GLIBC version mismatch)."
echo "    Test via SD card first, NOT NAND -- a bad SD boot is trivially"
echo "    recoverable, a bad NAND flash is not."
