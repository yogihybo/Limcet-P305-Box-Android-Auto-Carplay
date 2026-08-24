# custom_ui build. Plain Makefile (no cmake in this environment) --
# consistent with how the rest of this repo's tools/ binaries are built.
#
# Uses the same cross toolchain as the rest of this repo
# (arm-linux-gnueabihf-*, matches linux-arkmicro/env.source's
# CROSS_COMPILE). Override CROSS_COMPILE on the command line if needed.
#
# Three binaries: the main UI (src/), the carplay sidecar
# (sidecars/carplay/), and the androidauto sidecar
# (sidecars/androidauto/), built and run as separate processes -- see
# docs/ARCHITECTURE.md for why (isolating heavy per-integration
# dependency stacks -- libdbus, aasdk/Boost/OpenSSL/Protobuf -- from
# the core UI/HAL binary). `make all` builds all three. Use -j for
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
# -MMD -MP: emit a per-object .d file listing every header it actually
# includes, so a header-only edit (no matching .cpp/.c change) correctly
# triggers a rebuild of everything that includes it -- without this,
# every rule below only depends on its single source file via the
# pattern rule's $<, so Make has NO way to know e.g. hal_config.o needs
# rebuilding after hal_config.h changes, and silently keeps linking a
# stale object into the final binary. Confirmed as a real bug this
# project hit: hal_config.h's wifi_session_port_ default was edited but
# `make androidauto-sidecar` reported "Nothing to be done" and the
# binary's mtime didn't move. See the `-include` of these .d files near
# the bottom of this file.
CXXFLAGS      := -std=c++17 -Wall -Wextra -O2 -MMD -MP $(INCLUDES)
CFLAGS        := -Wall -Wextra -O2 -MMD -MP $(INCLUDES)
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
# 2026-08-16: -s (strip debug symbols at link time) added after real
# hardware showed androidauto-sidecar at 71MB on disk (vs custom_ui's
# 2.5MB) -- this device only has ~53MB free RAM, so a single binary
# bigger than all of it was thrashing hard (kswapd0/usb-storage/kworker
# all busy in a real `top` capture, 32% io-wait, 64% sys -- a memory/IO
# pressure signature, not a userspace busy-loop, which earlier looked
# like the more likely cause of a reported near-freeze). None of this
# project's own CXXFLAGS ever added -g -- the debug info comes from the
# statically-linked deps themselves (Boost/OpenSSL/Protobuf/aasdk,
# built by their own separate scripts), so stripping the final linked
# binary is the practical fix rather than rebuilding every dependency
# without -g. Applies to every target uniformly. On-device debugging
# stays available via gdbserver (see rcS's own diagnostic-tools list)
# against a live process; symbol resolution on the DEV machine's own
# gdb still works from the matching unstripped object files this
# Makefile keeps in build/*-obj/, matched by build-id if ever needed.
LDFLAGS       := -static -s
LIBS          := -lpthread

# --- Dynamic linking for `ui`/`androidauto-sidecar` only (Phase 4 of
# the new general-purpose dynamically-linked rootfs migration,
# merry-snacking-wirth.md) ---
# Everything else in this Makefile (carplay-sidecar, the 4
# androidauto-*-test diagnostic tools) stays on the static
# CROSS_COMPILE/LDFLAGS above, untouched.
#
# CRITICAL: this is NOT just "drop -static". This Makefile's own
# CROSS_COMPILE above is the SYSTEM Debian arm-linux-gnueabihf-g++
# (gcc 12, targets GLIBC_2.28+ per this file's own comment on
# LDFLAGS) -- a dynamically-linked binary from THAT toolchain would
# reference glibc symbol versions the new rootfs's own libc (2.25,
# Linaro 7.3.1, same toolchain the kernel/U-Boot/Buildroot already
# use) doesn't have -- the exact "GLIBC_2.28 not found" failure this
# project already hit and documented once for busybox
# (tools/nss-stub/README.md's own history). So the dynamic build also
# switches to Buildroot's own host toolchain wrapper (pre-sysrooted at
# BUILDROOT_STAGING below), not just the link mode.
BUILDROOT_OUTPUT_DIR ?= $(HOME)/Downloads/linux-arkmicro/buildroot/output
BUILDROOT_STAGING    ?= $(BUILDROOT_OUTPUT_DIR)/staging
CROSS_COMPILE_DYN     := $(BUILDROOT_OUTPUT_DIR)/host/bin/arm-buildroot-linux-gnueabihf-
CXX_DYN               := $(CROSS_COMPILE_DYN)g++
CC_DYN                := $(CROSS_COMPILE_DYN)gcc
LDFLAGS_DYN           := -s
LIBS_DYN              := -lpthread

# --- LVGL (C sources, shared by anything that links the UI) ---
LVGL_SRCS     := $(shell find $(LVGL_DIR)/src -name '*.c')
LVGL_OBJS     := $(patsubst $(LVGL_DIR)/%.c,$(BUILD_DIR)/lvgl-obj/%.o,$(LVGL_SRCS))

# --- main UI binary ---
# src/androidauto excluded here on purpose, same reasoning as
# carplay-sidecar (docs/ARCHITECTURE.md): briefly linked in directly
# (2026-08-11) to prove the link graph resolves inside the real app,
# then moved back out into its own process, sidecars/androidauto/ --
# aasdk/Boost/OpenSSL/libusb/Protobuf is a heavy dependency stack (the
# binary was ~50MB with it linked in, vs ~2.3MB without) that shouldn't
# live in the same process as the UI/HAL/settings code, any more than
# libdbus should for CarPlay. custom_ui talks to androidauto-sidecar
# over a small local socket protocol via src/hal/androidauto_client.h
# instead -- zero aasdk knowledge in this binary.
#
# 2026-08-20: libdbus is now the ONE exception to "custom_ui links no
# per-integration protocol library" above -- src/hal/bluez_aa_profile.cpp
# (moved here from src/androidauto/bluez_client.cpp) needs a real D-Bus
# client to register the Android Auto RFCOMM profile and receive
# Profile1.NewConnection's UNIX_FD, which a dbus-send shell-out (every
# other hal/bluetooth.cpp D-Bus call) fundamentally can't do -- there's
# no way to register an incoming-call object path or block on a
# connection from a one-shot CLI tool. This is deliberately just
# libdbus (456KB static, see DBUS_ARM_INSTALL below) -- NOT the full
# AASDK_INCLUDES/AASDK_LIBS stack (Boost/OpenSSL/libusb/Protobuf) that
# stays excluded from this binary for the reason above; moving
# Bluetooth connectivity into custom_ui (see docs -- all AA RFCOMM
# profile serving used to live in androidauto-sidecar, alongside its own
# separate BlueZ D-Bus connection) doesn't mean the heavy aasdk stack
# comes with it.
UI_TARGET     := $(BUILD_DIR)/custom_ui
UI_SRCS       := $(filter-out src/androidauto/% src/ui/icons.cpp src/ui/icons_m3.cpp,$(shell find src -name '*.cpp'))
UI_OBJS       := $(patsubst src/%.cpp,$(BUILD_DIR)/ui-obj/%.o,$(UI_SRCS))

# hal/bluez_aa_profile.cpp's own dbus/dbus.h include path -- not added
# to the general INCLUDES var since nothing else in this binary needs
# it; see the explicit per-file compile rule near UI_TARGET's own link
# rule below for where this is actually used.
# '=' (lazy/recursive), not ':=' -- DBUS_ARM_INSTALL itself is only
# defined further down this file (AASDK_INCLUDES/AASDK_LIBS's own
# section); Make fully parses the file before executing any rule, so a
# lazily-expanded reference here resolves fine regardless of definition
# order, whereas ':=' would have expanded DBUS_ARM_INSTALL to empty at
# this point in the file.
UI_DBUS_INCLUDES = -I$(DBUS_ARM_INSTALL)/include/dbus-1.0 -I$(DBUS_ARM_INSTALL)/lib/dbus-1.0/include
UI_DBUS_LIBS      = $(DBUS_ARM_INSTALL)/lib/libdbus-1.a
UI_DBUS_INCLUDES_DYN = -I$(BUILDROOT_STAGING)/usr/include/dbus-1.0 -I$(BUILDROOT_STAGING)/usr/lib/dbus-1.0/include
UI_DBUS_LIBS_DYN     = -L$(BUILDROOT_STAGING)/usr/lib -ldbus-1
# Same static-NSS-init crash class this project already fixed for
# bluetoothd (tools/bluetoothd-test/README.md's own "Cross-build
# dependency chain" section) linking this exact libdbus-1.a -- reusing
# tools/nss-stub/nss_stub.c as-is (the plain no-op provider; unlike
# androidauto-sidecar this binary never needs a REAL working dlopen) and
# the identical proven wrap-flag set from that README, not
# AASDK_WRAP_FLAGS (which wraps a slightly different symbol set tuned
# for the full aasdk/OpenSSL stack this binary doesn't link).
UI_NSS_STUB_OBJ   := $(BUILD_DIR)/ui-obj/nss_stub.o
UI_WRAP_FLAGS     := -Wl,--wrap=getpwnam,--wrap=getpwuid,--wrap=getpwnam_r,--wrap=getpwuid_r,--wrap=getpwent,--wrap=setpwent,--wrap=endpwent,--wrap=getgrgid,--wrap=getgrgid_r,--wrap=getgrouplist,--wrap=dlopen,--wrap=dlerror,--wrap=dlsym,--wrap=dlclose,--wrap=getaddrinfo

# src/ui/staging/'s 3 auto-generated LVGL font C files (lv_font_conv
# output, same shape as third_party/lvgl's own font sources) -- the
# only .c sources under src/ that belong to the UI binary, hence this
# being separate from UI_SRCS/UI_OBJS above (which only look for
# .cpp). Same src/androidauto/ exclusion as UI_SRCS -- that directory's
# one .c file (hantro_dlopen.c) is its own special case, already built
# via HANTRO_DLOPEN_OBJ below with different flags (no full INCLUDES),
# shared between the UI binary and androidauto-sidecar.
UI_C_SRCS     := $(filter-out src/androidauto/%,$(shell find src -name '*.c'))
UI_C_OBJS     := $(patsubst src/%.c,$(BUILD_DIR)/ui-obj/%.o,$(UI_C_SRCS))

# --- Android Auto (aasdk-backed) sources, shared by the future real
#     integration and the standalone probe test below ---
AASDK_SRCS    := $(shell find src/androidauto -name '*.cpp')
AASDK_OBJS    := $(patsubst src/%.cpp,$(BUILD_DIR)/ui-obj/%.o,$(AASDK_SRCS))

# Statically cross-compiled by third_party/build_{boost,openssl,libusb,
# protobuf,aasdk}.sh -- artifacts land outside the repo (too large to
# vendor, same reasoning as every other build_*.sh here). Override
# AASDK_DEPS_DIR if yours live somewhere other than the scripts' default.
AASDK_DEPS_DIR  ?= $(HOME)/build-deps

# 2026-08-15: separate override from AASDK_DEPS_DIR/boost/libusb/protobuf
# above -- points at a REBUILT OpenSSL (same 1.1.1w source, reconfigured
# with no-engine/no-dso/no-afalgeng/no-sock) instead of the original
# $(AASDK_DEPS_DIR)/openssl-arm-install. Reason: libcrypto.a's own
# internal dlopen/getaddrinfo/gethostbyname references were one of two
# sources of this binary's confirmed glibc static-NSS-init startup
# crash (see hantro_dlopen.c's own header comment for the full
# mechanism) -- `nm` confirms this rebuilt libcrypto.a/libssl.a
# reference neither symbol family at all anymore. Reversible: override
# this variable back to $(AASDK_DEPS_DIR)/openssl-arm-install (the
# original, untouched build) if the no-engine variant ever needs
# rolling back -- nothing about the original build was modified or
# removed.
#
# 2026-08-21: pointed at a second rebuild (openssl-arm-install-noengine-armv7,
# same no-engine/no-dso/no-afalgeng/no-sock flags, same 1.1.1w source) --
# the ORIGINAL no-engine build above was configured `linux-generic32`,
# which OpenSSL's own Configurations/10-main.conf maps to the plain-C
# fallback for AES/GHASH (aes_core.c, no modes_asm_src at all) with zero
# ARM assembly, regardless of this being a real armv7 target. Real
# phone-side symptom this caused: CPU spiking to 80%+ on every AA full-
# screen video refresh -- every channel (video included) is wrapped in
# real TLS via aasdk's Cryptor (AES-128-GCM, confirmed in
# third_party/aasdk/src/Messenger/Cryptor.cpp), and a big I-frame means
# a burst of SSL_read() calls each paying the generic-C AES-GCM cost.
# Reconfigured `linux-armv4 -march=armv7-a -mfpu=neon -D__ARM_MAX_ARCH__=8`
# instead -- this Cortex-A5 has no ARMv8 Crypto Extensions (hardware
# AES/GHASH don't exist on this core at all, not a config gap), but DOES
# have NEON, and linux-armv4's asm(armv4_asm) target pulls in real
# NEON-accelerated bit-sliced AES (bsaes-armv7.S) and NEON GHASH
# (ghash-armv4.S) with runtime CPU-capability dispatch (OPENSSL_armcap) --
# `nm` confirms bsaes_cbc_encrypt/gcm_ghash_neon/gcm_gmult_neon are now
# real defined symbols in libcrypto.a, and confirms the SAME
# dlopen/getaddrinfo/gethostbyname-free property as the original
# no-engine build (this rebuild used the identical no-engine/no-dso/
# no-afalgeng/no-sock flags). See third_party/build_openssl.sh for the
# full reproducible recipe. Reversible: override back to
# openssl-arm-install-noengine (the generic32 build, untouched) if this
# ever needs rolling back. **Not yet hardware-tested for actual CPU
# usage during AA video playback** -- confirmed to build and link
# clean, and to genuinely contain the NEON code paths, but real-device
# `top` numbers during a full-screen refresh are the real verification.
OPENSSL_ARM_INSTALL ?= $(AASDK_DEPS_DIR)/openssl-arm-install-noengine-armv7

# 2026-08-15: cross-compiled alsa-lib statically with --with-libdl=no
# (see alsa_output.h's own header comment) -- the other source of a
# real (non-libcrypto) dlopen reference in this binary (AlsaOutput used
# to dlopen the target's own libasound.so.2.0.0 at runtime). `nm`
# confirms zero dlopen/dlsym/dlclose/dlerror references in the
# resulting libasound.a.
#
# 2026-08-18: downgraded from 1.2.12 to 1.2.4 -- checked stock's own
# real deployed libasound.so.2.0.0
# (linux-arkmicro/buildroot-external/.../ArkSdk/AudioService/Alsa/
# Library/) and it's genuinely 1.2.4, an 8-minor-version gap from what
# this project had been building. Investigating irregular AA audio
# stutter (no queue drops, no repeated XRUNs -- ruling out this
# project's own buffering code and the already-hardware-confirmed
# kernel/DMA-level stutter fix, both checked first) with nothing else
# left to explain it, alsa-lib's own dmix/softvol/period-timing
# internals differing across 8 minor versions became the next most
# concrete, checkable candidate -- /etc/asound.conf topology itself is
# identical either way (this project's own alsa.conf @hooks-loads the
# device's real /etc/asound.conf, confirmed via that file's own
# content). Built from the real upstream 1.2.4 tarball with one local
# patch (see build-deps/alsa-lib-1.2.4/src/dlmisc.c's own comment --
# a genuine upstream bug in this version: two globals only declared
# under DL_ORIGIN_AVAILABLE, but unconditionally referenced by
# snd_dlpath() regardless, breaking the --with-libdl=no build; fixed
# in later versions including the 1.2.12 this project had been using,
# backported the same fix here rather than deviate from upstream).
ALSA_ARM_INSTALL ?= $(AASDK_DEPS_DIR)/alsa-arm-install-1.2.4

# 2026-08-19: cross-compiled from source, same as every other
# AASDK_DEPS_DIR entry -- no armhf apt/multiarch on this build machine.
# See tools/bluetoothd-test/README.md's "Cross-build dependency chain"
# section for the full build recipe.
#
# 2026-08-20: NO LONGER part of AASDK_INCLUDES/AASDK_LIBS below --
# bluez_client.cpp (the one D-Bus consumer that needed it) moved out of
# src/androidauto/ entirely, to src/hal/bluez_aa_profile.cpp (see
# UI_TARGET's own comment near UI_DBUS_INCLUDES/UI_DBUS_LIBS above) --
# Bluetooth connectivity is custom_ui's job now, not
# androidauto-sidecar's. This var stays defined here (still the one
# place DBUS_ARM_INSTALL's actual path is configured, UI_DBUS_INCLUDES/
# UI_DBUS_LIBS reference it lazily) but nothing in src/androidauto/
# references dbus/dbus.h anymore -- confirmed via grep before making
# this change.
DBUS_ARM_INSTALL ?= $(AASDK_DEPS_DIR)/dbus-arm-install

AASDK_DIR       := third_party/aasdk
AASDK_INCLUDES  := -I$(AASDK_DIR)/include \
                    -I$(AASDK_DIR)/build-arm/protobuf \
                    -I$(AASDK_DEPS_DIR)/boost-arm-install/include \
                    -I$(OPENSSL_ARM_INSTALL)/include \
                    -I$(AASDK_DEPS_DIR)/libusb-arm-install/include \
                    -I$(AASDK_DEPS_DIR)/libusb-arm-install/include/libusb-1.0 \
                    -I$(AASDK_DEPS_DIR)/protobuf-arm-install/include \
                    -I$(ALSA_ARM_INSTALL)/include \
                    -Ithird_party/bluez_uapi
# --start-group/--end-group: aasdk, aap_protobuf, protobuf, and Abseil
# static-link against each other in ways plain left-to-right link order
# can't always resolve in one pass -- --start-group lets ld retry the
# whole set until every symbol resolves.
AASDK_LIBS      := -Wl,--start-group \
                    $(AASDK_DIR)/build-arm/lib/libaasdk.a \
                    $(AASDK_DIR)/build-arm/lib/libaap_protobuf.a \
                    $(wildcard $(AASDK_DEPS_DIR)/boost-arm-install/lib/*.a) \
                    $(AASDK_DEPS_DIR)/libusb-arm-install/lib/libusb-1.0.a \
                    $(OPENSSL_ARM_INSTALL)/lib/libssl.a \
                    $(OPENSSL_ARM_INSTALL)/lib/libcrypto.a \
                    $(ALSA_ARM_INSTALL)/lib/libasound.a \
                    $(wildcard $(AASDK_DEPS_DIR)/protobuf-arm-install/lib/*.a) \
                    -Wl,--end-group -lpthread -ldl -lrt

# --- Dynamic variant, androidauto-sidecar only (Phase 4) ---
# 2026-08-21: Boost/libusb/Protobuf/aasdk itself REBUILT against the
# matching Linaro 7.3.1 toolchain (see merry-snacking-wirth.md's Phase
# 4 rebuild-scope notes) -- the earlier "keep them static from the old
# AASDK_DEPS_DIR" design was tried first and failed at real link time
# (undefined reference to __libc_single_threaded, a glibc-2.32+ symbol
# the old system-toolchain .a files referenced; libstdc++ symbol-
# versioning mismatches on std::__throw_bad_array_new_length/
# exception_ptr). AASDK_DEPS_DIR_DYN (a separate deps tree,
# $(HOME)/build-deps-dyn by default) and AASDK_DIR's own
# build-arm-dyn/ (NOT build-arm/, which the 4 still-static
# androidauto-*-test tools also link against by that same fixed path --
# a real conflict hit and fixed 2026-08-21, see build_aasdk.sh's own
# AASDK_BUILD_SUBDIR comment) keep this fully separate from the static
# AASDK_DEPS_DIR/AASDK_LIBS above.
#
# openssl: still from Buildroot's own staging sysroot (2.25-toolchain-
# native already) -- Buildroot's libopeonssl.mk (this 2019.05-rc1 tree)
# hardcodes `no-shared` for the TARGET build regardless of
# BR2_STATIC_LIBS, confirmed via direct inspection, so it stays static
# either way; no reason to duplicate it into AASDK_DEPS_DIR_DYN too.
# 2026-08-24: Boost/Protobuf/libusb/aasdk itself REBUILT (again) as real
# SHARED libraries (LINK_SHARED=1 on every build_*.sh, see their own
# header comments) -- real motivation: two reproducible OOM-killer
# kills of androidauto-sidecar this session even after the mlockall()
# fix, because none of this ~15-20MB of static code was page-shareable
# with anything else on this 173MB/no-swap device. Assessed and ruled
# out using Buildroot's own bundled protobuf package first (3.6.0,
# predates Abseil entirely -- a hard architectural incompatibility with
# aasdk's real v25.3/v30.0 requirement, not a version nuance) -- these
# are still this project's own cross-builds, at the exact same already-
# hardware-proven versions, just flipped to shared, in their own fully
# separate deps tree ($(HOME)/build-deps-dyn-shared) and aasdk build
# dir (build-arm-dyn-shared/, NOT build-arm-dyn/, which the previous
# static-Boost dyn build used and which nothing else references
# anymore). AASDK's own upstream CMakeLists.txt actually defaults to
# SHARED on non-macOS -- build_aasdk.sh's LINK_SHARED=1 path mostly
# just stops overriding that default, rather than fighting it.
#
# openssl: still static from Buildroot's own staging sysroot regardless
# (Buildroot's libopenssl.mk, this 2019.05-rc1 tree, hardcodes
# `no-shared` for the TARGET build, confirmed via direct inspection) --
# no reason to duplicate it into the shared deps tree.
AASDK_DEPS_DIR_DYN ?= $(HOME)/build-deps-dyn-shared
AASDK_INCLUDES_DYN := -I$(AASDK_DIR)/include \
                    -I$(AASDK_DIR)/build-arm-dyn-shared/protobuf \
                    -I$(AASDK_DEPS_DIR_DYN)/boost-arm-install/include \
                    -I$(BUILDROOT_STAGING)/usr/include \
                    -I$(AASDK_DEPS_DIR_DYN)/libusb-arm-install/include \
                    -I$(AASDK_DEPS_DIR_DYN)/libusb-arm-install/include/libusb-1.0 \
                    -I$(AASDK_DEPS_DIR_DYN)/protobuf-arm-install/include \
                    -I$(BUILDROOT_STAGING)/usr/include/dbus-1.0 \
                    -I$(BUILDROOT_STAGING)/usr/lib/dbus-1.0/include \
                    -Ithird_party/bluez_uapi
# AASDK_SHARED_LIB_DIR: where the real .so's this binary needs at
# runtime actually live -- staged into the rootfs overlay's /usr/lib
# (already on this device's LD_LIBRARY_PATH, see /etc/profile) by the
# deploy script, not a Buildroot-managed location. -rpath-link (build-
# time only, doesn't end up in the binary) lets the cross-linker
# resolve these against the real .so's instead of erroring on unlinked
# transitive symbols; the runtime search path comes from
# LD_LIBRARY_PATH on-device, not this flag.
# --as-needed: build_boost.sh/build_protobuf.sh build (and this line
# wildcards in) EVERY Boost/Abseil component, not just what aasdk
# actually uses (same "install() needs everything built" reason as the
# static .a wildcard this replaced) -- but unlike static linking, where
# only referenced object code gets pulled in, explicitly listing every
# .so on the link line makes the linker mark ALL of them NEEDED
# regardless of real symbol use. --as-needed drops any of these from
# the final NEEDED list that nothing in the binary actually
# references, keeping both the deployed footprint and the runtime
# dependency graph honest. Re-enabled to the default (--no-as-needed)
# before the last few libs, which we do want unconditionally.
AASDK_LIBS_DYN  := -Wl,--as-needed \
                    $(AASDK_DIR)/build-arm-dyn-shared/lib/libaasdk.so \
                    $(AASDK_DIR)/build-arm-dyn-shared/lib/libaap_protobuf.so \
                    $(wildcard $(AASDK_DEPS_DIR_DYN)/boost-arm-install/lib/*.so) \
                    $(AASDK_DEPS_DIR_DYN)/libusb-arm-install/lib/libusb-1.0.so \
                    $(BUILDROOT_STAGING)/usr/lib/libssl.a \
                    $(BUILDROOT_STAGING)/usr/lib/libcrypto.a \
                    $(wildcard $(AASDK_DEPS_DIR_DYN)/protobuf-arm-install/lib/*.so) \
                    -Wl,--no-as-needed \
                    -Wl,-rpath-link,$(AASDK_DIR)/build-arm-dyn-shared/lib \
                    -Wl,-rpath-link,$(AASDK_DEPS_DIR_DYN)/boost-arm-install/lib \
                    -Wl,-rpath-link,$(AASDK_DEPS_DIR_DYN)/libusb-arm-install/lib \
                    -Wl,-rpath-link,$(AASDK_DEPS_DIR_DYN)/protobuf-arm-install/lib \
                    -L$(BUILDROOT_STAGING)/usr/lib -lasound -ldbus-1 \
                    -lpthread -ldl -lrt -lstdc++fs

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
# getpwuid_r/getgrnam_r added 2026-08-15: alsa-lib's dmix/userfile
# plugins (now statically linked -- see ALSA_ARM_INSTALL above)
# reference these two reentrant pwd/grp lookups, same broken
# static-NSS-init trigger family as dlopen/getaddrinfo -- confirmed via
# this project's own build output ("requires at runtime the shared
# libraries..." linker warnings). Stubbed in both
# tools/nss-stub/nss_stub_busybox.c (the other 4 targets below) and
# hantro_dlopen.c (androidauto-sidecar's own replacement, see its own
# header comment).
# getgrouplist added 2026-08-19 for libdbus's fill_user_info() -- libdbus
# itself no longer links into androidauto-sidecar as of 2026-08-20 (see
# DBUS_ARM_INSTALL's own comment -- bluez_client.cpp moved to
# src/hal/bluez_aa_profile.cpp, linked into custom_ui instead), so this
# specific symbol is likely vestigial now. Left in place rather than
# removed untested -- --wrap on a symbol nothing actually references is
# a harmless no-op, and hantro_dlopen.c's __wrap_getgrouplist (this
# binary's actual wrap provider) still exists regardless.
AASDK_WRAP_FLAGS := -Wl,--wrap=dlopen,--wrap=dlerror,--wrap=dlsym,--wrap=dlclose,--wrap=getaddrinfo,--wrap=freeaddrinfo,--wrap=gai_strerror,--wrap=gethostbyname,--wrap=gethostbyname2,--wrap=gethostbyaddr,--wrap=getservbyname,--wrap=getservbyport,--wrap=getpwuid_r,--wrap=getpwnam_r,--wrap=getgrnam_r,--wrap=getgrouplist

# 2026-08-15: androidauto-sidecar specifically (not the other 4 targets
# below that also use AASDK_WRAP_FLAGS/NSS_STUB_OBJ) needs a REAL,
# working dlopen -- HantroH264Decoder dlopen()s /usr/lib/libmfc.so for
# real hardware video decode. See hantro_dlopen.c's own top comment for
# why NSS_STUB_OBJ's plain no-op stub can't just be swapped for a
# passthrough to the real glibc dlopen() (the same static-NSS-init
# startup crash NSS_STUB_OBJ exists to avoid comes right back the
# moment anything in the binary references the real symbol, whether or
# not that path is ever taken at runtime). HANTRO_DLOPEN_OBJ provides
# the same __wrap_* symbol set as NSS_STUB_OBJ (so it's a drop-in
# linker substitute) but with a real, working ELF loader for dlopen/
# dlsym/dlclose instead of stubs -- androidauto-sidecar's own link rule
# below uses this instead of NSS_STUB_OBJ; the other 4 AASDK_WRAP_FLAGS
# targets are unaffected.
# Only androidauto-sidecar links this (no test tool does) -- points at
# the new dynamic-build object tree directly (Phase 4), no separate
# static copy needed.
HANTRO_DLOPEN_OBJ := $(BUILD_DIR)/androidauto-sidecar-obj/androidauto/hantro_dlopen.o

# --- carplay sidecar binary ---
# -ldbus-1 goes back in once this actually links libdbus -- needs its
# headers/lib sourced from the target rootfs or cross-built, neither of
# which is set up yet (Phase 7, deferred -- see docs/IMPLEMENTATION_PLAN.md).
# Doesn't link LVGL at all -- this process has no UI of its own.
SIDECAR_TARGET := $(BUILD_DIR)/carplay-sidecar
SIDECAR_SRCS   := $(shell find sidecars/carplay -name '*.cpp')
SIDECAR_OBJS   := $(patsubst sidecars/carplay/%.cpp,$(BUILD_DIR)/sidecar-obj/%.o,$(SIDECAR_SRCS))
SIDECAR_INCLUDES := -Isidecars/carplay

# --- androidauto-sidecar: real Android Auto session, own process ---
# See sidecars/androidauto/main.cpp and src/hal/androidauto_client.h --
# owns androidauto::WirelessSessionManager (and therefore all of
# AASDK_OBJS/AASDK_LIBS) in isolation from the main UI binary, exposing
# a small local socket protocol instead. Shares the exact same
# dependency set as the probe-test targets below.
ANDROIDAUTO_SIDECAR_TARGET := $(BUILD_DIR)/androidauto-sidecar
ANDROIDAUTO_SIDECAR_OBJS   := $(BUILD_DIR)/androidauto-sidecar-obj/main.o

# core/hal_config.cpp (see src/core/hal_config.h) lives under src/core/,
# so it's part of UI_SRCS/UI_OBJS (the main custom_ui binary) but NOT
# AASDK_SRCS/AASDK_OBJS (src/androidauto/ only) -- yet
# androidauto/wireless_session_manager.cpp, which IS in AASDK_SRCS,
# reads it too. Pulled in explicitly here rather than moving the file
# into src/androidauto/, since it's genuinely shared, non-aasdk-specific
# code (custom_ui's own hal/bluetooth.cpp uses it as well).
#
# Same cross-binary situation for hal/video_layer.cpp (src/hal/video_layer.h,
# UI_SRCS, but androidauto/video_channel.cpp in AASDK_SRCS calls into it
# to push decoded frames to the hardware video-overlay layer) and
# core/log_timing.cpp (custom_ui's own process-wide kernel-dmesg-style
# timestamp, called from both files above -- androidauto-sidecar has
# its own separate androidauto::log_timing, different namespace/process,
# not this one). See HAL_CONFIG_OBJ_DYN/VIDEO_LAYER_OBJ_DYN/
# LOG_TIMING_CORE_OBJ_DYN below (Phase 4) for how these actually get
# built now -- both `ui` and `androidauto-sidecar` are dynamic, so
# there's no longer a static variant of these 3 objects to keep around.

# --- Dynamic-build object trees for `ui`/`androidauto-sidecar` (Phase 4) ---
# CANNOT just point the existing ui-obj/ pattern rules at CXX_DYN: that
# directory's object files (AASDK_OBJS/HAL_CONFIG_OBJ/VIDEO_LAYER_OBJ/
# LOG_TIMING_CORE_OBJ above, same file paths) are ALSO linked into the
# androidauto-usb-probe-test/androidauto-wireless-probe-test/
# androidauto-bluetooth-rfcomm-test/androidauto-bw-aap-test targets,
# which stay on the static/system-toolchain build -- e.g. AASDK_OBJS's
# own `find src/androidauto -name '*.cpp'` glob includes
# bluetooth_transport.cpp/bw_aap_client.cpp, the exact same files
# BT_TRANSPORT_OBJS/BW_AAP_CLIENT_OBJS reference by the same object
# path. Compiling that one file two different, toolchain-incompatible
# ways under one path is not possible -- `ui`/`androidauto-sidecar` get
# their own parallel object trees (ui-dyn-obj/,
# androidauto-sidecar-obj/androidauto/) instead, so nothing shared with
# the 4 still-static tools ever gets touched.
UI_OBJS_DYN          := $(patsubst src/%.cpp,$(BUILD_DIR)/ui-dyn-obj/%.o,$(UI_SRCS))
UI_C_OBJS_DYN        := $(patsubst src/%.c,$(BUILD_DIR)/ui-dyn-obj/%.o,$(UI_C_SRCS))
AASDK_OBJS_DYN       := $(patsubst src/%.cpp,$(BUILD_DIR)/androidauto-sidecar-obj/%.o,$(AASDK_SRCS))
HAL_CONFIG_OBJ_DYN   := $(BUILD_DIR)/ui-dyn-obj/core/hal_config.o
VIDEO_LAYER_OBJ_DYN  := $(BUILD_DIR)/ui-dyn-obj/hal/video_layer.o
LOG_TIMING_CORE_OBJ_DYN := $(BUILD_DIR)/ui-dyn-obj/core/log_timing.o
# LVGL_OBJS (lvgl-obj/) is exclusively used by UI_TARGET already (no
# test tool links LVGL) -- its existing pattern rule is switched to
# CC_DYN directly below rather than duplicated into a third tree.

# --- androidauto-usb-probe-test: standalone aasdk link-graph proof ---
# See tools/androidauto-usb-probe-test/main.cpp -- not part of `all`,
# build explicitly with `make androidauto-usb-probe-test`.
AASDK_TEST_TARGET := $(BUILD_DIR)/androidauto-usb-probe-test
AASDK_TEST_OBJS    := $(BUILD_DIR)/aasdk-test-obj/main.o

# --- androidauto-wireless-probe-test: Session over TCPTransport ---
# See tools/androidauto-wireless-probe-test/main.cpp. Shares
# AASDK_LIBS/AASDK_WRAP_FLAGS/NSS_STUB_OBJ with the USB probe test --
# same dependency set (this is TCP, not USB, but still needs
# libusb linked in since it's part of AASDK_LIBS's --start-group).
WIRELESS_TEST_TARGET := $(BUILD_DIR)/androidauto-wireless-probe-test
WIRELESS_TEST_OBJS   := $(BUILD_DIR)/wireless-test-obj/main.o

# --- androidauto-bluetooth-rfcomm-test: raw RFCOMM socket-layer proof ---
# SUPERSEDED as the wireless-AA discovery approach (kept buildable,
# still legitimate generic code) -- built on the assumption of a
# standard Linux BlueZ stack (AF_BLUETOOTH/BTPROTO_RFCOMM kernel
# sockets + our own SDP server), but real captured traffic
# (docs/logs/android auto log v{1,2,3}.txt) confirmed this device's
# actual Bluetooth stack is Feasycom's closed `blueware` daemon, which
# exposes a local Unix socket (/dev/bw_aap) instead -- see
# BW_AAP_TEST_TARGET below, that's the real path forward.
# See tools/androidauto-bluetooth-rfcomm-test/main.cpp -- NOT a full
# Android Auto session, just the socket plumbing (BluetoothRFCOMMTransport
# + accept_rfcomm_connection). Only needs libpthread, not the full
# AASDK_LIBS -- doesn't touch Boost/OpenSSL/Protobuf/aasdk's Messenger
# stack at all, just aasdk::transport::Transport's base class.
BT_TEST_TARGET := $(BUILD_DIR)/androidauto-bluetooth-rfcomm-test
BT_TEST_OBJS   := $(BUILD_DIR)/bt-test-obj/main.o
BT_TRANSPORT_OBJS := $(BUILD_DIR)/ui-obj/androidauto/bluetooth_transport.o \
                      $(BUILD_DIR)/ui-obj/androidauto/bluetooth_rfcomm_server.o

# --- androidauto-bw-aap-test: real /dev/bw_aap wireless-AA discovery ---
# See tools/androidauto-bw-aap-test/main.cpp and
# src/androidauto/wifi_setup_client.h (renamed 2026-08-21 from
# bw_aap_client.h/BwAapClient -- see that file's own header comment)
# for the full story -- this is the confirmed-real path (see comment
# above), replaying an actual captured message sequence against
# blueware's own local socket.
BW_AAP_TEST_TARGET := $(BUILD_DIR)/androidauto-bw-aap-test
BW_AAP_TEST_OBJS   := $(BUILD_DIR)/bw-aap-test-obj/main.o
BW_AAP_CLIENT_OBJS := $(BUILD_DIR)/ui-obj/androidauto/wifi_setup_client.o

.PHONY: all clean ui carplay-sidecar androidauto-sidecar androidauto-usb-probe-test androidauto-wireless-probe-test androidauto-bluetooth-rfcomm-test androidauto-bw-aap-test

all: ui carplay-sidecar androidauto-sidecar

ui: $(UI_TARGET) $(BUILD_DIR)/hal.conf $(BUILD_DIR)/default_settings.conf
carplay-sidecar: $(SIDECAR_TARGET)
androidauto-sidecar: $(ANDROIDAUTO_SIDECAR_TARGET) $(BUILD_DIR)/hal.conf $(BUILD_DIR)/alsa/alsa.conf

# Staged copy of the tracked config (see src/core/hal_config.h) right
# next to the compiled binaries -- both `ui` and `androidauto-sidecar`
# depend on this so scp'ing build/ to the device (this project's real
# test workflow, see scripts/run_on_device.sh) carries the config along
# automatically, no /etc or /data setup needed on-device.
$(BUILD_DIR)/hal.conf: etc/hal.conf
	@mkdir -p $(dir $@)
	cp $< $@

# Staged copy of the bundled settings seed (see src/core/config_store.h)
# -- same reasoning as hal.conf above. Only `ui` reads this (androidauto-
# sidecar has no ConfigStore dependency).
$(BUILD_DIR)/default_settings.conf: etc/default_settings.conf
	@mkdir -p $(dir $@)
	cp $< $@

# Staged copy of alsa-lib's own config tree (alsa.conf + cards/ctl/pcm)
# right next to the compiled binaries -- see sidecars/androidauto/main.cpp's
# own comment: this process resolves ALSA_CONFIG_PATH relative to its
# OWN executable directory (/proc/self/exe), same "no fixed install
# path yet" convention hal.conf/default_settings.conf above already
# use, rather than a real rootfs path like /usr/share/alsa (which this
# project has no fixed, real install location for yet either -- see
# hal/androidauto_client.cpp's trySpawnSidecar() comment). Real source
# tree is etc/alsa/ (a copy of $(ALSA_ARM_INSTALL)/share/alsa/ from the
# ALSA cross-build, not regenerated automatically -- see that
# directory's own provenance if it ever needs refreshing after an
# alsa-lib version bump).
$(BUILD_DIR)/alsa/alsa.conf: $(shell find etc/alsa -type f)
	@mkdir -p $(dir $@)
	cp -a etc/alsa/. $(BUILD_DIR)/alsa/
androidauto-usb-probe-test: $(AASDK_TEST_TARGET)
androidauto-wireless-probe-test: $(WIRELESS_TEST_TARGET)
androidauto-bluetooth-rfcomm-test: $(BT_TEST_TARGET)
androidauto-bw-aap-test: $(BW_AAP_TEST_TARGET)

# --- ui (dynamic, Phase 4) ---
$(UI_TARGET): $(UI_OBJS_DYN) $(UI_C_OBJS_DYN) $(LVGL_OBJS)
	@mkdir -p $(dir $@)
	$(CXX_DYN) $(LDFLAGS_DYN) -o $@ $^ $(UI_DBUS_LIBS_DYN) $(LIBS_DYN)

# hal/bluez_aa_profile.cpp, hal/ble_cts.cpp, and hal/bluetooth.cpp need
# dbus/dbus.h too on this path -- same explicit-rule reasoning as the
# static ui-obj/ versions below, just with the dynamic toolchain/obj
# dir. UI_DBUS_INCLUDES_DYN (headers from Buildroot staging sysroot)
# and UI_DBUS_LIBS_DYN (-ldbus-1 against Buildroot staging).
$(BUILD_DIR)/ui-dyn-obj/hal/bluez_aa_profile.o: src/hal/bluez_aa_profile.cpp
	@mkdir -p $(dir $@)
	$(CXX_DYN) $(CXXFLAGS) $(UI_DBUS_INCLUDES_DYN) -c -o $@ $<

$(BUILD_DIR)/ui-dyn-obj/hal/ble_cts.o: src/hal/ble_cts.cpp
	@mkdir -p $(dir $@)
	$(CXX_DYN) $(CXXFLAGS) $(UI_DBUS_INCLUDES_DYN) -c -o $@ $<

$(BUILD_DIR)/ui-dyn-obj/hal/bluetooth.o: src/hal/bluetooth.cpp
	@mkdir -p $(dir $@)
	$(CXX_DYN) $(CXXFLAGS) $(UI_DBUS_INCLUDES_DYN) -c -o $@ $<

$(BUILD_DIR)/ui-dyn-obj/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX_DYN) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/ui-dyn-obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC_DYN) $(CFLAGS) -c -o $@ $<

# Static path (unchanged): still needed for androidauto-usb-probe-test/
# androidauto-wireless-probe-test/androidauto-bluetooth-rfcomm-test/
# androidauto-bw-aap-test, which all still link AASDK_OBJS (or a
# subset, BT_TRANSPORT_OBJS/BW_AAP_CLIENT_OBJS) built via this exact
# pattern -- see the ui-dyn-obj/androidauto-sidecar-obj comment block
# above for why this can't just be redirected wholesale.
#
# hal/bluez_aa_profile.cpp, hal/ble_cts.cpp, and hal/bluetooth.cpp are
# the files in the main UI binary that need dbus/dbus.h -- explicit
# (non-pattern) rules, which Make prefers over the generic
# $(BUILD_DIR)/ui-obj/%.o pattern rule above, same convention this
# Makefile already uses for HANTRO_DLOPEN_OBJ/HAL_CONFIG_OBJ etc.
$(BUILD_DIR)/ui-obj/hal/bluez_aa_profile.o: src/hal/bluez_aa_profile.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(UI_DBUS_INCLUDES) -c -o $@ $<

$(BUILD_DIR)/ui-obj/hal/ble_cts.o: src/hal/ble_cts.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(UI_DBUS_INCLUDES) -c -o $@ $<

# 2026-08-21: bluez_monitor_loop() (hal/bluetooth.cpp) moved from
# shelling out to `dbus-send` via popen() every 1.5s to a persistent
# libdbus connection subscribed to real BlueZ signals -- needs
# dbus/dbus.h now too, same as the two files above.
$(BUILD_DIR)/ui-obj/hal/bluetooth.o: src/hal/bluetooth.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(UI_DBUS_INCLUDES) -c -o $@ $<

$(UI_NSS_STUB_OBJ): ../tools/nss-stub/nss_stub.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -c -o $@ $<

$(BUILD_DIR)/ui-obj/androidauto/%.o: src/androidauto/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(AASDK_INCLUDES) -c -o $@ $<

# Dynamic path (Phase 4): androidauto-sidecar's own copy of the same
# src/androidauto/*.cpp sources, compiled with the new toolchain/
# includes into a separate object tree so nothing here is shared with
# the static path above.
$(BUILD_DIR)/androidauto-sidecar-obj/androidauto/%.o: src/androidauto/%.cpp
	@mkdir -p $(dir $@)
	$(CXX_DYN) $(CXXFLAGS) $(AASDK_INCLUDES_DYN) -c -o $@ $<

# --- androidauto-sidecar (dynamic, Phase 4) ---
# No AASDK_WRAP_FLAGS/NSS_STUB_OBJ -- that workaround exists only for
# the static-glibc-NSS-init crash class (glibc >= 2.34, see
# tools/nss-stub/README.md), which doesn't apply once dynamic against
# this rootfs's own glibc 2.25 toolchain (CXX_DYN above). Unverified
# until this actually links/runs -- if a real static-NSS-style crash
# somehow still shows up, HANTRO_DLOPEN_OBJ's real dlopen() (still
# needed regardless, for libmfc.so) or a reintroduced wrap would be the
# fix, not assumed unnecessary without a real hardware test.
$(ANDROIDAUTO_SIDECAR_TARGET): $(ANDROIDAUTO_SIDECAR_OBJS) $(AASDK_OBJS_DYN) $(HAL_CONFIG_OBJ_DYN) $(VIDEO_LAYER_OBJ_DYN) $(LOG_TIMING_CORE_OBJ_DYN) $(HANTRO_DLOPEN_OBJ)
	@mkdir -p $(dir $@)
	$(CXX_DYN) $(LDFLAGS_DYN) -o $@ $^ $(AASDK_LIBS_DYN)

$(BUILD_DIR)/androidauto-sidecar-obj/%.o: sidecars/androidauto/%.cpp
	@mkdir -p $(dir $@)
	$(CXX_DYN) $(CXXFLAGS) $(AASDK_INCLUDES_DYN) -Isrc -c -o $@ $<

$(HANTRO_DLOPEN_OBJ): src/androidauto/hantro_dlopen.c
	@mkdir -p $(dir $@)
	$(CC_DYN) -O2 -c -o $@ $<

$(AASDK_TEST_TARGET): $(AASDK_TEST_OBJS) $(AASDK_OBJS) $(NSS_STUB_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) $(AASDK_WRAP_FLAGS) -o $@ $^ $(AASDK_LIBS)

$(BUILD_DIR)/aasdk-test-obj/%.o: tools/androidauto-usb-probe-test/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(AASDK_INCLUDES) -Isrc -c -o $@ $<

$(NSS_STUB_OBJ): $(NSS_STUB_SRC)
	@mkdir -p $(dir $@)
	$(CC) -O2 -c -o $@ $<

$(WIRELESS_TEST_TARGET): $(WIRELESS_TEST_OBJS) $(AASDK_OBJS) $(NSS_STUB_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) $(AASDK_WRAP_FLAGS) -o $@ $^ $(AASDK_LIBS)

$(BUILD_DIR)/wireless-test-obj/%.o: tools/androidauto-wireless-probe-test/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(AASDK_INCLUDES) -Isrc -c -o $@ $<

$(BT_TEST_TARGET): $(BT_TEST_OBJS) $(BT_TRANSPORT_OBJS) $(NSS_STUB_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) $(AASDK_WRAP_FLAGS) -o $@ $^ $(AASDK_LIBS)

$(BUILD_DIR)/bt-test-obj/%.o: tools/androidauto-bluetooth-rfcomm-test/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(AASDK_INCLUDES) -Isrc -c -o $@ $<

$(BW_AAP_TEST_TARGET): $(BW_AAP_TEST_OBJS) $(BW_AAP_CLIENT_OBJS) $(NSS_STUB_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) $(AASDK_WRAP_FLAGS) -o $@ $^ $(AASDK_LIBS)

$(BUILD_DIR)/bw-aap-test-obj/%.o: tools/androidauto-bw-aap-test/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(AASDK_INCLUDES) -Isrc -c -o $@ $<

# CC_DYN (Phase 4): LVGL is exclusively linked by UI_TARGET (no test
# tool uses it), safe to switch wholesale -- no static/dynamic split
# needed here unlike ui-obj/androidauto above.
$(BUILD_DIR)/lvgl-obj/%.o: $(LVGL_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC_DYN) $(CFLAGS) -c -o $@ $<

$(SIDECAR_TARGET): $(SIDECAR_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

$(BUILD_DIR)/sidecar-obj/%.o: sidecars/carplay/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(SIDECAR_INCLUDES) -c -o $@ $<

clean:
	rm -rf $(BUILD_DIR)

# Pulls in every .d file any compile rule above has generated (see
# CXXFLAGS/CFLAGS's -MMD -MP) so header changes are tracked across all
# the different object-output directories (ui-obj, androidauto-sidecar-obj,
# sidecar-obj, the various tools/ test-obj dirs, lvgl-obj...) without
# listing each one by hand. `-include` (not `include`) so a clean
# checkout with no .d files yet doesn't fail the very first build.
-include $(shell find $(BUILD_DIR) -name '*.d' 2>/dev/null)
