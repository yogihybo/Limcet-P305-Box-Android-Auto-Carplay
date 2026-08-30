# ==============================================================================
# custom_ui Makefile
#
# Primary Targets:
#   - ui                  : LVGL 9 GUI + Hardware Abstraction Layer + BlueZ Client
#   - androidauto-sidecar : Micro-AAP C sidecar (Nanopb + OpenSSL + ALSA)
#   - carplay-sidecar     : CarPlay projection sidecar daemon
# ==============================================================================

BUILDROOT_OUTPUT_DIR ?= $(HOME)/Downloads/linux-arkmicro/buildroot/output
BUILDROOT_STAGING    ?= $(BUILDROOT_OUTPUT_DIR)/staging
CROSS_COMPILE        ?= $(BUILDROOT_OUTPUT_DIR)/host/bin/arm-buildroot-linux-gnueabihf-

CXX           := $(CROSS_COMPILE)g++
CC            := $(CROSS_COMPILE)gcc
BUILD_DIR     := build

# System toolchain fallback for legacy static diagnostic test tools
SYSTEM_CROSS  ?= arm-linux-gnueabihf-
SYSTEM_CXX    := $(SYSTEM_CROSS)g++
SYSTEM_CC     := $(SYSTEM_CROSS)gcc

LVGL_DIR      := third_party/lvgl
INCLUDES      := -I. -Isrc -Iinclude -I$(LVGL_DIR)
CXXFLAGS      := -std=c++17 -Wall -Wextra -O2 -MMD -MP $(INCLUDES)
CFLAGS        := -Wall -Wextra -O2 -MMD -MP $(INCLUDES)
LDFLAGS       := -s
LIBS          := -lpthread

# D-Bus configuration from Buildroot staging sysroot
DBUS_INCLUDES := -I$(BUILDROOT_STAGING)/usr/include/dbus-1.0 -I$(BUILDROOT_STAGING)/usr/lib/dbus-1.0/include
DBUS_LIBS     := -L$(BUILDROOT_STAGING)/usr/lib -ldbus-1

# --- LVGL Sources ---
LVGL_SRCS     := $(shell find $(LVGL_DIR)/src -name '*.c')
LVGL_OBJS     := $(patsubst $(LVGL_DIR)/%.c,$(BUILD_DIR)/lvgl-obj/%.o,$(LVGL_SRCS))

# --- Main custom_ui Binary ---
UI_TARGET     := $(BUILD_DIR)/custom_ui
UI_SRCS       := $(filter-out src/androidauto/% src/ui/icons.cpp src/ui/icons_m3.cpp,$(shell find src -name '*.cpp'))
UI_OBJS       := $(patsubst src/%.cpp,$(BUILD_DIR)/ui-obj/%.o,$(UI_SRCS))
UI_C_SRCS     := $(filter-out src/androidauto/%,$(shell find src -name '*.c'))
UI_C_OBJS     := $(patsubst src/%.c,$(BUILD_DIR)/ui-obj/%.o,$(UI_C_SRCS))

# --- CarPlay Sidecar Binary ---
SIDECAR_TARGET   := $(BUILD_DIR)/carplay-sidecar
SIDECAR_SRCS     := $(shell find sidecars/carplay -name '*.cpp')
SIDECAR_OBJS     := $(patsubst sidecars/carplay/%.cpp,$(BUILD_DIR)/sidecar-obj/%.o,$(SIDECAR_SRCS))
SIDECAR_INCLUDES := -Isidecars/carplay

# --- Android Auto Sidecar Target ---
ANDROIDAUTO_SIDECAR_TARGET := $(BUILD_DIR)/androidauto-sidecar

# --- Legacy Diagnostic Tools Configuration ---
AASDK_DEPS_DIR   ?= $(HOME)/build-deps
AASDK_DIR        := third_party/aasdk
OPENSSL_ARM_INSTALL ?= $(AASDK_DEPS_DIR)/openssl-arm-install-noengine-armv7
ALSA_ARM_INSTALL    ?= $(AASDK_DEPS_DIR)/alsa-arm-install-1.2.4
DBUS_ARM_INSTALL    ?= $(AASDK_DEPS_DIR)/dbus-arm-install

AASDK_INCLUDES   := -I$(AASDK_DIR)/include \
                    -I$(AASDK_DIR)/build-arm/protobuf \
                    -I$(AASDK_DEPS_DIR)/boost-arm-install/include \
                    -I$(OPENSSL_ARM_INSTALL)/include \
                    -I$(AASDK_DEPS_DIR)/libusb-arm-install/include \
                    -I$(AASDK_DEPS_DIR)/libusb-arm-install/include/libusb-1.0 \
                    -I$(AASDK_DEPS_DIR)/protobuf-arm-install/include \
                    -I$(ALSA_ARM_INSTALL)/include \
                    -Ithird_party/bluez_uapi

AASDK_LIBS       := -Wl,--start-group \
                    $(AASDK_DIR)/build-arm/lib/libaasdk.a \
                    $(AASDK_DIR)/build-arm/lib/libaap_protobuf.a \
                    $(wildcard $(AASDK_DEPS_DIR)/boost-arm-install/lib/*.a) \
                    $(AASDK_DEPS_DIR)/libusb-arm-install/lib/libusb-1.0.a \
                    $(OPENSSL_ARM_INSTALL)/lib/libssl.a \
                    $(OPENSSL_ARM_INSTALL)/lib/libcrypto.a \
                    $(ALSA_ARM_INSTALL)/lib/libasound.a \
                    $(wildcard $(AASDK_DEPS_DIR)/protobuf-arm-install/lib/*.a) \
                    -Wl,--end-group -lpthread -ldl -lrt

AASDK_WRAP_FLAGS := -Wl,--wrap=dlopen,--wrap=dlerror,--wrap=dlsym,--wrap=dlclose,--wrap=getaddrinfo,--wrap=freeaddrinfo,--wrap=gai_strerror,--wrap=gethostbyname,--wrap=gethostbyname2,--wrap=gethostbyaddr,--wrap=getservbyname,--wrap=getservbyport,--wrap=getpwuid_r,--wrap=getpwnam_r,--wrap=getgrnam_r,--wrap=getgrouplist
NSS_STUB_SRC     := ../tools/nss-stub/nss_stub_busybox.c
NSS_STUB_OBJ     := $(BUILD_DIR)/aasdk-test-obj/nss_stub_busybox.o

AASDK_TEST_TARGET    := $(BUILD_DIR)/androidauto-usb-probe-test
AASDK_TEST_OBJS      := $(BUILD_DIR)/aasdk-test-obj/main.o
WIRELESS_TEST_TARGET := $(BUILD_DIR)/androidauto-wireless-probe-test
WIRELESS_TEST_OBJS   := $(BUILD_DIR)/wireless-test-obj/main.o
BT_TEST_TARGET       := $(BUILD_DIR)/androidauto-bluetooth-rfcomm-test
BT_TEST_OBJS         := $(BUILD_DIR)/bt-test-obj/main.o
BW_AAP_TEST_TARGET   := $(BUILD_DIR)/androidauto-bw-aap-test
BW_AAP_TEST_OBJS     := $(BUILD_DIR)/bw-aap-test-obj/main.o

# ==============================================================================
# Rules & Targets
# ==============================================================================

.PHONY: all clean ui carplay-sidecar androidauto-sidecar ladspa-eq \
        androidauto-usb-probe-test androidauto-wireless-probe-test \
        androidauto-bluetooth-rfcomm-test androidauto-bw-aap-test

all: ui carplay-sidecar androidauto-sidecar ladspa-eq

ui: $(UI_TARGET) $(BUILD_DIR)/hal.conf $(BUILD_DIR)/default_settings.conf
carplay-sidecar: $(SIDECAR_TARGET)
androidauto-sidecar: $(ANDROIDAUTO_SIDECAR_TARGET) $(BUILD_DIR)/hal.conf $(BUILD_DIR)/alsa/alsa.conf

# --- System-wide EQ/Loudness LADSPA plugin ---
# Hosted via ALSA's own built-in "type ladspa" PCM plugin (confirmed
# compiled directly into this target's libasound.so, no separate staged
# module needed for the hosting mechanism itself -- see
# firmware_overlay_dyn/etc/asound.conf's own comment). Real, plain-C
# shared library, zero dependency beyond the vendored ladspa.h (a single,
# stable, 20+ year old interface header, not a build/runtime dependency
# on any actual LADSPA SDK or plugin collection). See
# ladspa_eq/carpi_eq.c's own header comment for the full design
# reasoning (why a params file instead of ALSA controls, why inserted at
# dmix rather than per-source).
LADSPA_EQ_TARGET := $(BUILD_DIR)/ladspa/carpi_eq.so
ladspa-eq: $(LADSPA_EQ_TARGET)

$(LADSPA_EQ_TARGET): ladspa_eq/carpi_eq.c ladspa_eq/ladspa.h
	@mkdir -p $(dir $@)
	$(CC) -shared -fPIC -O2 -Wall -Wextra -Iladspa_eq -o $@ ladspa_eq/carpi_eq.c -lm
	@echo "Built $@"

# Staged runtime configuration files
$(BUILD_DIR)/hal.conf: etc/hal.conf
	@mkdir -p $(dir $@)
	cp $< $@

$(BUILD_DIR)/default_settings.conf: etc/default_settings.conf
	@mkdir -p $(dir $@)
	cp $< $@

$(BUILD_DIR)/alsa/alsa.conf: $(shell find etc/alsa -type f)
	@mkdir -p $(dir $@)
	cp -a etc/alsa/. $(BUILD_DIR)/alsa/

# --- Main UI Target ---
$(UI_TARGET): $(UI_OBJS) $(UI_C_OBJS) $(LVGL_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) -o $@ $^ $(DBUS_LIBS) $(LIBS)
	@echo "Built $@"

# Files requiring D-Bus client headers
$(BUILD_DIR)/ui-obj/hal/bluez_aa_profile.o: src/hal/bluez_aa_profile.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DBUS_INCLUDES) -c -o $@ $<

$(BUILD_DIR)/ui-obj/hal/ble_cts.o: src/hal/ble_cts.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DBUS_INCLUDES) -c -o $@ $<

$(BUILD_DIR)/ui-obj/hal/bluetooth.o: src/hal/bluetooth.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DBUS_INCLUDES) -c -o $@ $<

$(BUILD_DIR)/ui-obj/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/ui-obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/lvgl-obj/%.o: $(LVGL_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# --- Micro-AAP Sidecar Target ---
# Real fix (2026-08-30): this target's own file path was previously
# marked .PHONY -- a real anti-pattern, not a deliberate "always
# rebuild" choice. .PHONY means Make never checks this file's
# timestamp at all, so `androidauto-sidecar`/`all` could never tell
# whether the staged copy was actually current; every build silently
# re-ran the full recursive sub-make and re-copied unconditionally,
# and anything depending on this target's mtime downstream would see
# it as perpetually "just rebuilt." FORCE (a genuinely phony dummy
# target, not this real file) still forces re-entry into the recursive
# sub-make every time -- unavoidable, since micro_aap's own staleness
# can't be known without invoking it -- but that sub-make already does
# real dependency tracking internally (confirmed: reports "Nothing to
# be done" when genuinely current), and the final copy is now only
# performed, with output, when the sub-build's own result is actually
# newer than what's already staged.
.PHONY: FORCE
FORCE:

$(ANDROIDAUTO_SIDECAR_TARGET): FORCE
	@mkdir -p $(dir $@)
	$(MAKE) -C micro_aap BUILDROOT_OUTPUT_DIR=$(BUILDROOT_OUTPUT_DIR)
	@if [ micro_aap/build/micro-androidauto-sidecar -nt $@ ] || [ ! -e $@ ]; then \
		cp -f micro_aap/build/micro-androidauto-sidecar $@; \
		echo "Staged micro-androidauto-sidecar -> $@"; \
	else \
		echo "$@ already up to date"; \
	fi

# --- CarPlay Sidecar Target ---
$(SIDECAR_TARGET): $(SIDECAR_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)
	@echo "Built $@"

$(BUILD_DIR)/sidecar-obj/%.o: sidecars/carplay/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(SIDECAR_INCLUDES) -c -o $@ $<

# --- Legacy Diagnostic Probe Test Targets ---
$(NSS_STUB_OBJ): $(NSS_STUB_SRC)
	@mkdir -p $(dir $@)
	$(SYSTEM_CC) -O2 -c -o $@ $<

$(AASDK_TEST_TARGET): $(AASDK_TEST_OBJS) $(NSS_STUB_OBJ)
	@mkdir -p $(dir $@)
	$(SYSTEM_CXX) -static -s $(AASDK_WRAP_FLAGS) -o $@ $^ $(AASDK_LIBS)

$(BUILD_DIR)/aasdk-test-obj/%.o: tools/androidauto-usb-probe-test/%.cpp
	@mkdir -p $(dir $@)
	$(SYSTEM_CXX) $(CXXFLAGS) $(AASDK_INCLUDES) -Isrc -c -o $@ $<

$(WIRELESS_TEST_TARGET): $(WIRELESS_TEST_OBJS) $(NSS_STUB_OBJ)
	@mkdir -p $(dir $@)
	$(SYSTEM_CXX) -static -s $(AASDK_WRAP_FLAGS) -o $@ $^ $(AASDK_LIBS)

$(BUILD_DIR)/wireless-test-obj/%.o: tools/androidauto-wireless-probe-test/%.cpp
	@mkdir -p $(dir $@)
	$(SYSTEM_CXX) $(CXXFLAGS) $(AASDK_INCLUDES) -Isrc -c -o $@ $<

$(BT_TEST_TARGET): $(BT_TEST_OBJS) $(NSS_STUB_OBJ)
	@mkdir -p $(dir $@)
	$(SYSTEM_CXX) -static -s $(AASDK_WRAP_FLAGS) -o $@ $^ $(AASDK_LIBS)

$(BUILD_DIR)/bt-test-obj/%.o: tools/androidauto-bluetooth-rfcomm-test/%.cpp
	@mkdir -p $(dir $@)
	$(SYSTEM_CXX) $(CXXFLAGS) $(AASDK_INCLUDES) -Isrc -c -o $@ $<

$(BW_AAP_TEST_TARGET): $(BW_AAP_TEST_OBJS) $(NSS_STUB_OBJ)
	@mkdir -p $(dir $@)
	$(SYSTEM_CXX) -static -s $(AASDK_WRAP_FLAGS) -o $@ $^ $(AASDK_LIBS)

$(BUILD_DIR)/bw-aap-test-obj/%.o: tools/androidauto-bw-aap-test/%.cpp
	@mkdir -p $(dir $@)
	$(SYSTEM_CXX) $(CXXFLAGS) $(AASDK_INCLUDES) -Isrc -c -o $@ $<

# --- Clean Target ---
clean:
	rm -rf $(BUILD_DIR)
	$(MAKE) -C micro_aap clean

-include $(shell find $(BUILD_DIR) -name '*.d' 2>/dev/null)
