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
#include "core/log_timing.h"

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
//
// 2026-08-19: real gap found via code audit -- this used to just log
// and return on any read() <= 0 (EINTR, or blueware itself restarting
// and closing the fd out from under this), leaving
// ReaderState::started permanently true with no reader ever coming
// back. Every send_command() call from then on (including the write()
// succeeding fine, since that's a DIFFERENT fd-using code path) would
// wait on line_cv for a response that no thread can ever deliver
// again, silently timing out on every single call for the rest of
// the process's life -- Bluetooth functionality effectively dead
// with no error surfaced anywhere obvious. A full live-reconnect
// would also need to keep h's fd (used by send_command()'s own
// ::write() calls, a completely separate code path from this reader)
// in sync with whatever new fd this thread opens, which is real
// added complexity/risk for a background HAL reader -- instead, this
// now takes the actual BluetoothHandle (not just a copied fd) so it
// can mark BOTH `h->fd = -1` and the reader `started` flag false on
// exit, matching close_bluetooth()'s own convention. That makes
// send_command() fail FAST and visibly (h.fd < 0 check, already
// there) instead of hanging on a dead condition variable -- a clear,
// diagnosable "Bluetooth is down" state rather than a silent,
// indefinite hang.
void reader_loop(BluetoothHandle * h) {
    std::string buffer;
    char chunk[256];
    int fd = h->fd;
    for (;;) {
        ssize_t n = ::read(fd, chunk, sizeof(chunk));
        if (n <= 0) {
            std::fprintf(stderr, "%s hal::bluetooth::bluetooth reader: read() returned %zd, stopping (%s) -- "
                         "marking Bluetooth handle dead so future commands fail fast instead of hanging\n",
                         core::log_timestamp().c_str(), n, std::strerror(errno));
            h->fd = -1;
            ReaderState & rs = reader_state();
            std::lock_guard<std::mutex> lock(rs.line_mtx);
            rs.started = false;
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
                        std::printf("%s hal::bluetooth::send_command: dropping unrelated line '%s' (expected "
                                    "prefix '%s')\n", core::log_timestamp().c_str(), entry.c_str(), rs.expected_prefix.c_str());
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
        std::printf("%s hal::bluetooth::ensure_bluetooth_daemon_running: blueware already running\n", core::log_timestamp().c_str());
        return;  // already running
    }
    std::printf("%s hal::bluetooth::ensure_bluetooth_daemon_running: blueware not running, starting it\n", core::log_timestamp().c_str());
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
    std::printf("%s hal::bluetooth::ensure_bluetooth_daemon_running: launching '%s' (log: %s)\n", core::log_timestamp().c_str(), cmd.c_str(),
                cfg.bluetooth_log_path().c_str());
    if (std::system(cmd.c_str()) != 0) {
        std::fprintf(stderr, "%s hal::bluetooth::ensure_bluetooth_daemon_running: failed to launch '%s'\n", core::log_timestamp().c_str(),
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
                    std::fprintf(stderr, "%s hal::bluetooth::init_bluetooth: tcsetattr(%s) failed: %s\n", core::log_timestamp().c_str(),
                                 resolved_path.c_str(), std::strerror(errno));
                }
            } else {
                std::fprintf(stderr, "%s hal::bluetooth::init_bluetooth: tcgetattr(%s) failed: %s\n", core::log_timestamp().c_str(),
                             resolved_path.c_str(), std::strerror(errno));
            }
            std::printf("%s hal::bluetooth::init_bluetooth: %s opened (attempt %d/%d)\n", core::log_timestamp().c_str(),
                        resolved_path.c_str(), attempt + 1, kMaxAttempts);
            return true;
        }
        if (attempt + 1 < kMaxAttempts) {
            usleep(kRetryDelayUs);
        }
    }
    std::fprintf(stderr, "%s hal::bluetooth::init_bluetooth: warning: %s unavailable after %d attempts (%s)\n", core::log_timestamp().c_str(),
                 resolved_path.c_str(), kMaxAttempts, std::strerror(errno));
    return false;
}

bool send_command(BluetoothHandle & h, const std::string & command,
                   std::vector<std::string> & response_lines, int timeout_ms,
                   const std::string & expected_prefix, bool silent_on_error) {
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
        std::fprintf(stderr, "%s hal::bluetooth::send_command: write failed for '%s' (%s)\n", core::log_timestamp().c_str(), command.c_str(),
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
    if (!silent_on_error && find_error_response(response_lines, err)) {
        std::fprintf(stderr, "%s hal::bluetooth::send_command: adapter reported '%s' for command '%s'\n", core::log_timestamp().c_str(),
                     err.c_str(), command.c_str());
    }

    return !response_lines.empty();
}

void start_bluetooth_reader(BluetoothHandle & h) {
    if (h.fd < 0) return;
    ReaderState & rs = reader_state();
    if (rs.started) return;
    rs.started = true;
    std::thread(reader_loop, &h).detach();
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

bool split_plist_entry(const std::string & entry, std::string & mac, std::string & name) {
    mac.clear();
    name.clear();
    // 2026-08-15: found on real hardware -- unlike +AAPDEV= (which really
    // is "<mac><sep><name>", see split_mac_and_name()), a real +PLIST=
    // entry is FOUR separator-delimited fields, e.g.
    // "1<sep>16424<sep>04006EAF29C4<sep>Pixel 9 Pro" (index, some
    // numeric code, MAC, name) -- the previously-documented uncertainty
    // in this function's own header comment ("PLIST's own per-entry
    // format hasn't actually been observed yet") turned out to matter:
    // split_mac_and_name() only ever matches entries that START with 12
    // hex digits, so every real PLIST entry failed to parse and callers
    // fell back to treating the WHOLE raw 4-field line (separator bytes
    // included) as the MAC -- sent verbatim to HFPCONN, which the
    // adapter correctly rejected (ERR002) since it isn't a real address.
    // Rather than assume a fixed field count/order, this scans for the
    // first run of exactly 12 hex digits bounded by non-hex-digit
    // characters (or the string's own edges) -- that run IS the MAC
    // wherever it falls -- and takes whatever follows the next
    // separator byte as the name.
    size_t run_start = std::string::npos;
    for (size_t i = 0; i < entry.size(); ++i) {
        bool is_hex = std::isxdigit(static_cast<unsigned char>(entry[i])) != 0;
        if (is_hex) {
            if (run_start == std::string::npos) run_start = i;
            size_t run_len = i - run_start + 1;
            if (run_len == 12) {
                bool bounded_after = (i + 1 == entry.size()) ||
                                      !std::isxdigit(static_cast<unsigned char>(entry[i + 1]));
                if (bounded_after) {
                    mac = entry.substr(run_start, 12);
                    size_t after = i + 1;
                    // Skip exactly one separator byte, same convention
                    // as split_mac_and_name().
                    if (after < entry.size()) ++after;
                    name = after < entry.size() ? entry.substr(after) : "";
                    return true;
                }
                // Longer-than-12 hex run (e.g. part of a longer numeric
                // field) -- not a MAC, keep scanning past it.
                run_start = std::string::npos;
            }
        } else {
            run_start = std::string::npos;
        }
    }
    return false;
}

bool set_adapter_enabled(BluetoothHandle & h, bool enabled) {
    std::vector<std::string> resp;
    if (!send_command(h, enabled ? "BTEN=1" : "BTEN=0", resp)) {
        return false;
    }
    std::string err;
    if (find_error_response(resp, err)) {
        std::fprintf(stderr, "%s hal::bluetooth::set_adapter_enabled(%d): adapter reported '%s'\n", core::log_timestamp().c_str(), enabled,
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
        std::printf("%s hal::bluetooth::set_discoverable: no confirmed SCAN=0 command exists, sending it "
                    "anyway (unconfirmed)\n", core::log_timestamp().c_str());
    }
    if (!send_command(h, discoverable ? "SCAN=1" : "SCAN=0", resp)) {
        return false;
    }
    std::string err;
    if (find_error_response(resp, err)) {
        std::fprintf(stderr, "%s hal::bluetooth::set_discoverable(%d): adapter reported '%s'\n", core::log_timestamp().c_str(), discoverable,
                     err.c_str());
        return false;
    }
    return true;
}

bool list_paired_devices(BluetoothHandle & h, std::vector<std::string> & devices) {
    return send_command(h, "PLIST", devices, 2000, "+PLIST=");
}

bool connect_device(BluetoothHandle & h, const std::string & mac) {
    std::printf("%s hal::bluetooth::connect_device: sending HFPCONN=%s\n", core::log_timestamp().c_str(), mac.c_str());
    std::vector<std::string> resp;
    if (!send_command(h, "HFPCONN=" + mac, resp)) {
        // send_command() itself already logged the specific reason
        // (write failure with errno, or nothing further if it was
        // simply a timeout) -- no need to guess at which one here.
        std::fprintf(stderr, "%s hal::bluetooth::connect_device: HFPCONN=%s got no response\n", core::log_timestamp().c_str(), mac.c_str());
        return false;
    }
    // 2026-08-12 FIX: this used to return `ok` from send_command()
    // directly, i.e. "did we get ANY response" -- a real hardware
    // capture showed HFPCONN reply with "ERR002" and this function
    // still reported success (the UI showed "HFPCONN sent") because a
    // non-empty response was all it ever checked for. See
    // find_error_response()'s own comment.
    //
    // 2026-08-15: check the error case FIRST and return early instead
    // of always dumping every response line -- send_command() already
    // logs the adapter's error line once on its own (see its own
    // comment on why that generic log stays uniform across every
    // caller), so unconditionally also printing "-> N response line(s)"
    // plus each line plus this function's own "failed: ..." summary
    // meant a single ERR002 showed up three times in a row. The
    // response-line dump is still useful for genuinely multi-line
    // SUCCESS replies, so it's kept, just skipped on the error path.
    std::string err;
    if (find_error_response(resp, err)) {
        std::fprintf(stderr, "%s hal::bluetooth::connect_device: HFPCONN=%s failed: adapter reported '%s'\n", core::log_timestamp().c_str(),
                     mac.c_str(), err.c_str());
        return false;
    }
    std::printf("%s hal::bluetooth::connect_device: HFPCONN=%s -> %zu response line(s):\n", core::log_timestamp().c_str(), mac.c_str(),
                resp.size());
    for (const auto & line : resp) {
        std::printf("%s hal::bluetooth::connect_device:   %s\n", core::log_timestamp().c_str(), line.c_str());
    }
    return true;
}

bool disconnect_device(BluetoothHandle & h) {
    // See bluetooth.h's own comment -- A2DPDISC/HFPDISC disconnect
    // whatever's currently active, not a specific MAC; both sent since
    // either profile (or both) could be the one actually connected.
    // Best-effort: a real hardware capture (see find_error_response()'s
    // own comment) showed this adapter can reply "OK" to a command
    // that doesn't actually apply (e.g. disconnecting a profile that
    // isn't connected) -- treated the same as every other disconnect-
    // style call in this codebase, log the outcome, don't treat "no
    // active link to disconnect" as a hard failure.
    std::printf("%s hal::bluetooth::disconnect_device: sending A2DPDISC, HFPDISC\n", core::log_timestamp().c_str());
    std::vector<std::string> resp;
    bool a2dpOk = send_command(h, "A2DPDISC", resp, 2000, "", /*silent_on_error=*/true);
    std::string a2dpErr;
    if (a2dpOk && find_error_response(resp, a2dpErr)) {
        std::printf("%s hal::bluetooth::disconnect_device: A2DPDISC -> '%s' (likely just not connected)\n",
                     core::log_timestamp().c_str(), a2dpErr.c_str());
    }
    resp.clear();
    bool hfpOk = send_command(h, "HFPDISC", resp, 2000, "", /*silent_on_error=*/true);
    std::string hfpErr;
    if (hfpOk && find_error_response(resp, hfpErr)) {
        std::printf("%s hal::bluetooth::disconnect_device: HFPDISC -> '%s' (likely just not connected)\n",
                     core::log_timestamp().c_str(), hfpErr.c_str());
    }
    return a2dpOk || hfpOk;
}

bool set_device_name(BluetoothHandle & h, const std::string & name) {
    std::vector<std::string> resp;
    if (!send_command(h, "NAME=" + name, resp)) {
        return false;
    }
    std::string err;
    if (find_error_response(resp, err)) {
        std::fprintf(stderr, "%s hal::bluetooth::set_device_name('%s'): adapter reported '%s'\n", core::log_timestamp().c_str(), name.c_str(),
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
        std::fprintf(stderr, "%s hal::bluetooth::set_pairing_pin: adapter reported '%s'\n", core::log_timestamp().c_str(), err.c_str());
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
        std::fprintf(stderr, "%s hal::bluetooth::get_adapter_address: adapter reported '%s'\n", core::log_timestamp().c_str(), err.c_str());
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
    // 2026-08-18: silent_on_error=true -- AT+CCLK? routinely gets ERR004
    // this early in boot (no phone connected yet to have a clock to
    // report), which used to log both send_command()'s own generic
    // "adapter reported 'ERR004' for command 'CCLK?'" and this
    // function's own "no +CCLK line in response" right after it for
    // the exact same routine, expected event -- pure boot-log noise,
    // not something worth surfacing. A real, ongoing failure to sync
    // once a phone IS connected would still show up via the absence of
    // the success log below, just without two lines of noise on every
    // boot before that.
    if (!send_command(h, "CCLK?", resp, 2000, "", /*silent_on_error=*/true)) {
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
            std::fprintf(stderr, "%s hal::bluetooth::sync_clock_from_phone: couldn't parse CCLK value '%s'\n", core::log_timestamp().c_str(),
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
            std::fprintf(stderr, "%s hal::bluetooth::sync_clock_from_phone: timegm() failed for '%s'\n", core::log_timestamp().c_str(),
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
            std::fprintf(stderr, "%s hal::bluetooth::sync_clock_from_phone: settimeofday() failed: %s\n", core::log_timestamp().c_str(),
                         std::strerror(errno));
            return false;
        }

        std::printf("%s hal::bluetooth::sync_clock_from_phone: system clock set from phone (AT+CCLK? -> '%s', "
                    "epoch=%lld)\n", core::log_timestamp().c_str(), ts.c_str(), static_cast<long long>(epoch));
        return true;
    }

    std::fprintf(stderr, "%s hal::bluetooth::sync_clock_from_phone: no +CCLK line in response\n", core::log_timestamp().c_str());
    return false;
}

bool diagnose_pbdown(BluetoothHandle & h) {
    std::vector<std::string> resp;
    // No expected_prefix -- unlike CCLK's known "+CCLK: ..." shape,
    // PBDOWN's real response format is completely unconfirmed (could be
    // a direct reply, could arrive as separate PBDATA=/PBCNT=/PBSTAT=
    // broadcasts instead -- send_command() alone can't see broadcasts
    // that arrive after this call's own timeout window, only whatever's
    // in flight during it). silent_on_error=false: unlike CCLK, this is
    // a one-shot manual diagnostic run deliberately, not a routine boot
    // path -- ERR responses here are exactly the information being
    // sought, not noise to suppress.
    bool ok = send_command(h, "PBDOWN", resp, 5000, "", /*silent_on_error=*/false);
    std::printf("%s hal::bluetooth::diagnose_pbdown: send_command returned %s, %zu line(s)\n",
                core::log_timestamp().c_str(), ok ? "true" : "false", resp.size());
    for (const auto & line : resp) {
        std::printf("%s hal::bluetooth::diagnose_pbdown: raw line: '%s'\n", core::log_timestamp().c_str(),
                    line.c_str());
    }
    if (resp.empty()) {
        std::printf("%s hal::bluetooth::diagnose_pbdown: no response lines at all -- either PBAP isn't "
                    "connected/authorized on the phone, or blueware answers asynchronously outside this "
                    "call's window (watch for late PBDATA=/PBCNT=/PBSTAT= broadcasts in the console too)\n",
                    core::log_timestamp().c_str());
    }
    return ok;
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

namespace {
// State for the readiness-wait + retry loop below -- heap-allocated
// via shared_ptr and captured by the broadcast observer registered in
// auto_reconnect_paired_device(), since watch_bluetooth_broadcasts()
// has no unregister API (observers live for the process's lifetime,
// see its own header comment) and this function's stack frame is long
// gone by the time later broadcasts arrive. Safe either way: once this
// function returns, the still-alive lambda just harmlessly sets flags
// nobody reads anymore.
struct ReconnectSync {
    std::mutex mtx;
    std::condition_variable cv;
    bool devstat_ready = false;
    bool link_confirmed = false;
};
}  // namespace

bool auto_reconnect_paired_device(BluetoothHandle & h) {
    if (h.fd < 0) {
        std::printf("%s hal::bluetooth::auto_reconnect_paired_device: no bluetooth handle, skipping\n", core::log_timestamp().c_str());
        return false;
    }
    std::vector<std::string> devices;
    if (!list_paired_devices(h, devices) || devices.empty()) {
        std::printf("%s hal::bluetooth::auto_reconnect_paired_device: no paired devices to reconnect to\n", core::log_timestamp().c_str());
        return false;
    }
    std::string mac, name;
    std::string connect_id = split_plist_entry(devices.front(), mac, name) ? mac : devices.front();

    // 2026-08-19: see docs/BLUETOOTH_RECONNECT_HANDOFF.md -- a real
    // captured boot log (docs/logs/bluetooth log stock_260718.txt)
    // shows blueware's own local radio/profile init (SRAM firmware
    // upload, then a AT+xSTAT=1 enable per profile) spans dozens of
    // lines before it emits `+DEVSTAT=3`, which fires exactly once,
    // deterministically, right as that local init sequence completes
    // -- NOT, despite that doc's original framing, an ongoing "unit is
    // in page-scan mode" state. Calling connect_device() before this
    // point risked a syntax-level "OK" from blueware's AT parser while
    // the underlying baseband link was never actually paged. Bounded
    // (kDevStatTimeout) so a run where blueware already finished before
    // this function was even called -- or, on some future build,
    // doesn't emit this line at all -- doesn't hang forever.
    auto sync = std::make_shared<ReconnectSync>();
    watch_bluetooth_broadcasts([sync](const std::string & line) {
        std::lock_guard<std::mutex> lock(sync->mtx);
        if (line == "+DEVSTAT=3") {
            sync->devstat_ready = true;
            sync->cv.notify_all();
        } else if (line.rfind("+HFPDEV=", 0) == 0 || line.rfind("+AAPDEV=", 0) == 0) {
            // AAPDEV= is a confirmed-real broadcast (see
            // docs/BLUEWARE_AT_COMMANDS.md); HFPDEV= is inferred by
            // name/shape only, never directly observed -- harmless to
            // also watch for, since if it never fires this just falls
            // through to the retry loop's own bounded attempt count.
            sync->link_confirmed = true;
            sync->cv.notify_all();
        }
    });

    constexpr auto kDevStatTimeout = std::chrono::seconds(3);
    {
        std::unique_lock<std::mutex> lock(sync->mtx);
        if (!sync->cv.wait_for(lock, kDevStatTimeout, [&] { return sync->devstat_ready; })) {
            std::printf("%s hal::bluetooth::auto_reconnect_paired_device: timed out waiting for "
                        "+DEVSTAT=3, proceeding anyway\n", core::log_timestamp().c_str());
        }
    }

    // 3-attempt bounded retry with a fixed backoff, terminating early
    // if a connection broadcast is observed -- see this function's own
    // header comment (matches BLUETOOTH_RECONNECT_HANDOFF.md's proposed
    // action plan).
    // 2026-08-19: real hardware log showed all 3 HFPCONN attempts
    // getting a genuine 'OK' from blueware, yet this function still
    // logged "giving up" -- the confirming +AAPDEV= broadcast this
    // function is actually waiting on (link_confirmed) arrived ~2.8s
    // AFTER the old 2s*3=6s retry budget expired (real timestamps:
    // last attempt's backoff ended at 10.13s, "giving up" logged at
    // 10.43s, +AAPDEV= didn't fire until 13.20s). The link was fine the
    // whole time -- the timeout was just tuned tighter than blueware's
    // real boot-to-broadcast timing on this hardware. 4s*3=12s
    // comfortably covers the observed ~9.7s gap with margin.
    constexpr int kMaxAttempts = 3;
    constexpr auto kRetryBackoff = std::chrono::seconds(4);
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        std::printf("%s hal::bluetooth::auto_reconnect_paired_device: attempt %d/%d, reconnecting to '%s'\n",
                    core::log_timestamp().c_str(), attempt, kMaxAttempts, connect_id.c_str());
        connect_device(h, connect_id);

        std::unique_lock<std::mutex> lock(sync->mtx);
        if (sync->cv.wait_for(lock, kRetryBackoff, [&] { return sync->link_confirmed; })) {
            std::printf("%s hal::bluetooth::auto_reconnect_paired_device: link confirmed after attempt %d\n",
                        core::log_timestamp().c_str(), attempt);
            return true;
        }
    }
    std::printf("%s hal::bluetooth::auto_reconnect_paired_device: giving up after %d attempts\n",
                core::log_timestamp().c_str(), kMaxAttempts);
    return false;
}

}  // namespace hal
