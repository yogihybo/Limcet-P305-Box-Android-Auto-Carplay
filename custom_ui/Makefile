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

# ==============================================================================
# Rules & Targets
# ==============================================================================

.PHONY: all clean ui carplay-sidecar androidauto-sidecar ladspa-eq

all: ui carplay-sidecar androidauto-sidecar ladspa-eq

ui: $(UI_TARGET) $(BUILD_DIR)/hal.conf $(BUILD_DIR)/default_settings.conf androidauto-sidecar
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

# --- Clean Target ---
clean:
	rm -rf $(BUILD_DIR)
	$(MAKE) -C micro_aap clean

-include $(shell find $(BUILD_DIR) -name '*.d' 2>/dev/null)
