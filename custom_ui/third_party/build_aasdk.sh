#!/bin/bash
# Cross-compile aasdk itself (this must run AFTER build_boost.sh,
# build_openssl.sh, and build_libusb.sh -- it links against their
# outputs).
#
# Patches aasdk's own CMakeLists.txt (and protobuf/CMakeLists.txt)
# in-place before configuring: both unconditionally build SHARED
# libraries on any non-macOS system
# (`add_library(aasdk SHARED ...)` / `add_library(aap_protobuf SHARED
# ...)`), which conflicts with this project's static-linking
# requirement (same host-toolchain-vs-target-glibc-2.27 reasoning as
# every other build_*.sh here -- a shared libaasdk.so built with this
# toolchain would need a runtime loader/rpath setup on-device we don't
# want, on top of the glibc symbol version problem it would still
# carry). The patch is applied with `sed -i`, not upstreamed --
# aasdk's own Darwin-only STATIC branch suggests this was a deliberate
# (if narrow) upstream choice, so we patch our vendored copy locally
# rather than trying to get a behavior change accepted upstream.
#
# -DAASDK_TEST=OFF: skips aasdk's own googletest FetchContent (a test
# suite we don't need, and one more thing to cross-compile/network-fetch
# for no benefit here).

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AASDK_DIR="$SCRIPT_DIR/aasdk"
DEPS_DIR="${AASDK_DEPS_DIR:-$HOME/build-deps}"
CROSS_COMPILE="${CROSS_COMPILE:-arm-linux-gnueabihf-}"

echo "==> Patching aasdk to build static libs (SHARED -> STATIC, non-macOS branch)..."
sed -i 's/add_library(aasdk SHARED/add_library(aasdk STATIC/' "$AASDK_DIR/CMakeLists.txt"
sed -i 's/add_library(aap_protobuf SHARED/add_library(aap_protobuf STATIC/' "$AASDK_DIR/protobuf/CMakeLists.txt"

cat > "$DEPS_DIR/arm-toolchain.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_C_COMPILER ${CROSS_COMPILE}gcc)
set(CMAKE_CXX_COMPILER ${CROSS_COMPILE}g++)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(BUILD_SHARED_LIBS OFF)
EOF

mkdir -p "$AASDK_DIR/build-arm"
cd "$AASDK_DIR/build-arm"

echo "==> Configuring aasdk..."
cmake \
    -DCMAKE_TOOLCHAIN_FILE="$DEPS_DIR/arm-toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$DEPS_DIR/boost-arm-install;$DEPS_DIR/openssl-arm-install;$DEPS_DIR/libusb-arm-install" \
    -DBoost_USE_STATIC_LIBS=ON \
    -DBUILD_SHARED_LIBS=OFF \
    -DAASDK_TEST=OFF \
    ..

echo "==> Building..."
cmake --build . -j"$(nproc)"

echo
echo "✔ libaasdk.a + libaap_protobuf.a: $AASDK_DIR/build-arm/lib/"
echo
echo "Not yet wired into custom_ui/Makefile -- next step is the actual"
echo "LinuxVideoSink/LinuxAudioSink/etc integration classes (Phase 2,"
echo "see docs/IMPLEMENTATION_PLAN.md)."
