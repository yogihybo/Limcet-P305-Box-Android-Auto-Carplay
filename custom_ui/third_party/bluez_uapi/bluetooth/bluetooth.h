/*
 * Minimal vendored subset of BlueZ's public bluetooth.h UAPI header.
 * This build host has no bluez-dev/libbluetooth-dev package installed
 * and no root access to install one (same constraint documented
 * throughout custom_ui/third_party/build_*.sh), and the cross
 * toolchain's sysroot doesn't bundle it either. Rather than pull in
 * all of BlueZ just for a handful of stable, unchanged-in-20-years
 * socket constants, this vendors only what's needed for a raw RFCOMM
 * socket: AF_BLUETOOTH/BTPROTO_RFCOMM and the bdaddr_t type. These
 * match BlueZ's own public header exactly (well-documented, stable
 * kernel/userspace ABI, not something that varies by BlueZ version).
 */
#pragma once

#include <cstdint>
#include <cstdio>
#include <sys/socket.h>

#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#ifndef PF_BLUETOOTH
#define PF_BLUETOOTH AF_BLUETOOTH
#endif

#define BTPROTO_RFCOMM 3

typedef struct {
    uint8_t b[6];
} __attribute__((packed)) bdaddr_t;

static inline int bt_str2ba(const char *str, bdaddr_t *ba) {
    unsigned int b[6];
    if (sscanf(str, "%2x:%2x:%2x:%2x:%2x:%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return -1;
    }
    for (int i = 0; i < 6; i++) {
        ba->b[i] = static_cast<uint8_t>(b[5 - i]);
    }
    return 0;
}
