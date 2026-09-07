#include "hal/ssh_access.h"

#include "core/log_timing.h"

#include <cstdio>
#include <cstdlib>

namespace hal {

void set_ssh_enabled(bool enabled) {
    bool running = (std::system("pidof sshd >/dev/null 2>&1") == 0);

    if (enabled) {
        // Ensure WiFi Access Point is running so client devices can associate
        // and reach 192.168.43.1 via SSH.
        bool ap_running = (std::system("pidof hostapd >/dev/null 2>&1") == 0);
        if (!ap_running) {
            if (std::system("test -x /etc/wifi_ap.sh") == 0) {
                std::printf("%s [HAL:SSH] Starting WiFi AP for SSH access (/etc/wifi_ap.sh)\n",
                            core::log_timestamp().c_str());
                std::system("/etc/wifi_ap.sh >/dev/null 2>&1 &");
            } else if (std::system("test -x /etc/init.d/hostapd.sh") == 0) {
                std::printf("%s [HAL:SSH] Starting stock WiFi AP for SSH access (/etc/init.d/hostapd.sh)\n",
                            core::log_timestamp().c_str());
                std::system("/etc/init.d/hostapd.sh start >/dev/null 2>&1 &");
            }
        } else {
            // hostapd is already running; ensure udhcpd is also running so clients get an IP lease.
            if (std::system("pidof udhcpd >/dev/null 2>&1") != 0) {
                std::printf("%s [HAL:SSH] Ensuring udhcpd DHCP daemon is running\n",
                            core::log_timestamp().c_str());
                std::system("mkdir -p /var/lib/misc 2>/dev/null; touch /data/udhcpd.leases 2>/dev/null; udhcpd /etc/udhcpd.conf >/dev/null 2>&1 &");
            }
        }

        if (!running) {
            // Same invocation rcS used to run unconditionally at boot -- see
            // that file's own comment for why it's /usr/sbin/sshd, not
            // /usr/bin/sshd, on this rootfs. mkdir here matches rcS's own
            // pre-flight step (sshd's privilege-separation directory).
            std::system("mkdir -p /var/run/sshd /var/empty");
            std::system("/usr/sbin/sshd -f /etc/ssh/sshd_config >/dev/null 2>&1 &");
            std::printf("%s [HAL:SSH] SSH access enabled (sshd started)\n", core::log_timestamp().c_str());
        }
    } else if (!enabled && running) {
        std::system("killall sshd 2>/dev/null");
        std::printf("%s [HAL:SSH] SSH access disabled (sshd stopped)\n", core::log_timestamp().c_str());
        // Note: hostapd is intentionally left running when disabling SSH so
        // active wireless Android Auto or CarPlay projection sessions are not disrupted.
    }
}

}  // namespace hal
