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
UI_TARGET     := $(BUILD_DIR)/custom_ui
UI_SRCS       := $(shell find src -name '*.cpp')
UI_OBJS       := $(patsubst src/%.cpp,$(BUILD_DIR)/ui-obj/%.o,$(UI_SRCS))

# --- carplay sidecar binary ---
# -ldbus-1 goes back in once this actually links libdbus -- needs its
# headers/lib sourced from the target rootfs or cross-built, neither of
# which is set up yet (Phase 7, deferred -- see docs/IMPLEMENTATION_PLAN.md).
# Doesn't link LVGL at all -- this process has no UI of its own.
SIDECAR_TARGET := $(BUILD_DIR)/carplay-sidecar
SIDECAR_SRCS   := $(shell find sidecars/carplay -name '*.cpp')
SIDECAR_OBJS   := $(patsubst sidecars/carplay/%.cpp,$(BUILD_DIR)/sidecar-obj/%.o,$(SIDECAR_SRCS))
SIDECAR_INCLUDES := -Isidecars/carplay

.PHONY: all clean ui carplay-sidecar

all: ui carplay-sidecar

ui: $(UI_TARGET)
carplay-sidecar: $(SIDECAR_TARGET)

$(UI_TARGET): $(UI_OBJS) $(LVGL_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

$(BUILD_DIR)/ui-obj/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

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
