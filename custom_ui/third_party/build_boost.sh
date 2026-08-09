#!/bin/bash
# Cross-compile the Boost libraries aasdk actually needs a compiled
# artifact for (boost_log, boost_log_setup -- everything else aasdk
# uses, per its own CMakeLists.txt COMPONENTS list plus a grep of its
# #includes, is header-only: system, asio, algorithm, core, endian).
#
# Not vendored as a git submodule or committed as binary blobs -- the
# Boost source release is ~130MB and we only need ~10MB of compiled
# .a output, so this script (matching this repo's existing build_*.sh
# convention for external components) is the reproducible artifact
# instead. Run once before building anything that links aasdk.
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

BOOST_VERSION="1.87.0"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BOOST_BUILD_DIR:-$HOME/build-deps}"
CROSS_COMPILE="${CROSS_COMPILE:-arm-linux-gnueabihf-}"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

if [[ ! -d "boost-${BOOST_VERSION}" ]]; then
    echo "==> Downloading Boost ${BOOST_VERSION} (CMake release, ~130MB)..."
    curl -sL -o boost.tar.gz \
        "https://github.com/boostorg/boost/releases/download/boost-${BOOST_VERSION}/boost-${BOOST_VERSION}-cmake.tar.gz"
    tar xzf boost.tar.gz
    rm boost.tar.gz
fi

cat > arm-toolchain.cmake <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_C_COMPILER ${CROSS_COMPILE}gcc)
set(CMAKE_CXX_COMPILER ${CROSS_COMPILE}g++)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(BUILD_SHARED_LIBS OFF)
EOF

cd "boost-${BOOST_VERSION}"
mkdir -p build-arm
cd build-arm

echo "==> Configuring..."
cmake -DCMAKE_TOOLCHAIN_FILE="$BUILD_DIR/arm-toolchain.cmake" \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_SHARED_LIBS=OFF \
      ..

echo "==> Building boost_log + boost_log_setup (and their transitive deps)..."
cmake --build . --target boost_log boost_log_setup -j"$(nproc)"

echo
echo "✔ Static libs: $BUILD_DIR/boost-${BOOST_VERSION}/build-arm/stage/lib/*.a"
echo "✔ Headers (header-only Boost.System/Asio/etc included): $BUILD_DIR/boost-${BOOST_VERSION}/libs/*/include, $BUILD_DIR/boost-${BOOST_VERSION}/boost/"
echo
echo "Point custom_ui's build at these paths -- not yet wired into"
echo "custom_ui/Makefile, that lands alongside the actual aasdk"
echo "integration work (Phase 2, see docs/IMPLEMENTATION_PLAN.md)."
