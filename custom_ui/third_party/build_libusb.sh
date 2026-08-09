#!/bin/bash
# Cross-compile libusb 1.0 (static) for aasdk's find_package(libusb-1.0
# REQUIRED) -- it talks to the phone/head unit over real USB, so this
# is a genuine runtime dependency, not just a build-time one.
#
# Not vendored as a git submodule -- fetched from the RELEASE TARBALL
# (not a git clone) specifically because release tarballs ship a
# pre-generated `configure` script. This build host has no root access
# (no `apt-get install autoconf/automake/libtool`) and a git checkout
# of libusb only ships autogen.sh, which needs autoreconf to produce
# `configure` -- the tarball route sidesteps that entirely.
#
# Static linking is load-bearing, not a style choice -- same
# host-toolchain-vs-target-glibc (2.27) mismatch reasoning as
# build_boost.sh / build_openssl.sh and custom_ui/Makefile's own
# -static.
#
# --disable-udev: configure fails without it ("udev support requested
# but libudev header not installed" -- no libudev-dev on this build
# host, no root to install one). This does NOT remove hotplug support:
# libusb falls back to its Linux-netlink-based hotplug backend
# (os/linux_netlink.lo) when udev isn't available, which still compiles
# in fine.

set -e

LIBUSB_VERSION="1.0.29"
BUILD_DIR="${LIBUSB_BUILD_DIR:-$HOME/build-deps}"
CROSS_COMPILE="${CROSS_COMPILE:-arm-linux-gnueabihf-}"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

if [[ ! -d "libusb-${LIBUSB_VERSION}" ]]; then
    echo "==> Downloading libusb ${LIBUSB_VERSION} (release tarball, has pre-generated configure)..."
    curl -sL -o libusb.tar.bz2 \
        "https://github.com/libusb/libusb/releases/download/v${LIBUSB_VERSION}/libusb-${LIBUSB_VERSION}.tar.bz2"
    tar xjf libusb.tar.bz2
    rm libusb.tar.bz2
fi

cd "libusb-${LIBUSB_VERSION}"

echo "==> Configuring (static only, no udev -- netlink hotplug backend used instead)..."
CC="${CROSS_COMPILE}gcc" \
    ./configure --host=arm-linux-gnueabihf \
    --enable-static --disable-shared --disable-udev \
    --prefix="$BUILD_DIR/libusb-arm-install"

echo "==> Building..."
make -j"$(nproc)"

echo
echo "✔ Static lib: $BUILD_DIR/libusb-${LIBUSB_VERSION}/libusb/.libs/libusb-1.0.a"
echo "✔ Headers: $BUILD_DIR/libusb-${LIBUSB_VERSION}/libusb/libusb.h"
echo
echo "Not yet wired into custom_ui/Makefile -- lands with the actual"
echo "aasdk integration work (Phase 2, see docs/IMPLEMENTATION_PLAN.md)."
