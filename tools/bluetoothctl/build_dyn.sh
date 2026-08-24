#!/bin/bash
# Dynamic-linked build of bluetoothctl for the new `dyn` rootfs
# (ark1668_ft_dyn_defconfig). 2026-08-24: real hw finding -- unlike
# bluetoothd itself (a whole daemon with a 7-component dependency
# chain, assessed and deferred earlier this session), bluetoothctl is
# a single self-contained client binary whose deps (glib2, readline,
# dbus, ncurses) are all common Buildroot packages already enabled
# for this defconfig (BR2_PACKAGE_LIBGLIB2/READLINE added alongside
# the existing DBUS/NCURSES). No static-glibc-NSS crash class applies
# once dynamic, so tools/nss-stub's --wrap= workaround is dropped too.
#
# Real dependency note: this Buildroot's glib2 is 2.56.4, built with
# --with-pcre=system against PCRE1 (see libglib2.mk), NOT PCRE2 -- the
# OLD static build's -lpcre2-8 was only needed because that build's
# glib was compiled against pcre2 on a different host. bluetoothctl's
# own source has no direct pcre2 reference. This Buildroot's ncurses
# also has no separate libtinfo -- tinfo is merged into libncurses
# itself, so link against -lncurses, not -ltinfo/-ltinfow.
set -euo pipefail

BUILDROOT_OUTPUT_DIR="${BUILDROOT_OUTPUT_DIR:-$HOME/Downloads/linux-arkmicro/buildroot/output}"
STAGE="$BUILDROOT_OUTPUT_DIR/staging"
CC="$BUILDROOT_OUTPUT_DIR/host/bin/arm-buildroot-linux-gnueabihf-gcc"
STRIP="$BUILDROOT_OUTPUT_DIR/host/bin/arm-buildroot-linux-gnueabihf-strip"

SRCS="src/client/main.c src/client/display.c src/client/agent.c src/client/gatt.c \
src/client/advertising.c src/client/adv_monitor.c src/client/admin.c src/client/player.c \
src/gdbus/object.c src/gdbus/watch.c src/gdbus/mainloop.c src/gdbus/polkit.c src/gdbus/client.c \
src/src/shared/shell.c src/src/shared/util.c src/src/shared/io-glib.c src/src/shared/timeout-glib.c \
src/src/shared/mainloop-glib.c src/src/shared/mainloop-notify.c src/src/shared/queue.c src/src/shared/log.c \
src/lib/bluetooth.c src/lib/hci.c"

"$CC" -O2 \
  -Isrc -Isrc/src -Isrc/lib -Isrc/client \
  -I"$STAGE/usr/include" -I"$STAGE/usr/include/glib-2.0" -I"$STAGE/usr/lib/glib-2.0/include" \
  -I"$STAGE/usr/include/dbus-1.0" -I"$STAGE/usr/lib/dbus-1.0/include" \
  -DVERSION=\"5.66\" \
  $SRCS \
  -L"$STAGE/usr/lib" -lreadline -lncurses -ldbus-1 -lglib-2.0 -lgobject-2.0 -lm -lpthread \
  -o bluetoothctl.dyn

"$STRIP" -s bluetoothctl.dyn
echo "Built: $(pwd)/bluetoothctl.dyn ($(du -h bluetoothctl.dyn | cut -f1))"
