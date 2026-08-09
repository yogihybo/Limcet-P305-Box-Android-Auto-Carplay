#!/bin/bash
# Cross-compile OpenSSL 1.1.1 (static) for aasdk's find_package(OpenSSL
# REQUIRED). Not vendored as a submodule/binary blob -- reproducible
# script instead, matching this repo's build_*.sh convention.
#
# Version note: the target device ships libssl.so.1.1/libcrypto.so.1.1
# (OpenSSL 1.1.x). We're NOT trying to link against those shared libs
# or match their exact patch version -- we're statically linking our
# own freshly-built copy, so the target's shipped OpenSSL is irrelevant
# here (no ABI/wire compatibility concern the way there would be for,
# say, a protocol schema version). Picked 1.1.1w (the final 1.1.1
# release) mainly to stay on the same major.minor as what's already
# proven to run on this hardware, not out of technical necessity.
#
# Static linking is load-bearing, not a style choice -- same
# host-toolchain-vs-target-glibc (2.27) mismatch reasoning as
# build_boost.sh and custom_ui/Makefile's own -static.

set -e

OPENSSL_VERSION="1.1.1w"
BUILD_DIR="${OPENSSL_BUILD_DIR:-$HOME/build-deps}"
CROSS_COMPILE="${CROSS_COMPILE:-arm-linux-gnueabihf-}"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

if [[ ! -d "openssl-${OPENSSL_VERSION}" ]]; then
    echo "==> Downloading OpenSSL ${OPENSSL_VERSION}..."
    curl -sL -o openssl.tar.gz \
        "https://github.com/openssl/openssl/releases/download/OpenSSL_${OPENSSL_VERSION//./_}/openssl-${OPENSSL_VERSION}.tar.gz"
    tar xzf openssl.tar.gz
    rm openssl.tar.gz
fi

cd "openssl-${OPENSSL_VERSION}"

echo "==> Configuring (linux-generic32, no-shared, no-tests)..."
CC="${CROSS_COMPILE}gcc" AR="${CROSS_COMPILE}ar" RANLIB="${CROSS_COMPILE}ranlib" \
    ./Configure linux-generic32 no-shared no-tests \
    --prefix="$BUILD_DIR/openssl-arm-install"

echo "==> Building..."
make -j"$(nproc)"

echo
echo "✔ Static libs: $BUILD_DIR/openssl-${OPENSSL_VERSION}/{libssl,libcrypto}.a"
echo "✔ Headers: $BUILD_DIR/openssl-${OPENSSL_VERSION}/include/"
echo
echo "Not yet wired into custom_ui/Makefile -- lands with the actual"
echo "aasdk integration work (Phase 2, see docs/IMPLEMENTATION_PLAN.md)."
