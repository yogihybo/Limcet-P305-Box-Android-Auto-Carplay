#include "androidauto/bluez_stack.h"
#include "androidauto/log_timing.h"

#include <cstdio>
#include <cstdlib>

namespace androidauto {

bool bluez_stack_start() {
    if (std::system("pidof bluetoothd >/dev/null 2>&1") == 0) {
        std::printf("%s androidauto: bluez_stack: BlueZ stack already running and active\n",
                   logTimestamp().c_str());
        return true;
    }
    std::printf("%s androidauto: bluez_stack: starting BlueZ stack\n", logTimestamp().c_str());
    int rc = std::system("/usr/bin/bluez-bringup.sh start");
    return (rc == 0);
}

void bluez_stack_stop() {
    // BlueZ is the permanent system Bluetooth stack -- never stop it or revert to blueware
    std::printf("%s androidauto: bluez_stack: keeping BlueZ stack active permanently\n",
               logTimestamp().c_str());
}

}  // namespace androidauto
