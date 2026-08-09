/*
 * Minimal vendored subset of BlueZ's public rfcomm.h UAPI header --
 * see bluetooth.h in this same directory for why this is vendored
 * instead of installed.
 */
#pragma once

#include "bluetooth.h"

struct sockaddr_rc {
    sa_family_t rc_family;
    bdaddr_t rc_bdaddr;
    uint8_t rc_channel;
} __attribute__((packed));
