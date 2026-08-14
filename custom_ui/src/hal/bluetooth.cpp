#include "hal/bluetooth.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>

#include <fcntl.h>
#include <sys/time.h>
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

// 2026-08-12: backs start_bluetooth_reader()/watch_bluetooth_broadcasts()
// -- see hal/bluetooth.h's comments on both for the "why". One reader
// thread owns all ::read()s on the shared fd for the whole process;
// send_command() no longer touches the fd directly at all, it just
// posts a request here and waits on a condition variable for matching
// lines to arrive, exactly the same one-reader-thread-plus-dispatch
// pattern hal/mcu_input.h already uses for its own serial link.
struct ReaderState {
    // Serializes whole request/response exchanges -- exactly one
    // outstanding send_command() call at a time, matching how this
    // AT-command protocol has always been used in practice (no
    // pipelining, no confirmed way to tell two commands' replies
    // apart if they were ever interleaved).
    std::mutex request_mtx;

    // Guards everything below -- touched by both send_command() (the
    // waiting side) and reader_loop() (the producing side, running on
    // its own thread).
    std::mutex line_mtx;
    std::condition_variable line_cv;
    bool request_active = false;
    std::string expected_prefix;
    std::vector<std::string> matched_lines;

    // Broadcast observers -- see watch_bluetooth_broadcasts()'s own
    // comment. Called directly from reader_loop()'s thread, so callers
    // must keep them fast and must not call back into send_command()
    // (would deadlock against request_mtx if a broadcast callback ever
    // tried to issue its own AT command synchronously from here).
    std::mutex observer_mtx;
    std::vector<std::function<void(const std::string &)>> observers;

    bool started = false;
};

ReaderState & reader_state() {
    static ReaderState state;
    return state;
}

// Runs for the process's whole lifetime once started -- no shutdown
// path, same convention as every other background reader in this
// codebase (hal::McuInputHal, core::ReverseGearWatcher). Splits raw
// bytes into \r\n/\n-terminated lines (same trimming rules
// send_command() used to do inline) and, for each complete line:
// dispatches to every registered broadcast observer, THEN checks
// whether a send_command() call is currently waiting and whether this
// line matches its expected_prefix (or no filter was set) -- if so,
// appends it (prefix-stripped, same as before) to matched_lines and
// wakes the waiter.
void reader_loop(int fd) {
    std::string buffer;
    char chunk[256];
    for (;;) {
        ssize_t n = ::read(fd, chunk, sizeof(chunk));
        if (n <= 0) {
            std::fprintf(stderr, "hal::bluetooth reader: read() returned %zd, stopping (%s)\n", n,
                         std::strerror(errno));
            return;
        }
        buffer.append(chunk, static_cast<size_t>(n));

        size_t start = 0;
        for (size_t i = 0; i < buffer.size(); ++i) {
            if (buffer[i] != '\n') continue;
            std::string entry = buffer.substr(start, i - start);
            start = i + 1;
            while (!entry.empty() && entry.back() == '\r') entry.pop_back();
            if (entry.empty()) continue;

            ReaderState & rs = reader_state();

            {
                std::lock_guard<std::mutex> lock(rs.observer_mtx);
                for (auto & observer : rs.observers) {
                    observer(entry);
                }
            }

            {
                std::lock_guard<std::mutex> lock(rs.line_mtx);
                if (rs.request_active) {
                    if (rs.expected_prefix.empty() || entry.rfind(rs.expected_prefix, 0) == 0) {
                        if (!rs.expected_prefix.empty()) {
                            entry.erase(0, rs.expected_prefix.size());
                        }
                        rs.matched_lines.push_back(std::move(entry));
                        rs.line_cv.notify_one();
                    } else {
                        std::printf("hal::send_command: dropping unrelated line '%s' (expected "
                                    "prefix '%s')\n", entry.c_str(), rs.expected_prefix.c_str());
                    }
                }
            }
        }
        buffer.erase(0, start);
    }
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
    // (Redirecting to a log file instead of /dev/null was tried as an
    // improvement over the real app -- the same doc section notes the
    // real app's `> /dev/null 2>&1` throws away blueware's own detailed
    // bpio_init/GPIO91 error messages -- but per explicit request this
    // reverted back to /dev/null, matching stock exactly: not needed in
    // practice, and it's one more file quietly growing on /tmp. Still
    // fully configurable via hal.conf's LogPath if it's needed again for
    // a specific debugging session.)
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

    ReaderState & rs = reader_state();
    // Serializes whole exchanges across every caller (UI thread doing
    // PLIST/ADDR, watch_bluetooth_broadcasts() callers that might issue
    // their own commands, etc.) -- exactly one outstanding request at a
    // time. Held for this whole function, released on return.
    std::lock_guard<std::mutex> request_lock(rs.request_mtx);

    {
        std::lock_guard<std::mutex> lock(rs.line_mtx);
        rs.matched_lines.clear();
        rs.expected_prefix = expected_prefix;
        rs.request_active = true;
    }

    // Confirmed literal template from BlueToothAdapter_Blueware::
    // writeCommand() -- see this header's top comment.
    std::string line = "AT+" + command + "\r\n";
    ssize_t written = ::write(h.fd, line.data(), line.size());
    if (written != static_cast<ssize_t>(line.size())) {
        std::fprintf(stderr, "hal::send_command: write failed for '%s' (%s)\n", command.c_str(),
                     std::strerror(errno));
        std::lock_guard<std::mutex> lock(rs.line_mtx);
        rs.request_active = false;
        return false;
    }

    // Wait for matching lines to arrive, dispatched by reader_loop()
    // (see its own comment) running on the background reader thread.
    // blueware may reply with more than one "+PREFIX=..." line (e.g.
    // PLIST enumerating several devices), and there's no confirmed
    // terminator marking "response complete" for any given command, so
    // this waits for whatever arrives inside the window rather than a
    // specific sentinel.
    //
    // 2026-08-12: wait up to the full timeout_ms for the FIRST line
    // (blueware may genuinely be slow to start replying), but once at
    // least one has arrived, only wait kIdleGapMs for MORE before
    // deciding the response is complete -- still bounded by the
    // original timeout_ms as an absolute ceiling either way, so a burst
    // of continuous unsolicited broadcast traffic can't make this hang
    // indefinitely. (This replaced an earlier version of this same idea
    // built directly on select()/::read() -- functionally identical
    // timing, just now waiting on the shared reader's condition
    // variable instead of polling the fd directly, since the fd itself
    // is now owned exclusively by the background reader thread.)
    constexpr int kIdleGapMs = 300;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    {
        std::unique_lock<std::mutex> lock(rs.line_mtx);
        for (;;) {
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline) break;
            auto remaining = deadline - now;
            auto wait_for = rs.matched_lines.empty()
                                 ? remaining
                                 : std::min<std::chrono::steady_clock::duration>(
                                       std::chrono::milliseconds(kIdleGapMs), remaining);
            if (rs.line_cv.wait_for(lock, wait_for) == std::cv_status::timeout) {
                break;  // idle gap (or overall timeout) elapsed with nothing new
            }
            // Otherwise a new line arrived (or a spurious wakeup) --
            // loop back around and re-evaluate the deadline/idle gap.
        }
        response_lines = rs.matched_lines;
        rs.request_active = false;
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

void start_bluetooth_reader(BluetoothHandle & h) {
    if (h.fd < 0) return;
    ReaderState & rs = reader_state();
    if (rs.started) return;
    rs.started = true;
    std::thread(reader_loop, h.fd).detach();
}

void watch_bluetooth_broadcasts(std::function<void(const std::string &)> callback) {
    ReaderState & rs = reader_state();
    std::lock_guard<std::mutex> lock(rs.observer_mtx);
    rs.observers.push_back(std::move(callback));
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

bool sync_clock_from_phone(BluetoothHandle & h) {
    std::vector<std::string> resp;
    // No expected_prefix -- unlike blueware's own vocabulary, AT+CCLK
    // is a standard Hayes command replying "+CCLK: \"...\"" (colon, not
    // equals), so this parses for the quoted value directly instead of
    // relying on send_command()'s exact-prefix-match/strip mechanism.
    // See this function's header comment for why.
    if (!send_command(h, "CCLK?", resp, 2000)) {
        std::fprintf(stderr, "hal::sync_clock_from_phone: AT+CCLK? got no response\n");
        return false;
    }

    for (const auto & line : resp) {
        if (line.find("CCLK") == std::string::npos) {
            continue;
        }
        auto q1 = line.find('"');
        auto q2 = q1 == std::string::npos ? std::string::npos : line.find('"', q1 + 1);
        if (q1 == std::string::npos || q2 == std::string::npos) {
            continue;
        }
        std::string ts = line.substr(q1 + 1, q2 - q1 - 1);

        // Expected: yy/MM/dd,hh:mm:ss+-zz -- timezone quarter-hour
        // offset is optional (not every phone/module includes it).
        int yy = 0, mo = 0, dd = 0, hh = 0, mi = 0, ss = 0, tz = 0;
        char sign = 0;
        int n = std::sscanf(ts.c_str(), "%d/%d/%d,%d:%d:%d%c%d", &yy, &mo, &dd, &hh, &mi, &ss,
                             &sign, &tz);
        if (n < 6) {
            std::fprintf(stderr, "hal::sync_clock_from_phone: couldn't parse CCLK value '%s'\n",
                         ts.c_str());
            continue;
        }

        struct tm tmv {};
        tmv.tm_year = yy + 100;  // 2-digit year, always 2000s+ for a real phone
        tmv.tm_mon = mo - 1;
        tmv.tm_mday = dd;
        tmv.tm_hour = hh;
        tmv.tm_min = mi;
        tmv.tm_sec = ss;

        time_t epoch = timegm(&tmv);
        if (epoch == static_cast<time_t>(-1)) {
            std::fprintf(stderr, "hal::sync_clock_from_phone: timegm() failed for '%s'\n",
                         ts.c_str());
            continue;
        }

        if (n >= 8) {
            // tz is in quarter-hours; convert phone-local wall time to UTC.
            int offsetSeconds = tz * 15 * 60;
            if (sign == '-') {
                offsetSeconds = -offsetSeconds;
            }
            epoch -= offsetSeconds;
        }

        struct timeval tv {};
        tv.tv_sec = epoch;
        tv.tv_usec = 0;
        if (settimeofday(&tv, nullptr) != 0) {
            std::fprintf(stderr, "hal::sync_clock_from_phone: settimeofday() failed: %s\n",
                         std::strerror(errno));
            return false;
        }

        std::printf("hal::sync_clock_from_phone: system clock set from phone (AT+CCLK? -> '%s', "
                    "epoch=%lld)\n", ts.c_str(), static_cast<long long>(epoch));
        return true;
    }

    std::fprintf(stderr, "hal::sync_clock_from_phone: no +CCLK line in response\n");
    return false;
}

// 2026-08-14: pure diagnostic, changes nothing -- added after finding a
// real, well-documented Android Auto 17.4+ client-side gate (community
// issue thread, github.com/andreknieriem/open-headunit #698): the phone's
// own Gearhead app aborts wireless setup outright if the paired
// Bluetooth device reports ANY battery-level value, via either an HFP
// battery indicator or a BLE GATT Battery Service (0x180F) -- logged on
// the PHONE side as "Bluetooth device ... has a battery level, exiting"
// / WIRELESS_STOPPED_SETUP_WITH_HU_WITH_BT_BATTERY_LEVEL. This exactly
// matches this project's own long-standing symptom (full aasdk-level
// session completes, then the connection dies anyway) -- and critically,
// the decision happens entirely inside the phone's own app based on a
// Bluetooth/GATT query, never touching the AA TCP wire session at all,
// which is also consistent with the TCP trace logging (see
// third_party/aasdk's TCPWrapper) showing genuine silence before the
// eventual FIN on at least one real run.
//
// This device's own config/code never calls blueware's AT+HFPBATT (the
// HFP battery-report command) -- grepped, confirmed absent from both
// etc/blueware-bw121.properties and this whole codebase -- but that
// doesn't rule out the module having its own always-on default (no
// physical battery to report a real value from, but some BT SoC
// reference firmwares expose a Battery Service by default regardless)
// or a BLE-only Battery Service unrelated to the HFP-specific command
// entirely. AT+HFPBATT? and AT+GATTSTAT? are real, read-only diagnostic
// queries in blueware's own vocabulary (confirmed via `strings
// usr/bin/blueware`) -- this logs their raw responses so the next real
// hardware test has actual evidence instead of another guess. Not
// wired into any decision -- purely observational for now.
bool diagnose_battery_reporting(BluetoothHandle & h) {
    std::vector<std::string> resp;
    bool anyResponse = false;

    if (send_command(h, "HFPBATT?", resp, 2000)) {
        anyResponse = true;
        for (const auto & line : resp) {
            std::printf("hal::diagnose_battery_reporting: AT+HFPBATT? -> '%s'\n", line.c_str());
        }
    } else {
        std::printf("hal::diagnose_battery_reporting: AT+HFPBATT? got no response\n");
    }

    if (send_command(h, "GATTSTAT?", resp, 2000)) {
        anyResponse = true;
        for (const auto & line : resp) {
            std::printf("hal::diagnose_battery_reporting: AT+GATTSTAT? -> '%s'\n", line.c_str());
        }
    } else {
        std::printf("hal::diagnose_battery_reporting: AT+GATTSTAT? got no response\n");
    }

    return anyResponse;
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
        // Starts the persistent background reader the moment the fd is
        // usable -- see start_bluetooth_reader()'s own comment. Every
        // caller in this codebase uses this shared handle, so this is
        // the one natural place to do it rather than requiring every
        // call site to remember to.
        start_bluetooth_reader(handle);
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
