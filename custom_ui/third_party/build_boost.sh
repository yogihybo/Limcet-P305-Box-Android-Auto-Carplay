#!/bin/bash
# Cross-compile Boost for aasdk. aasdk's own #includes/CMakeLists.txt
# COMPONENTS list only need boost_log/boost_log_setup compiled
# (everything else it uses -- system, asio, algorithm, core, endian --
# is header-only). BUT: building only those two targets and then
# running `cmake --install` fails -- Boost's CMake install() rules are
# generated for every configured library regardless of what actually
# got built, so `cmake --install` errors on the first unbuilt library's
# missing .a (e.g. libboost_charconv.a) with no per-component way to
# skip it (no COMPONENT tagging in Boost's own BoostInstall.cmake to
# filter by). So: build everything, then install everything, once.
# Slower (full Boost build) but the only option that leaves a clean,
# find_package(Boost)-discoverable install tree.
#
# Not vendored as a git submodule or committed as binary blobs -- the
# Boost source release is ~130MB, so this script (matching this repo's
# existing build_*.sh convention for external components) is the
# reproducible artifact instead. Run once before building anything
# that links aasdk.
#
# Static linking is load-bearing, not a style choice, same reasoning as
# custom_ui/Makefile's own -static: this repo's cross toolchain targets
# a newer glibc (2.28+) than the device's real runtime (2.27, confirmed
# against firmware_dumps/.../lib/libc-2.27.so) -- a dynamically-linked
# Boost .so built with this toolchain would carry the same
# GLIBC_2.28/2.33/2.34 version requirements the custom_ui binary itself
# had to avoid. No prebuilt ARMHF Boost package (Debian/Ubuntu or
# otherwise) is safe to use for the same reason -- they're all built
# against a modern host glibc, not this device's 2.27.
#
# Uses Boost's CMake build (not the traditional b2/Boost.Build
# toolset-file cross-compile route) -- Boost 1.87+ ships a real,
# maintained CMakeLists.txt that "just works" with a standard CMake
# toolchain file, confirmed 2026-08-09: `boost_system` doesn't exist as
# a build target at all (Boost.System has been header-only since 1.69,
# kept only for legacy ABI linking, so there's genuinely nothing to
# compile for it) -- only boost_log/boost_log_setup produce real
# artifacts, and building them pulls in their own transitive deps
# (date_time, chrono, atomic, container, thread, serialization,
# context, random, filesystem) automatically via normal CMake target
# dependencies, no manual dependency chasing needed.

set -e

# 2026-08-24: LINK_SHARED=1 builds real .so's instead of .a's -- for
# the dynamically-linked custom_ui rootfs (AASDK_DEPS_DIR_DYN=
# $HOME/build-deps-dyn, a fully separate tree from the static
# $HOME/build-deps default), to let this ~15-20MB of Boost/Protobuf/
# AASDK code become page-shareable instead of 100% private per-process
# memory -- real motivation: two reproducible OOM-killer hits of
# androidauto-sidecar this session on this 173MB/no-swap device.
# Unset/0 (default) preserves the exact original static behavior for
# the old $HOME/build-deps path the static androidauto-*-test
# diagnostic tools still use -- zero behavior change there.
LINK_SHARED="${LINK_SHARED:-0}"

# Pinned to 1.83.0 (not the newest release) -- matches Ubuntu 24.04's
# libboost-all-dev, which is what aasdk's own CI (.github/workflows/
# ci.yml, runs-on: ubuntu-24.04) actually builds and tests against.
# This is load-bearing, not cosmetic: aasdk's own source still uses
# `boost::asio::io_service` / `boost::asio::io_context::strand`
# directly (confirmed by grepping its Channel/Messenger headers) --
# both were fully removed from Boost.Asio by 1.87 (confirmed by
# actually hitting "'boost::asio::io_service' has not been declared"
# cross-compiling aasdk against a first attempt at Boost 1.87.0).
# 1.83 still has them.
BOOST_VERSION="1.83.0"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BOOST_BUILD_DIR:-$HOME/build-deps}"
CROSS_COMPILE="${CROSS_COMPILE:-arm-linux-gnueabihf-}"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

if [[ ! -d "boost-${BOOST_VERSION}" ]]; then
    # 1.83.0 predates the separate slimmed-down "-cmake" release
    # asset (that naming starts a couple releases later) -- use the
    # full source release instead, which still has the same top-level
    # CMakeLists.txt/CMake support, just bundled with everything else.
    echo "==> Downloading Boost ${BOOST_VERSION} (full source release)..."
    curl -sL -o boost.tar.gz \
        "https://github.com/boostorg/boost/releases/download/boost-${BOOST_VERSION}/boost-${BOOST_VERSION}.tar.gz"
    tar xzf boost.tar.gz
    rm boost.tar.gz
fi

SHARED_FLAG="OFF"
if [[ "$LINK_SHARED" == "1" ]]; then
    SHARED_FLAG="ON"
fi

cat > arm-toolchain.cmake <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_C_COMPILER ${CROSS_COMPILE}gcc)
set(CMAKE_CXX_COMPILER ${CROSS_COMPILE}g++)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(BUILD_SHARED_LIBS ${SHARED_FLAG})
EOF

cd "boost-${BOOST_VERSION}"
mkdir -p build-arm
cd build-arm

echo "==> Configuring (BUILD_SHARED_LIBS=$SHARED_FLAG)..."
cmake -DCMAKE_TOOLCHAIN_FILE="$BUILD_DIR/arm-toolchain.cmake" \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_SHARED_LIBS=$SHARED_FLAG \
      ..

echo "==> Building all of Boost (needed for a clean 'cmake --install', see note above)..."
cmake --build . -j"$(nproc)"

echo "==> Installing to $BUILD_DIR/boost-arm-install..."
cmake --install . --prefix "$BUILD_DIR/boost-arm-install"

echo
echo "✔ Installed: $BUILD_DIR/boost-arm-install/{include,lib}"
echo
echo "Add $BUILD_DIR/boost-arm-install to CMAKE_PREFIX_PATH (along with"
echo "the OpenSSL/libusb install prefixes) when configuring aasdk, with"
echo "-DBoost_USE_STATIC_LIBS=ON (aasdk's CMakeLists.txt defaults this"
echo "OFF, but we only built static .a files)."
