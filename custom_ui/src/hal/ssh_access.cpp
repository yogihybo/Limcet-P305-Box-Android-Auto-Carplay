#include "hal/ssh_access.h"

#include "core/log_timing.h"

#include <cstdio>
#include <cstdlib>

namespace hal {

void set_ssh_enabled(bool enabled) {
    bool running = (std::system("pidof sshd >/dev/null 2>&1") == 0);

    if (enabled && !running) {
        // Same invocation rcS used to run unconditionally at boot -- see
        // that file's own comment for why it's /usr/sbin/sshd, not
        // /usr/bin/sshd, on this rootfs. mkdir here matches rcS's own
        // pre-flight step (sshd's privilege-separation directory).
        std::system("mkdir -p /var/run/sshd /var/empty");
        std::system("/usr/sbin/sshd -f /etc/ssh/sshd_config >/dev/null 2>&1 &");
        std::printf("%s [HAL:SSH] SSH access enabled (sshd started)\n", core::log_timestamp().c_str());
    } else if (!enabled && running) {
        std::system("killall sshd 2>/dev/null");
        std::printf("%s [HAL:SSH] SSH access disabled (sshd stopped)\n", core::log_timestamp().c_str());
    }
}

}  // namespace hal
