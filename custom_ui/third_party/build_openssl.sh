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

# ---------------------------------------------------------------------------
# no-engine rebuild -- what custom_ui/Makefile's OPENSSL_ARM_INSTALL
# actually points at. 2026-08-15: same 1.1.1w source, reconfigured
# no-engine/no-dso/no-afalgeng/no-sock -- the original linux-generic32
# build above links libcrypto.a with dlopen/getaddrinfo/gethostbyname
# references (engine loading, BIO_sock), one of two confirmed sources of
# a glibc static-NSS-init startup crash (see custom_ui/src/androidauto/
# hantro_dlopen.c's header comment).
#
# 2026-08-21: ALSO switched target from linux-generic32 to linux-armv4
# (with -march=armv7-a -mfpu=neon -D__ARM_MAX_ARCH__=8). generic32 maps
# to OpenSSL's plain-C AES/GHASH fallback regardless of this being a
# real ARM target (Configurations/10-main.conf: modes_asm_src => "" for
# generic32) -- confirmed real cause of an 80%+ CPU spike on every AA
# full-screen video refresh, since every aasdk channel (video included)
# is wrapped in real TLS (AES-128-GCM) and a big I-frame means a burst
# of SSL_read() calls each paying the generic-C cost. This Cortex-A5 has
# no ARMv8 Crypto Extensions (no hardware AES/GHASH at all -- not a
# config gap), but does have NEON; linux-armv4 pulls in real NEON
# bit-sliced AES (bsaes-armv7.S) and NEON GHASH (ghash-armv4.S) with
# runtime CPU-capability dispatch (OPENSSL_armcap) instead.
echo
echo "==> Configuring no-engine rebuild (linux-armv4, NEON AES/GHASH)..."
cd ..
if [[ ! -d "openssl-${OPENSSL_VERSION}-noengine-src" ]]; then
    tar xzf "$BUILD_DIR/openssl.tar.gz" 2>/dev/null || \
        curl -sL -o openssl.tar.gz \
            "https://github.com/openssl/openssl/releases/download/OpenSSL_${OPENSSL_VERSION//./_}/openssl-${OPENSSL_VERSION}.tar.gz" \
        && tar xzf openssl.tar.gz && rm openssl.tar.gz
    mv "openssl-${OPENSSL_VERSION}" "openssl-${OPENSSL_VERSION}-noengine-src"
fi
cd "openssl-${OPENSSL_VERSION}-noengine-src"

CC="${CROSS_COMPILE}gcc" AR="${CROSS_COMPILE}ar" RANLIB="${CROSS_COMPILE}ranlib" \
    ./Configure linux-armv4 -march=armv7-a -mfpu=neon -D__ARM_MAX_ARCH__=8 \
    no-shared no-tests no-engine no-dso no-afalgeng no-sock \
    --prefix="$BUILD_DIR/openssl-arm-install-noengine-armv7"

echo "==> Building..."
make -j"$(nproc)"

echo "==> Installing..."
make install_sw

echo
echo "✔ No-engine, NEON-accelerated static libs installed to:"
echo "  $BUILD_DIR/openssl-arm-install-noengine-armv7/lib/{libssl,libcrypto}.a"
echo
echo "Verify: nm \$BUILD_DIR/openssl-arm-install-noengine-armv7/lib/libcrypto.a |"
echo "  grep 'T bsaes_cbc_encrypt\\|T gcm_ghash_neon' -- should show real defined symbols."
echo "custom_ui/Makefile's OPENSSL_ARM_INSTALL already points here by default."
