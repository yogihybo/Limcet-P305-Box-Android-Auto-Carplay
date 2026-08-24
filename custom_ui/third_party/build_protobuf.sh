#!/bin/bash
# Cross-compile Protobuf (static, ARM) for aasdk's protobuf/ subdir
# (`aap_protobuf`), which generates + compiles the actual Android Auto
# wire-protocol .proto files.
#
# Correction to an earlier (wrong) assumption in this project's own
# notes: aasdk's TOP-LEVEL CMakeLists.txt mentions "Using built
# protobuf v30.0" next to its SKIP_BUILD_PROTOBUF option, which reads
# like it self-fetches protobuf via FetchContent -- it does NOT. The
# only FetchContent_Declare anywhere in aasdk's CMake is for
# googletest. aasdk's protobuf/CMakeLists.txt (non-macOS branch) does
# a plain find_path()/find_library()/find_program() for an already
# system-installed protobuf (headers, libprotobuf, and a protoc
# binary) and hard FATAL_ERRORs if not found. So this dependency does
# need real work here, same as Boost/OpenSSL/libusb.
#
# Two distinct builds are needed, not one:
#   1. A HOST-native `protoc` binary -- runs on the build machine
#      during cmake configure/build to compile .proto -> .cc/.h; a
#      cross-compiled ARM protoc can't run on this host at all. Using
#      Google's own prebuilt protoc release binary instead of building
#      one -- no reason to compile a host toolchain artifact from
#      source when a matching official binary exists.
#   2. A cross-compiled ARM static libprotobuf -- what aap_protobuf
#      actually links into the final (ARM) binary.
# Both must be the SAME protobuf version -- generated .pb.cc/.pb.h
# code is not guaranteed compatible across protoc/libprotobuf version
# skew.
#
# Static linking (ARM side) is load-bearing, not a style choice -- same
# host-toolchain-vs-target-glibc (2.27) mismatch reasoning as every
# other build_*.sh here.

set -e

# 2026-08-24: LINK_SHARED=1 builds a real .so (and shared Abseil, via
# protobuf_ABSL_PROVIDER=module inheriting the same BUILD_SHARED_LIBS)
# instead of .a's -- see build_boost.sh's own header comment for the
# real motivation (page-shareable memory on this 173MB/no-swap
# device). Unset/0 (default) is the exact original static behavior.
LINK_SHARED="${LINK_SHARED:-0}"

PROTOBUF_VERSION="25.3"
BUILD_DIR="${PROTOBUF_BUILD_DIR:-$HOME/build-deps}"
CROSS_COMPILE="${CROSS_COMPILE:-arm-linux-gnueabihf-}"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# --- 1. Host protoc (prebuilt binary, no build needed) ---
if [[ ! -x "$BUILD_DIR/protoc-host/bin/protoc" ]]; then
    echo "==> Downloading prebuilt host protoc ${PROTOBUF_VERSION}..."
    mkdir -p protoc-host
    curl -sL -o protoc-host.zip \
        "https://github.com/protocolbuffers/protobuf/releases/download/v${PROTOBUF_VERSION}/protoc-${PROTOBUF_VERSION}-linux-x86_64.zip"
    (cd protoc-host && unzip -oq ../protoc-host.zip)
    rm protoc-host.zip
    chmod +x "$BUILD_DIR/protoc-host/bin/protoc"
fi

# --- 2. ARM static libprotobuf (cross-compiled from source) ---
if [[ ! -d "protobuf-${PROTOBUF_VERSION}" ]]; then
    echo "==> Downloading protobuf ${PROTOBUF_VERSION} source (bundles Abseil)..."
    curl -sL -o protobuf.tar.gz \
        "https://github.com/protocolbuffers/protobuf/releases/download/v${PROTOBUF_VERSION}/protobuf-${PROTOBUF_VERSION}.tar.gz"
    tar xzf protobuf.tar.gz
    rm protobuf.tar.gz
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

cd "protobuf-${PROTOBUF_VERSION}"
mkdir -p build-arm
cd build-arm

echo "==> Configuring (BUILD_SHARED_LIBS=$SHARED_FLAG; ARM libprotobuf, host protoc for codegen, bundled Abseil)..."
cmake -DCMAKE_TOOLCHAIN_FILE="$BUILD_DIR/arm-toolchain.cmake" \
      -DCMAKE_BUILD_TYPE=Release \
      -Dprotobuf_BUILD_TESTS=OFF \
      -Dprotobuf_BUILD_PROTOC_BINARIES=OFF \
      -Dprotobuf_ABSL_PROVIDER=module \
      -DBUILD_SHARED_LIBS=$SHARED_FLAG \
      -DProtobuf_PROTOC_EXECUTABLE="$BUILD_DIR/protoc-host/bin/protoc" \
      ..

echo "==> Building..."
cmake --build . -j"$(nproc)"

echo "==> Installing to $BUILD_DIR/protobuf-arm-install..."
cmake --install . --prefix "$BUILD_DIR/protobuf-arm-install"

echo
echo "✔ Host protoc: $BUILD_DIR/protoc-host/bin/protoc"
echo "✔ ARM static libprotobuf: $BUILD_DIR/protobuf-arm-install/{include,lib}"
echo
echo "build_aasdk.sh adds protoc-host/bin to PATH and protobuf-arm-install"
echo "to CMAKE_PREFIX_PATH -- run this script before build_aasdk.sh."
