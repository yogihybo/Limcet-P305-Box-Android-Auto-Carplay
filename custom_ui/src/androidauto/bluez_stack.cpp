#include "androidauto/bluez_stack.h"
#include "androidauto/log_timing.h"

#include <cstdio>
#include <cstdlib>

namespace androidauto {

bool bluez_stack_start() {
    std::printf("%s androidauto: bluez_stack: starting (stopping blueware first)\n",
               logTimestamp().c_str());
    int rc = std::system("/usr/bin/bluez-bringup.sh start");
    if (rc != 0) {
        std::fprintf(stderr, "%s androidauto: bluez_stack: bluez-bringup.sh start failed (rc=%d) "
                     "-- see /var/log/rtk_hciattach.log, /var/log/dbus-daemon.log, "
                     "/var/log/bluetoothd.log\n", logTimestamp().c_str(), rc);
        return false;
    }
    std::printf("%s androidauto: bluez_stack: ready\n", logTimestamp().c_str());
    return true;
}

void bluez_stack_stop() {
    std::printf("%s androidauto: bluez_stack: stopping (freeing chip back for blueware)\n",
               logTimestamp().c_str());
    std::system("/usr/bin/bluez-bringup.sh stop");
}

}  // namespace androidauto
