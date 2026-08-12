#include "hal/bluetooth.h"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include "core/hal_config.h"

namespace hal {

namespace {

// 2026-08-12: send_command() (and by extension every wrapper below)
// used to treat "got any response at all" as success -- a real
// hardware capture showed HFPCONN reply with "ERR002" and this code
// still reported the connect attempt as successful (non-empty
// response_lines), because nothing ever looked AT the response
// content. "ERR<code>" is the confirmed real prefix for an
// adapter-reported failure (both decompiled sources' vocabulary uses
// bare "OK" for success acks and "ERR<code>" for failures -- see
// hal/bluetooth.h's top comment). No documented meaning for specific
// ERR codes exists in either source, so this only detects the presence
// of one, not what it means.
bool find_error_response(const std::vector<std::string> & lines, std::string & err_out) {
    for (const auto & line : lines) {
        if (line.rfind("ERR", 0) == 0) {
            err_out = line;
            return true;
        }
    }
    return false;
}

}  // namespace

void ensure_bluetooth_daemon_running() {
    if (std::system("pidof blueware >/dev/null 2>&1") == 0) {
        std::printf("hal::ensure_bluetooth_daemon_running: blueware already running\n");
        return;  // already running
    }
    std::printf("hal::ensure_bluetooth_daemon_running: blueware not running, starting it\n");
    // Daemon path/properties-file argument/log redirect are all
    // configurable now -- see core/hal_config.h -- rather than
    // hardcoded here. The properties-file argument matters and was
    // originally missing entirely: the real stock app never launches
    // blueware bare. docs/WIRELESS_AND_INIT.md section 5 decompiled
    // BlueToothAdapter_Blueware::initBlueToothAdapter() (0x47728) and
    // found it always passes one of two config paths depending on a
    // board-variant flag byte:
    //   blueware /etc/blueware-bw121.properties > /dev/null 2>&1 &
    //   blueware /etc/blueware-bw123.properties > /dev/null 2>&1 &
    // This board is confirmed the bw121 variant (MODULE_TYPE=BW121,
    // same doc section) -- that's what hal.conf's shipped default
    // (firmware_overlay/etc/custom_ui/hal.conf) points at.
    // (Redirecting to a log file instead of /dev/null is a deliberate
    // improvement over the real app, not a divergence worth losing --
    // the same doc section notes the real app's `> /dev/null 2>&1`
    // throws away blueware's own detailed bpio_init/GPIO91 error
    // messages, which would otherwise be the single most direct way to
    // diagnose a BT-enable failure.)
    const core::HalConfig & cfg = core::hal_config();
    std::string cmd = cfg.bluetooth_daemon_path() + " " + cfg.bluetooth_properties_path() +
                       " >" + cfg.bluetooth_log_path() + " 2>&1 &";
    std::printf("hal::ensure_bluetooth_daemon_running: launching '%s' (log: %s)\n", cmd.c_str(),
                cfg.bluetooth_log_path().c_str());
    if (std::system(cmd.c_str()) != 0) {
        std::fprintf(stderr, "hal::ensure_bluetooth_daemon_running: failed to launch '%s'\n",
                     cmd.c_str());
    }
}

bool init_bluetooth(BluetoothHandle & out, const char * path) {
    ensure_bluetooth_daemon_running();

    std::string resolved_path = path ? path : core::hal_config().bluetooth_serial_port();

    // Retry briefly -- blueware needs a moment after spawning to
    // create /dev/bw_serial. If it was already running (the common
    // case once one screen has already triggered this), the first
    // attempt succeeds immediately and no delay is added.
    constexpr int kMaxAttempts = 20;
    constexpr int kRetryDelayUs = 100000;  // 100ms -> up to ~2s total
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        out.fd = open(resolved_path.c_str(), O_RDWR | O_NOCTTY);
        if (out.fd >= 0) {
            // 2026-08-12 FIX: this never touched the fd's termios
            // settings at all -- <termios.h> was included but unused.
            // Left at whatever default the tty/line-discipline gives
            // it (typically canonical mode + local echo), which meant
            // every "AT+<command>\r\n" we wrote could get echoed
            // straight back on the next read. get_adapter_address()
            // has no expected_prefix filter (unlike list_paired_
            // devices()'s "+PLIST=" filter), so that echoed line won
            // as resp.front() -- real symptom: the Bluetooth screen's
            // address field showing the literal command ("AT...")
            // instead of the real address. cfmakeraw() + disabling
            // ECHO explicitly (belt-and-braces, cfmakeraw already
            // clears it) puts this fd in the same raw, non-canonical,
            // no-echo mode every other AT-command-over-serial HAL in
            // this codebase's real-world equivalents assumes.
            struct termios tio {};
            if (tcgetattr(out.fd, &tio) == 0) {
                cfmakeraw(&tio);
                tio.c_lflag &= ~static_cast<tcflag_t>(ECHO | ECHOE | ECHOK | ECHONL);
                if (tcsetattr(out.fd, TCSANOW, &tio) != 0) {
                    std::fprintf(stderr, "hal::init_bluetooth: tcsetattr(%s) failed: %s\n",
                                 resolved_path.c_str(), std::strerror(errno));
                }
            } else {
                std::fprintf(stderr, "hal::init_bluetooth: tcgetattr(%s) failed: %s\n",
                             resolved_path.c_str(), std::strerror(errno));
            }
            std::printf("hal::init_bluetooth: %s opened (attempt %d/%d)\n",
                        resolved_path.c_str(), attempt + 1, kMaxAttempts);
            return true;
        }
        if (attempt + 1 < kMaxAttempts) {
            usleep(kRetryDelayUs);
        }
    }
    std::fprintf(stderr, "hal::init_bluetooth: warning: %s unavailable after %d attempts (%s)\n",
                 resolved_path.c_str(), kMaxAttempts, std::strerror(errno));
    return false;
}

bool send_command(BluetoothHandle & h, const std::string & command,
                   std::vector<std::string> & response_lines, int timeout_ms,
                   const std::string & expected_prefix) {
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

    // Split on \r\n / \n, drop empty lines. If expected_prefix is set,
    // also drop any line NOT starting with it (see this function's own
    // header comment -- blueware emits unsolicited status broadcasts
    // on this same link independent of what was sent, and without this
    // filter they silently end up mixed into the caller's result) and
    // strip the prefix off the ones that match.
    auto keep = [&](std::string entry) {
        while (!entry.empty() && (entry.back() == '\r')) entry.pop_back();
        if (entry.empty()) return;
        if (!expected_prefix.empty()) {
            if (entry.rfind(expected_prefix, 0) != 0) {
                std::printf("hal::send_command: dropping unrelated line '%s' (expected prefix "
                           "'%s')\n", entry.c_str(), expected_prefix.c_str());
                return;
            }
            entry.erase(0, expected_prefix.size());
        }
        response_lines.push_back(entry);
    };

    size_t start = 0;
    for (size_t i = 0; i < buffer.size(); ++i) {
        if (buffer[i] == '\n') {
            keep(buffer.substr(start, i - start));
            start = i + 1;
        }
    }
    if (start < buffer.size()) {
        keep(buffer.substr(start));
    }

    // Diagnostic only -- deliberately does NOT affect the return value.
    // send_command()'s contract is "did we get a response", not "did
    // the command succeed" (different commands have different success
    // shapes, e.g. PLIST's success IS an empty list), so this just
    // makes an adapter-reported failure visible in the log for every
    // command uniformly; callers that have a clear success/failure
    // contract (connect_device(), set_device_name(), etc.) check for it
    // themselves and act on it.
    std::string err;
    if (find_error_response(response_lines, err)) {
        std::fprintf(stderr, "hal::send_command: adapter reported '%s' for command '%s'\n",
                     err.c_str(), command.c_str());
    }

    return !response_lines.empty();
}

bool split_mac_and_name(const std::string & entry, std::string & mac, std::string & name) {
    mac.clear();
    name.clear();
    if (entry.size() < 13) return false;  // 12 hex chars + at least 1 separator byte
    for (int i = 0; i < 12; ++i) {
        if (!std::isxdigit(static_cast<unsigned char>(entry[i]))) return false;
    }
    mac = entry.substr(0, 12);
    // Exactly one separator byte observed on real hardware (a non-
    // printable byte between the MAC and the device name in a real
    // +AAPDEV= line) -- skip it, whatever it is, rather than assuming
    // it's a space/comma/etc.
    name = entry.size() > 13 ? entry.substr(13) : "";
    return true;
}

bool set_adapter_enabled(BluetoothHandle & h, bool enabled) {
    std::vector<std::string> resp;
    if (!send_command(h, enabled ? "BTEN=1" : "BTEN=0", resp)) {
        return false;
    }
    std::string err;
    if (find_error_response(resp, err)) {
        std::fprintf(stderr, "hal::set_adapter_enabled(%d): adapter reported '%s'\n", enabled,
                     err.c_str());
        return false;
    }
    return true;
}

bool set_discoverable(BluetoothHandle & h, bool discoverable) {
    std::vector<std::string> resp;
    // Only SCAN=1 is confirmed vocabulary (see header) -- still allow
    // the false case to be requested by the caller without silently
    // no-op'ing, but flag it as unconfirmed on the wire. printf, not
    // stderr -- this is a known documentation caveat about the AT
    // command itself, not a failure of this specific call.
    if (!discoverable) {
        std::printf("hal::set_discoverable: no confirmed SCAN=0 command exists, sending it "
                    "anyway (unconfirmed)\n");
    }
    if (!send_command(h, discoverable ? "SCAN=1" : "SCAN=0", resp)) {
        return false;
    }
    std::string err;
    if (find_error_response(resp, err)) {
        std::fprintf(stderr, "hal::set_discoverable(%d): adapter reported '%s'\n", discoverable,
                     err.c_str());
        return false;
    }
    return true;
}

bool list_paired_devices(BluetoothHandle & h, std::vector<std::string> & devices) {
    return send_command(h, "PLIST", devices, 2000, "+PLIST=");
}

bool connect_device(BluetoothHandle & h, const std::string & mac) {
    std::printf("hal::connect_device: sending HFPCONN=%s\n", mac.c_str());
    std::vector<std::string> resp;
    if (!send_command(h, "HFPCONN=" + mac, resp)) {
        // send_command() itself already logged the specific reason
        // (write failure with errno, or nothing further if it was
        // simply a timeout) -- no need to guess at which one here.
        std::fprintf(stderr, "hal::connect_device: HFPCONN=%s got no response\n", mac.c_str());
        return false;
    }
    std::printf("hal::connect_device: HFPCONN=%s -> %zu response line(s):\n", mac.c_str(),
                resp.size());
    for (const auto & line : resp) {
        std::printf("hal::connect_device:   %s\n", line.c_str());
    }
    // 2026-08-12 FIX: this used to return `ok` from send_command()
    // directly, i.e. "did we get ANY response" -- a real hardware
    // capture showed HFPCONN reply with "ERR002" and this function
    // still reported success (the UI showed "HFPCONN sent") because a
    // non-empty response was all it ever checked for. See
    // find_error_response()'s own comment.
    std::string err;
    if (find_error_response(resp, err)) {
        std::fprintf(stderr, "hal::connect_device: HFPCONN=%s failed: adapter reported '%s'\n",
                     mac.c_str(), err.c_str());
        return false;
    }
    return true;
}

bool set_device_name(BluetoothHandle & h, const std::string & name) {
    std::vector<std::string> resp;
    if (!send_command(h, "NAME=" + name, resp)) {
        return false;
    }
    std::string err;
    if (find_error_response(resp, err)) {
        std::fprintf(stderr, "hal::set_device_name('%s'): adapter reported '%s'\n", name.c_str(),
                     err.c_str());
        return false;
    }
    return true;
}

bool set_pairing_pin(BluetoothHandle & h, const std::string & pin) {
    std::vector<std::string> resp;
    if (!send_command(h, "PIN=" + pin, resp)) {
        return false;
    }
    std::string err;
    if (find_error_response(resp, err)) {
        std::fprintf(stderr, "hal::set_pairing_pin: adapter reported '%s'\n", err.c_str());
        return false;
    }
    return true;
}

bool get_adapter_address(BluetoothHandle & h, std::string & address) {
    std::vector<std::string> resp;
    // expected_prefix="+ADDR=" (confirmed vocabulary, see this file's
    // header comment) -- defense in depth alongside the termios fix in
    // init_bluetooth(): even if an echo or unrelated broadcast line
    // still lands in this window for some other reason, only a real
    // "+ADDR=..." line can end up in resp now, same as
    // list_paired_devices()'s existing "+PLIST=" filter.
    if (!send_command(h, "ADDR", resp, 2000, "+ADDR=") || resp.empty()) {
        return false;
    }
    // Same fix as connect_device()/set_device_name()/etc -- without
    // this check, an "ERR<code>" response would get treated as if it
    // were the adapter's own address string.
    std::string err;
    if (find_error_response(resp, err)) {
        std::fprintf(stderr, "hal::get_adapter_address: adapter reported '%s'\n", err.c_str());
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

BluetoothHandle & shared_handle() {
    static BluetoothHandle handle;
    static bool tried = false;
    if (!tried) {
        init_bluetooth(handle);  // non-fatal if /dev/bw_serial is absent
        tried = true;
    }
    return handle;
}

bool auto_reconnect_paired_device(BluetoothHandle & h) {
    if (h.fd < 0) {
        std::printf("hal::auto_reconnect_paired_device: no bluetooth handle, skipping\n");
        return false;
    }
    std::vector<std::string> devices;
    if (!list_paired_devices(h, devices) || devices.empty()) {
        std::printf("hal::auto_reconnect_paired_device: no paired devices to reconnect to\n");
        return false;
    }
    std::string mac, name;
    std::string connect_id = split_mac_and_name(devices.front(), mac, name) ? mac : devices.front();
    std::printf("hal::auto_reconnect_paired_device: reconnecting to '%s'\n", connect_id.c_str());
    return connect_device(h, connect_id);
}

}  // namespace hal
