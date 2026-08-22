#!/bin/bash
# Cross-compile aasdk itself (this must run AFTER build_boost.sh,
# build_openssl.sh, build_libusb.sh, AND build_protobuf.sh -- it links
# against all four's outputs).
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
#
# Protobuf variable case mismatch: aasdk's protobuf/CMakeLists.txt
# manually finds protobuf into mixed-case `Protobuf_INCLUDE_DIR`/
# `Protobuf_LIBRARY`, but its own include_directories()/
# target_link_libraries() calls (and the top-level CMakeLists.txt's)
# reference the all-caps `PROTOBUF_INCLUDE_DIR`/`PROTOBUF_LIBRARIES` --
# a different, never-actually-set variable name. `aap_protobuf` built
# anyway (apparently via CMake's bundled FindProtobuf.cmake module,
# `include()`-d for its `protobuf_generate_cpp()` macro, incidentally
# also running its own internal search); the top-level `aasdk` target
# did not (confirmed: "google/protobuf/message.h: No such file"). Fix:
# pass the all-caps variables explicitly as cache entries on the
# command line below, so every scope sees the same values regardless
# of which spelling a given CMakeLists.txt line happens to use.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AASDK_DIR="$SCRIPT_DIR/aasdk"
DEPS_DIR="${AASDK_DEPS_DIR:-$HOME/build-deps}"
# Defaults to the original hardcoded "build-arm" for backwards
# compatibility. Override when building against a second, different
# toolchain (e.g. custom_ui's new dynamically-linked rootfs, Linaro
# 7.3.1) without clobbering an existing static build-arm/ that other
# still-static targets (androidauto-usb-probe-test etc., see
# custom_ui/Makefile) link against by the same fixed path -- real
# problem hit 2026-08-21: reusing build-arm/ for a different toolchain
# silently picked up a stale CMakeCache.txt pointing at the wrong
# dependency versions, and partially overwrote the working static
# libaap_protobuf.a before the mismatch was caught.
AASDK_BUILD_SUBDIR="${AASDK_BUILD_SUBDIR:-build-arm}"
CROSS_COMPILE="${CROSS_COMPILE:-arm-linux-gnueabihf-}"

echo "==> Patching aasdk to build static libs (SHARED -> STATIC, non-macOS branch)..."
sed -i 's/add_library(aasdk SHARED/add_library(aasdk STATIC/' "$AASDK_DIR/CMakeLists.txt"
sed -i 's/add_library(aap_protobuf SHARED/add_library(aap_protobuf STATIC/' "$AASDK_DIR/protobuf/CMakeLists.txt"
# CMakeLists.txt's own `set(Boost_USE_STATIC_LIBS OFF)` (a plain, non-
# CACHE set()) shadows our -DBoost_USE_STATIC_LIBS=ON command-line flag
# for the rest of that directory scope -- confirmed by hitting
# "Could not find a configuration file for package boost_log_setup
# that exactly matches requested version" (it found the -static
# variant but wasn't looking for it). Patch the default directly, and
# drop -DBOOST_ALL_DYN_LINK (a dynamic-import declspec macro that has
# no business being defined when linking Boost statically).
sed -i 's/set(Boost_USE_STATIC_LIBS OFF)/set(Boost_USE_STATIC_LIBS ON)/' "$AASDK_DIR/CMakeLists.txt"
sed -i '/add_definitions(-DBOOST_ALL_DYN_LINK)/d' "$AASDK_DIR/CMakeLists.txt"

# GCC < 8's libstdc++ only ships the pre-standardization Filesystem TS
# under <experimental/filesystem> (requires -lstdc++fs) -- the
# finalized, non-experimental <filesystem> landed in GCC 8. Hit
# building ModernLogger.cpp with the Linaro 7.3.1-2018.05 toolchain
# (custom_ui's new dynamically-linked rootfs): "fatal error: filesystem:
# No such file or directory". Applied unconditionally (not gated on
# which CROSS_COMPILE this run uses) -- the __GNUC__ < 8 guard below
# makes the result self-adapting: falls through to real <filesystem>
# on the system toolchain (GCC 12, still used for the static
# androidauto-*-test tools) with zero behavior change there. Idempotent
# same as the patches above -- a no-op if already applied.
if grep -q '^#include <filesystem>$' "$AASDK_DIR/src/Common/ModernLogger.cpp"; then
    sed -i '/^#include <filesystem>$/c\
#if defined(__GNUC__) \&\& __GNUC__ < 8 \&\& !defined(__clang__)\
#include <experimental/filesystem>\
namespace fs = std::experimental::filesystem;\
#else\
#include <filesystem>\
namespace fs = std::filesystem;\
#endif' "$AASDK_DIR/src/Common/ModernLogger.cpp"
    sed -i 's/std::filesystem::/fs::/g' "$AASDK_DIR/src/Common/ModernLogger.cpp"
fi
if ! grep -q 'stdc++fs' "$AASDK_DIR/CMakeLists.txt"; then
    sed -i '/target_link_libraries(aasdk PUBLIC/i\
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 8)\
    target_link_libraries(aasdk PUBLIC stdc++fs)\
endif()\
' "$AASDK_DIR/CMakeLists.txt"
fi

mkdir -p "$AASDK_DIR/$AASDK_BUILD_SUBDIR"
cd "$AASDK_DIR/$AASDK_BUILD_SUBDIR"

cat > "$AASDK_DIR/$AASDK_BUILD_SUBDIR/arm-toolchain.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_C_COMPILER ${CROSS_COMPILE}gcc)
set(CMAKE_CXX_COMPILER ${CROSS_COMPILE}g++)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(BUILD_SHARED_LIBS OFF)
EOF

echo "==> Configuring aasdk..."
PATH="$DEPS_DIR/protoc-host/bin:$PATH" \
cmake \
    -DCMAKE_TOOLCHAIN_FILE="$AASDK_DIR/$AASDK_BUILD_SUBDIR/arm-toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$DEPS_DIR/boost-arm-install;$DEPS_DIR/openssl-arm-install;$DEPS_DIR/libusb-arm-install;$DEPS_DIR/protobuf-arm-install" \
    -DBoost_USE_STATIC_LIBS=ON \
    -DBUILD_SHARED_LIBS=OFF \
    -DAASDK_TEST=OFF \
    -DPROTOBUF_PROTOC_EXECUTABLE="$DEPS_DIR/protoc-host/bin/protoc" \
    -DPROTOBUF_INCLUDE_DIR="$DEPS_DIR/protobuf-arm-install/include" \
    -DPROTOBUF_LIBRARIES="$DEPS_DIR/protobuf-arm-install/lib/libprotobuf.a" \
    ..

echo "==> Building..."
cmake --build . -j"$(nproc)"

echo
echo "✔ libaasdk.a + libaap_protobuf.a: $AASDK_DIR/$AASDK_BUILD_SUBDIR/lib/"
echo
echo "Not yet wired into custom_ui/Makefile -- next step is the actual"
echo "LinuxVideoSink/LinuxAudioSink/etc integration classes (Phase 2,"
echo "see docs/IMPLEMENTATION_PLAN.md)."
