#include "hal/bluetooth.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

namespace hal {

void ensure_bluetooth_daemon_running() {
    if (std::system("pidof blueware >/dev/null 2>&1") == 0) {
        return;  // already running
    }
    std::printf("hal::ensure_bluetooth_daemon_running: blueware not running, starting it\n");
    // Fixed system path, not resolved relative to our own binary --
    // this is a vendor daemon already installed on the device rootfs,
    // not something shipped alongside custom_ui (contrast
    // androidauto_client.cpp's trySpawnSidecar(), which DOES resolve
    // relative to /proc/self/exe for that reason).
    if (std::system("/usr/bin/blueware >/tmp/blueware.log 2>&1 &") != 0) {
        std::fprintf(stderr, "hal::ensure_bluetooth_daemon_running: failed to launch "
                     "/usr/bin/blueware\n");
    }
}

bool init_bluetooth(BluetoothHandle & out, const char * path) {
    ensure_bluetooth_daemon_running();

    // Retry briefly -- blueware needs a moment after spawning to
    // create /dev/bw_serial. If it was already running (the common
    // case once one screen has already triggered this), the first
    // attempt succeeds immediately and no delay is added.
    constexpr int kMaxAttempts = 20;
    constexpr int kRetryDelayUs = 100000;  // 100ms -> up to ~2s total
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        out.fd = open(path, O_RDWR | O_NOCTTY);
        if (out.fd >= 0) {
            std::printf("hal::init_bluetooth: %s opened (attempt %d/%d)\n", path, attempt + 1,
                        kMaxAttempts);
            return true;
        }
        if (attempt + 1 < kMaxAttempts) {
            usleep(kRetryDelayUs);
        }
    }
    std::fprintf(stderr, "hal::init_bluetooth: warning: %s unavailable after %d attempts (%s)\n",
                 path, kMaxAttempts, std::strerror(errno));
    return false;
}

bool send_command(BluetoothHandle & h, const std::string & command,
                   std::vector<std::string> & response_lines, int timeout_ms) {
    response_lines.clear();
    if (h.fd < 0) return false;

    // Confirmed literal template from BlueToothAdapter_Blueware::
    // writeCommand() -- see this header's top comment.
    std::string line = "AT+" + command + "\r\n";
    ssize_t written = ::write(h.fd, line.data(), line.size());
    if (written != static_cast<ssize_t>(line.size())) {
        std::fprintf(stderr, "hal::send_command: write failed for '%s' (%s)\n", command.c_str(),
                     std::strerror(errno));
        return false;
    }

    // Collect response bytes until timeout_ms elapses with nothing
    // further pending -- blueware may reply with more than one
    // "+PREFIX=..." line (e.g. PLIST enumerating several devices), and
    // there's no confirmed terminator marking "response complete" for
    // any given command, so this reads whatever arrives inside the
    // window rather than waiting for a specific sentinel.
    std::string buffer;
    for (;;) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(h.fd, &read_set);
        struct timeval tv {};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        int ready = ::select(h.fd + 1, &read_set, nullptr, nullptr, &tv);
        if (ready <= 0) {
            break;  // timeout or error -- stop collecting
        }

        char chunk[256];
        ssize_t n = ::read(h.fd, chunk, sizeof(chunk));
        if (n <= 0) {
            break;
        }
        buffer.append(chunk, static_cast<size_t>(n));
    }

    // Split on \r\n / \n, drop empty lines.
    size_t start = 0;
    for (size_t i = 0; i < buffer.size(); ++i) {
        if (buffer[i] == '\n') {
            std::string entry = buffer.substr(start, i - start);
            while (!entry.empty() && (entry.back() == '\r')) entry.pop_back();
            if (!entry.empty()) response_lines.push_back(entry);
            start = i + 1;
        }
    }
    if (start < buffer.size()) {
        std::string entry = buffer.substr(start);
        while (!entry.empty() && (entry.back() == '\r')) entry.pop_back();
        if (!entry.empty()) response_lines.push_back(entry);
    }

    return !response_lines.empty();
}

bool set_adapter_enabled(BluetoothHandle & h, bool enabled) {
    std::vector<std::string> resp;
    return send_command(h, enabled ? "BTEN=1" : "BTEN=0", resp);
}

bool set_discoverable(BluetoothHandle & h, bool discoverable) {
    std::vector<std::string> resp;
    // Only SCAN=1 is confirmed vocabulary (see header) -- still allow
    // the false case to be requested by the caller without silently
    // no-op'ing, but flag it as unconfirmed on the wire.
    if (!discoverable) {
        std::fprintf(stderr,
                     "hal::set_discoverable: no confirmed SCAN=0 command exists, sending it "
                     "anyway (unconfirmed)\n");
    }
    return send_command(h, discoverable ? "SCAN=1" : "SCAN=0", resp);
}

bool list_paired_devices(BluetoothHandle & h, std::vector<std::string> & devices) {
    return send_command(h, "PLIST", devices);
}

bool connect_device(BluetoothHandle & h, const std::string & mac) {
    std::printf("hal::connect_device: sending HFPCONN=%s\n", mac.c_str());
    std::vector<std::string> resp;
    bool ok = send_command(h, "HFPCONN=" + mac, resp);
    if (ok) {
        std::printf("hal::connect_device: HFPCONN=%s -> %zu response line(s):\n", mac.c_str(),
                    resp.size());
        for (const auto & line : resp) {
            std::printf("hal::connect_device:   %s\n", line.c_str());
        }
    } else {
        std::fprintf(stderr, "hal::connect_device: HFPCONN=%s got no response (write failed or "
                     "timed out)\n", mac.c_str());
    }
    return ok;
}

bool set_device_name(BluetoothHandle & h, const std::string & name) {
    std::vector<std::string> resp;
    return send_command(h, "NAME=" + name, resp);
}

bool set_pairing_pin(BluetoothHandle & h, const std::string & pin) {
    std::vector<std::string> resp;
    return send_command(h, "PIN=" + pin, resp);
}

bool get_adapter_address(BluetoothHandle & h, std::string & address) {
    std::vector<std::string> resp;
    if (!send_command(h, "ADDR", resp) || resp.empty()) {
        return false;
    }
    address = resp.front();
    return true;
}

void close_bluetooth(BluetoothHandle & h) {
    if (h.fd >= 0) {
        close(h.fd);
        h.fd = -1;
    }
}

}  // namespace hal
