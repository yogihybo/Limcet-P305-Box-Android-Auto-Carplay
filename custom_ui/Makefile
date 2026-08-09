# custom_ui build. Plain Makefile (no cmake in this environment) --
# consistent with how the rest of this repo's tools/ binaries are built.
#
# Uses the same cross toolchain as the rest of this repo
# (arm-linux-gnueabihf-*, matches linux-arkmicro/env.source's
# CROSS_COMPILE). Override CROSS_COMPILE on the command line if needed.
#
# Two binaries: the main UI (src/) and the carplay sidecar
# (sidecars/carplay/), built and run as separate processes -- see
# docs/ARCHITECTURE.md for why. `make all` builds both. Use -j for
# parallel builds (LVGL alone is ~500 source files).

CROSS_COMPILE ?= arm-linux-gnueabihf-
CXX           := $(CROSS_COMPILE)g++
CC            := $(CROSS_COMPILE)gcc

BUILD_DIR     := build

LVGL_DIR      := third_party/lvgl
# lv_conf.h lives at custom_ui/ (next to third_party/, per LVGL's own
# "assume lv_conf.h is next to the lvgl folder" convention) -- "-I."
# from custom_ui/ is what makes `__has_include("lv_conf.h")` find it.
INCLUDES      := -I. -Isrc -Iinclude -I$(LVGL_DIR)
CXXFLAGS      := -std=c++17 -Wall -Wextra -O2 $(INCLUDES)
CFLAGS        := -Wall -Wextra -O2 $(INCLUDES)
# Static linking is required, not optional: this repo's cross toolchain
# (Debian's system arm-linux-gnueabihf-g++, gcc 12) produces dynamic
# binaries that reference GLIBC_2.28/2.33/2.34 symbol versions -- the
# target's actual runtime glibc is 2.27 (confirmed via
# firmware_dumps/.../lib/libc-2.27.so), so a dynamically-linked binary
# from this toolchain will not even start on the device
# ("version GLIBC_2.28 not found"). This is the exact same
# host-toolchain-vs-target-glibc mismatch this project already solved
# once for busybox (tools/nss-stub/README.md) -- static linking sidesteps
# it by not depending on the target's libc.so.6 at runtime at all.
# Confirmed clean for this binary: no NSS/dlopen warnings at static-link
# time, so the nss-stub treatment busybox needed isn't required here.
LDFLAGS       := -static
LIBS          := -lpthread

# --- LVGL (C sources, shared by anything that links the UI) ---
LVGL_SRCS     := $(shell find $(LVGL_DIR)/src -name '*.c')
LVGL_OBJS     := $(patsubst $(LVGL_DIR)/%.c,$(BUILD_DIR)/lvgl-obj/%.o,$(LVGL_SRCS))

# --- main UI binary ---
# src/androidauto excluded here on purpose -- it needs aasdk/Boost/
# OpenSSL/libusb/Protobuf, and the base UI (screens, HAL, settings)
# should keep building fast and dependency-light. It's linked in for
# real once the actual Android Auto session/service integration lands
# (see docs/IMPLEMENTATION_PLAN.md Phase 2) -- until then it's only
# exercised by the androidauto-usb-probe-test target below.
UI_TARGET     := $(BUILD_DIR)/custom_ui
UI_SRCS       := $(filter-out src/androidauto/%,$(shell find src -name '*.cpp'))
UI_OBJS       := $(patsubst src/%.cpp,$(BUILD_DIR)/ui-obj/%.o,$(UI_SRCS))

# --- Android Auto (aasdk-backed) sources, shared by the future real
#     integration and the standalone probe test below ---
AASDK_SRCS    := $(shell find src/androidauto -name '*.cpp')
AASDK_OBJS    := $(patsubst src/%.cpp,$(BUILD_DIR)/ui-obj/%.o,$(AASDK_SRCS))

# Statically cross-compiled by third_party/build_{boost,openssl,libusb,
# protobuf,aasdk}.sh -- artifacts land outside the repo (too large to
# vendor, same reasoning as every other build_*.sh here). Override
# AASDK_DEPS_DIR if yours live somewhere other than the scripts' default.
AASDK_DEPS_DIR  ?= $(HOME)/build-deps
AASDK_DIR       := third_party/aasdk
AASDK_INCLUDES  := -I$(AASDK_DIR)/include \
                    -I$(AASDK_DIR)/build-arm/protobuf \
                    -I$(AASDK_DEPS_DIR)/boost-arm-install/include \
                    -I$(AASDK_DEPS_DIR)/openssl-arm-install/include \
                    -I$(AASDK_DEPS_DIR)/libusb-arm-install/include \
                    -I$(AASDK_DEPS_DIR)/libusb-arm-install/include/libusb-1.0 \
                    -I$(AASDK_DEPS_DIR)/protobuf-arm-install/include
# --start-group/--end-group: aasdk, aap_protobuf, protobuf, and Abseil
# static-link against each other in ways plain left-to-right link order
# can't always resolve in one pass -- --start-group lets ld retry the
# whole set until every symbol resolves.
AASDK_LIBS      := -Wl,--start-group \
                    $(AASDK_DIR)/build-arm/lib/libaasdk.a \
                    $(AASDK_DIR)/build-arm/lib/libaap_protobuf.a \
                    $(wildcard $(AASDK_DEPS_DIR)/boost-arm-install/lib/*.a) \
                    $(AASDK_DEPS_DIR)/libusb-arm-install/lib/libusb-1.0.a \
                    $(AASDK_DEPS_DIR)/openssl-arm-install/lib/libssl.a \
                    $(AASDK_DEPS_DIR)/openssl-arm-install/lib/libcrypto.a \
                    $(wildcard $(AASDK_DEPS_DIR)/protobuf-arm-install/lib/*.a) \
                    -Wl,--end-group -lpthread -ldl -lrt

# Same static-NSS-init crash this project already hit and fixed for
# nano/htop/tmux/gdbserver/busybox (tools/nss-stub/README.md):
# libcrypto.a references dlopen/getaddrinfo/gethostbyname internally
# (engine loading, BIO_lookup_ex) -- the link itself only warns
# ("Using 'dlopen' in statically linked applications requires..."),
# but this project has already confirmed on real hardware that those
# warnings are NOT harmless here: any static binary that references
# those symbols pulls in glibc's static-dlopen-NSS init machinery,
# which asserts and crashes at process startup on this toolchain/
# kernel combination, whether or not the code path is ever exercised.
# Reusing tools/nss-stub/nss_stub_busybox.c as-is (not duplicating it)
# -- it already wraps exactly this symbol set (dlopen family +
# getaddrinfo/gethostbyname family), unlike nss_stub.c which wraps the
# getpwnam/getgrgid family instead (this binary never references those).
NSS_STUB_SRC   := ../tools/nss-stub/nss_stub_busybox.c
NSS_STUB_OBJ   := $(BUILD_DIR)/aasdk-test-obj/nss_stub_busybox.o
AASDK_WRAP_FLAGS := -Wl,--wrap=dlopen,--wrap=dlerror,--wrap=dlsym,--wrap=dlclose,--wrap=getaddrinfo,--wrap=freeaddrinfo,--wrap=gai_strerror,--wrap=gethostbyname,--wrap=gethostbyname2,--wrap=gethostbyaddr,--wrap=getservbyname,--wrap=getservbyport

# --- carplay sidecar binary ---
# -ldbus-1 goes back in once this actually links libdbus -- needs its
# headers/lib sourced from the target rootfs or cross-built, neither of
# which is set up yet (Phase 7, deferred -- see docs/IMPLEMENTATION_PLAN.md).
# Doesn't link LVGL at all -- this process has no UI of its own.
SIDECAR_TARGET := $(BUILD_DIR)/carplay-sidecar
SIDECAR_SRCS   := $(shell find sidecars/carplay -name '*.cpp')
SIDECAR_OBJS   := $(patsubst sidecars/carplay/%.cpp,$(BUILD_DIR)/sidecar-obj/%.o,$(SIDECAR_SRCS))
SIDECAR_INCLUDES := -Isidecars/carplay

# --- androidauto-usb-probe-test: standalone aasdk link-graph proof ---
# See tools/androidauto-usb-probe-test/main.cpp -- not part of `all`,
# build explicitly with `make androidauto-usb-probe-test`.
AASDK_TEST_TARGET := $(BUILD_DIR)/androidauto-usb-probe-test
AASDK_TEST_SRCS    := tools/androidauto-usb-probe-test/main.cpp
AASDK_TEST_OBJS    := $(BUILD_DIR)/aasdk-test-obj/main.o

.PHONY: all clean ui carplay-sidecar androidauto-usb-probe-test

all: ui carplay-sidecar

ui: $(UI_TARGET)
carplay-sidecar: $(SIDECAR_TARGET)
androidauto-usb-probe-test: $(AASDK_TEST_TARGET)

$(UI_TARGET): $(UI_OBJS) $(LVGL_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

$(BUILD_DIR)/ui-obj/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/ui-obj/androidauto/%.o: src/androidauto/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(AASDK_INCLUDES) -c -o $@ $<

$(AASDK_TEST_TARGET): $(AASDK_TEST_OBJS) $(AASDK_OBJS) $(NSS_STUB_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) $(AASDK_WRAP_FLAGS) -o $@ $^ $(AASDK_LIBS)

$(BUILD_DIR)/aasdk-test-obj/%.o: tools/androidauto-usb-probe-test/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(AASDK_INCLUDES) -Isrc -c -o $@ $<

$(NSS_STUB_OBJ): $(NSS_STUB_SRC)
	@mkdir -p $(dir $@)
	$(CC) -O2 -c -o $@ $<

$(BUILD_DIR)/lvgl-obj/%.o: $(LVGL_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(SIDECAR_TARGET): $(SIDECAR_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

$(BUILD_DIR)/sidecar-obj/%.o: sidecars/carplay/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(SIDECAR_INCLUDES) -c -o $@ $<

clean:
	rm -rf $(BUILD_DIR)
