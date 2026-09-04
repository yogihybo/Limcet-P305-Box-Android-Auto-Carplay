#include "hal/mcu_input.h"
#include "hal/androidauto_client.h"
#include "core/log_timing.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

namespace hal {

namespace {

static McuInputHal * g_mcu_instance = nullptr;

// MCUAdapter_BoxP300::getPortSettings() (libMcuCenter.so) -- confirmed
// real baud, see tools/mcu-handshake/mcu-handshake.c's own comment.
constexpr int kBaud = B38400;

// CMD 0x02 knob/button codes -- live hardware captured
constexpr unsigned char kBtnNextTrack = 3;
constexpr unsigned char kBtnPrevTrack = 4;
constexpr unsigned char kBtnAnswer = 8;
constexpr unsigned char kBtnHangup = 9;
constexpr unsigned char kBtnHome = 12;
constexpr unsigned char kKnobPush = 13;
constexpr unsigned char kKnobCounterClockwise = 64;
constexpr unsigned char kKnobClockwise = 65;

// MCUAdapter_BoxP300::getPackageCheckSum() -- plain byte sum, one's
// complemented, over cmd+len+payload (not the leading 0x2E signature).
// Identical to mcu-handshake.c's calc_mcu_checksum().
unsigned char mcu_checksum(const unsigned char * data, int len) {
    unsigned int sum = 0;
    for (int i = 0; i < len; i++) sum += data[i];
    return static_cast<unsigned char>(~sum & 0xFF);
}

bool set_interface_attribs(int fd) {
    struct termios tty {};
    if (tcgetattr(fd, &tty) < 0) {
        perror("hal::McuInputHal: tcgetattr");
        return false;
    }

    cfsetospeed(&tty, kBaud);
    cfsetispeed(&tty, kBaud);

    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CRTSCTS;

    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tty.c_oflag &= ~OPOST;

    tty.c_cc[VMIN] = 1;
    tty.c_cc[VTIME] = 1;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        perror("hal::McuInputHal: tcsetattr");
        return false;
    }
    return true;
}

// Matches send_mcu_frame() in mcu-handshake.c exactly.
void send_mcu_frame(int fd, unsigned char cmd, const unsigned char * payload, int payload_len) {
    unsigned char frame[256];
    unsigned char cksum_buf[256];
    int idx = 0;

    frame[idx++] = 0x2E;
    frame[idx++] = cmd;
    frame[idx++] = static_cast<unsigned char>(payload_len);
    if (payload_len > 0) {
        std::memcpy(&frame[idx], payload, payload_len);
    }

    std::memcpy(cksum_buf, &frame[1], 2 + payload_len);
    unsigned char chk = mcu_checksum(cksum_buf, 2 + payload_len);
    idx += payload_len;
    frame[idx++] = chk;

    if (write(fd, frame, idx) < 0) {
        perror("hal::McuInputHal: write (send_mcu_frame)");
    }
}

// Matches send_startup_sequence() in mcu-handshake.c -- see that file's
// header comment for full provenance (disassembly of
// MCUAdapter_BoxP300 in libMcuCenter.so). Sent here because every real
// capture of MCU traffic this project has was taken with this sequence
// already sent (mcu-handshake's default) -- whether it's strictly
// required for the MCU to report touch/knob/button events isn't
// confirmed, but it's cheap/harmless and matches the only known-working
// configuration.
//
// CORRECTED 2026-09-02: the CMD 0x82 mode value was mode=4 ("the only
// mode reachable from inside libMcuCenter.so" per the original trace,
// which never crossed into MsnCoreApp itself). Full tracing this
// session found the real, actually-sent-at-init value: MsnCoreApp::
// onFirstInit() calls modeAppChanged(app, mode=1) -- a genuine real
// init-time send, not a guess. mode=1 isn't in onModeAppChanged()'s
// special-case set ({2,4,5,7,13} append byte 0x08; 23 appends 0x0A),
// so no extra byte gets appended -- payload is just the fixed leading
// byte, no mode-4-specific 0x08 and no padding. See docs/
// MCU_COMMAND_REFERENCE.md's "CMD 0x82's real consumer" section.
//
// CMD 0x85 removed 2026-09-02: exhaustively confirmed (same session)
// that no code anywhere in MCUAdapter_BoxP300 or the shared MCUAdapter
// base class ever sends it -- MCUAdapter_BoxP300::onRecvAppProtocol(),
// the one real candidate entry point, is a hard `bx lr` no-op on this
// product (real, unlike CMD 0x84, which a *different* vehicle-adapter
// variant genuinely sends -- 0x85 has no live sender anywhere in this
// shared library). Sending it never replicated real stock behavior and
// had no confirmed effect of its own, so it's dropped rather than kept
// as unexplained traffic.
void send_startup_sequence(int fd) {
    unsigned char hello_payload = 0x01;
    unsigned char mode1_payload[1] = {0x01};
    unsigned char state_payload[2] = {0x00, 0x03};

    send_mcu_frame(fd, 0x81, &hello_payload, 1);
    usleep(50000);
    send_mcu_frame(fd, 0x82, mode1_payload, 1);
    usleep(50000);
    send_mcu_frame(fd, 0x84, state_payload, 2);
}

// Robust stream parser with byte-level synchronization and zero packet loss on line noise.
// Returns 1 on valid frame, 0 on no full frame yet, -1 on I/O error/EOF.
// 2026-09-04: lifted out of read_mcu_frame() (was function-local static)
// so a real reconnect (see McuInputHal::run()'s new staleness-triggered
// reconnect path) can discard whatever partial/stale bytes were sitting
// in the parser across a close()/open() of the underlying fd, via
// reset_mcu_frame_parser() below. Still only ever touched from the one
// MCU reader thread, same as before -- no new thread-safety concern.
unsigned char ring_buf[1024];
size_t ring_len = 0;

void reset_mcu_frame_parser() {
    ring_len = 0;
}

// 2026-09-04: added a bounded wait (select()) in front of the blocking
// read() below, keyed off McuInputHal::run()'s new keepalive-probe/
// staleness-detection logic -- that logic needs the read loop to wake
// up periodically even when the MCU is silent, to check "is it time to
// send a CMD 0x88 probe" / "have we gone too long with zero frames".
// Returns 1 on a valid frame (as before), 0 for BOTH "no full frame
// yet, keep looping" (the old meaning) and "timed out waiting for
// data, no error" (new) -- run() doesn't need to tell these apart, both
// just mean "nothing to act on this call, try again". -1 stays a real
// fatal I/O error/EOF, unchanged.
int read_mcu_frame(int fd, unsigned char * out_cmd, unsigned char * out_payload,
                    unsigned char * out_len, int timeout_ms) {
    while (true) {
        // 1. Hunt for 0x2E synchronization byte at start of buffer
        while (ring_len > 0 && ring_buf[0] != 0x2E) {
            // Discard noise bytes until 0x2E sync byte is at head
            std::memmove(&ring_buf[0], &ring_buf[1], --ring_len);
        }

        // 2. Check if we have at least 3 bytes (sync 0x2E, cmd, length)
        if (ring_len >= 3) {
            unsigned char cmd = ring_buf[1];
            unsigned char length = ring_buf[2];
            size_t total_frame_size = 1 /* 0x2E */ + 1 /* cmd */ + 1 /* len */ + length /* payload */ + 1 /* chk */;

            if (total_frame_size > sizeof(ring_buf)) {
                // Invalid length byte due to corrupt stream -> skip sync byte and re-hunt
                std::memmove(&ring_buf[0], &ring_buf[1], --ring_len);
                continue;
            }

            if (ring_len >= total_frame_size) {
                // Complete frame is present in buffer
                unsigned char chk_calc = mcu_checksum(&ring_buf[1], length + 2);
                unsigned char chk_recv = ring_buf[total_frame_size - 1];

                if (chk_calc == chk_recv) {
                    *out_cmd = cmd;
                    *out_len = length;
                    if (length > 0) {
                        std::memcpy(out_payload, &ring_buf[3], length);
                    }

                    // Consume the frame from the ring buffer
                    size_t rem = ring_len - total_frame_size;
                    if (rem > 0) {
                        std::memmove(&ring_buf[0], &ring_buf[total_frame_size], rem);
                    }
                    ring_len = rem;
                    return 1;
                } else {
                    // Checksum mismatch -> skip the first 0x2E byte and hunt for next valid packet
                    std::memmove(&ring_buf[0], &ring_buf[1], --ring_len);
                    continue;
                }
            }
        }

        // 3. Need more bytes from UART: read into remaining buffer space
        if (ring_len >= sizeof(ring_buf)) {
            // Buffer full of unrecognized data -> reset to avoid stall
            ring_len = 0;
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        int sel = select(fd + 1, &rfds, nullptr, nullptr, &tv);
        if (sel == 0) {
            return 0; // Timed out, no data -- not an error, see this function's own comment
        }
        if (sel < 0) {
            if (errno == EINTR) continue;
            return -1; // Fatal select() error
        }

        ssize_t n = read(fd, &ring_buf[ring_len], sizeof(ring_buf) - ring_len);
        if (n > 0) {
            ring_len += n;
        } else if (n == 0) {
            return -1; // EOF
        } else {
            if (errno == EAGAIN || errno == EINTR) {
                usleep(1000);
                continue;
            }
            return -1; // Fatal I/O error
        }
    }
}

}  // namespace

McuInputHal::McuInputHal(std::string port) : port_(std::move(port)) {
    g_mcu_instance = this;
}

McuInputHal::~McuInputHal() {
    if (g_mcu_instance == this) {
        g_mcu_instance = nullptr;
    }
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) {
        // read_mcu_frame() blocks in read() with no timeout -- closing
        // the fd from here unblocks it with an error, same pattern
        // relied on for the rest of this codebase's blocking-read
        // background threads (e.g. hal::wait_reverse_gear_change()).
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
        thread_.join();
    }
}

bool McuInputHal::start() {
    fd_ = open(port_.c_str(), O_RDWR | O_NOCTTY);
    if (fd_ < 0) {
        std::fprintf(stderr, "%s hal::McuInputHal: couldn't open %s: %s\n", core::log_timestamp().c_str(), port_.c_str(),
                     std::strerror(errno));
        return false;
    }
    if (!set_interface_attribs(fd_)) {
        close(fd_);
        fd_ = -1;
        return false;
    }

    send_startup_sequence(fd_);

    g_mcu_instance = this;
    running_.store(true, std::memory_order_release);
    thread_ = core::SizedThread(core::kDefaultThreadStackSize, &McuInputHal::run, this);
    return true;
}

void McuInputHal::run() {
    unsigned char cmd, payload[256], len;
    // 2026-09-02: the MCU is known (disassembly-confirmed, see this file's
    // own CMD 0x12 case below) to fire a spurious CMD 0x12 once during its
    // own startup telemetry burst, unrelated to reverse gear. Under the
    // old direction-blind "any 0x12 == disengage" mapping this was always
    // harmless (boots already disengaged, so it was a same-value no-op).
    // Under the new payload[0]-direction mapping it's a real bug if that
    // startup frame happens to carry payload[0]==0x01: a real hardware
    // report showed AA's video (fb1) becoming visible through our own
    // LVGL layer (fb0) briefly after boot with no reverse gear ever
    // engaged -- exactly what hide_display() firing on a false "ENGAGED"
    // would produce. The debounce window below only guards flips *after*
    // a first commit, so this startup frame (the very first commit, no
    // prior state to compare against) sailed straight through it. Give
    // the MCU's own init burst a fixed grace window to finish before
    // trusting any CMD 0x12 direction at all. Non-const now (was const)
    // -- a real reconnect (below) needs to restart this grace window
    // too, since the reopened link gets its own fresh startup burst.
    auto run_start = std::chrono::steady_clock::now();
    constexpr auto kStartupGraceWindow = std::chrono::seconds(5);

    // 2026-09-04: first_light/first_reverse used to be block-local
    // statics (initialized once, ever, for the lifetime of this
    // thread). Lifted to run()-scope locals so reconnect() below can
    // reset them to true -- after a real reconnect, the very next CMD
    // 0x01 frame is a fresh unprompted status report (same mechanism
    // confirmed via a real mcu-handshake capture at the original
    // connect), and should be trusted as authoritative again, not
    // treated as just another "did it change from what we already
    // think" comparison against a possibly-now-stale prior value.
    bool first_light = true;
    bool first_reverse = true;

    // 2026-09-04: real hardware gap this was meant to close --
    // reverse_gear_/night_mode_/knob/touch input all depend entirely
    // on this one UART link now (see this session's CMD 0x01 bit-2
    // reverse-gear change), and there was no way to notice a silently
    // dead link short of a hard read() error. Originally paired with
    // an active CMD 0x88 probe every 5s PLUS a "no frame for 15s ->
    // reconnect" staleness check -- REMOVED the staleness/reconnect
    // half after two real hardware captures proved its premise wrong:
    // the probe below is confirmed genuinely transmitted on schedule
    // (its own log line fires every 5s exactly, write() never errors)
    // and never once produced a CMD 0x60/0x88 reply, contrary to what
    // both candidate MCU firmware sources (real vendor disassembly and
    // this project's own clean-room hardware/MCU/source/) suggested it
    // should do -- while a completely healthy link legitimately went
    // 15+ seconds with zero frames of any kind during real idle (no
    // headlights/knob/gear activity). So "no frame recently" was never
    // a valid dead-link signal on this hardware; treating it as one
    // just repeatedly tore down a working connection. Kept: the probe
    // itself (harmless, still tracked via last_probe_sent below, in
    // case some future firmware/condition does answer it) and passive
    // last_frame_time/last_frame_epoch_ms_ tracking (is_link_alive()
    // stays purely informational, see its own header comment) -- ONLY
    // a real read() error/EOF (below) triggers reconnect() now, same
    // as before this whole keepalive feature existed.
    constexpr auto kProbeInterval = std::chrono::seconds(5);
    auto last_probe_sent = run_start;
    auto last_frame_time = run_start;

    // 2026-09-04: real capture analysis (user-driven) found CMD 0x12
    // firing at several distinct real transition points -- a headlights
    // change, a real reverse-gear change, and a HOME-button-driven
    // factory/custom_ui display-mode switch -- not arbitrary MCU
    // activity. Tracked here so the CMD 0x12 log line below can label
    // which nearby tracked event it most likely correlates with,
    // instead of leaving that correlation to manual timestamp
    // cross-referencing across separate log lines every time.
    auto last_headlight_change = std::chrono::steady_clock::time_point{};
    auto last_reverse_change = std::chrono::steady_clock::time_point{};
    auto last_home_button_event = std::chrono::steady_clock::time_point{};

    auto reconnect = [&]() {
        std::fprintf(stderr, "%s [HAL:MCU] Link stale/dead -- reopening %s\n",
                     core::log_timestamp().c_str(), port_.c_str());
        int old_fd = fd_.load(std::memory_order_acquire);
        if (old_fd >= 0) {
            close(old_fd);
        }
        fd_.store(-1, std::memory_order_release);
        reset_mcu_frame_parser();

        int new_fd = open(port_.c_str(), O_RDWR | O_NOCTTY);
        if (new_fd < 0) {
            std::fprintf(stderr, "%s [HAL:MCU] Reopen of %s failed: %s -- will keep retrying\n",
                         core::log_timestamp().c_str(), port_.c_str(), std::strerror(errno));
            return;
        }
        if (!set_interface_attribs(new_fd)) {
            close(new_fd);
            return;
        }
        send_startup_sequence(new_fd);
        fd_.store(new_fd, std::memory_order_release);

        // Fresh link -- give it the same trust/grace treatment as the
        // original connect (see the comments on run_start/first_light/
        // first_reverse above).
        run_start = std::chrono::steady_clock::now();
        first_light = true;
        first_reverse = true;
        last_probe_sent = run_start;
        last_frame_time = run_start;
        std::fprintf(stderr, "%s [HAL:MCU] Reconnected to %s\n",
                     core::log_timestamp().c_str(), port_.c_str());
    };

    while (running_.load(std::memory_order_acquire)) {
        auto now = std::chrono::steady_clock::now();
        if (now - last_probe_sent >= kProbeInterval) {
            static const unsigned char kKeepaliveProbe[8] = {0, 0, 0, 0, 0, 0, 0, 0};
            int cur_fd = fd_.load(std::memory_order_acquire);
            if (cur_fd >= 0) {
                // 2026-09-04: real hardware capture showed repeated
                // "Link stale/dead" reconnects during genuine idle
                // periods with zero CMD 0x88/0x60 reply frames ever
                // observed -- this line exists to answer the first,
                // most basic question that capture couldn't: is the
                // probe actually being transmitted on schedule at all.
                std::printf("%s [HAL:MCU] Sending CMD 0x88 keepalive probe\n",
                            core::log_timestamp().c_str());
                send_mcu_frame(cur_fd, 0x88, kKeepaliveProbe, 8);
            }
            last_probe_sent = now;
        }
        // 2026-09-04: REMOVED the "no frame for kLinkStaleTimeout ->
        // reconnect" branch that used to live here. Real hardware
        // capture (two consecutive tests) proved the premise wrong:
        // the CMD 0x88 probe above is confirmed genuinely transmitted
        // on schedule (its own log line fires every 5s exactly, write()
        // never errors) and NEVER once produced a CMD 0x60/0x88 reply
        // -- contrary to what both candidate MCU firmware sources
        // (real vendor disassembly and this project's own clean-room
        // hardware/MCU/source/) suggested it should do. Meanwhile a
        // completely healthy link legitimately went 15+ seconds with
        // zero frames of any kind during real idle (no headlights/
        // knob/gear activity) -- so "no frame in 15s" was never a
        // valid dead-link signal on this hardware to begin with, and
        // treating it as one just repeatedly tore down a working
        // connection. last_frame_time/last_frame_epoch_ms_ are still
        // tracked below (is_link_alive() stays informational), but
        // nothing acts on staleness anymore -- only a real read()
        // error/EOF (below) triggers reconnect() now, same as before
        // this whole keepalive feature existed.
        int r = read_mcu_frame(fd_, &cmd, payload, &len, /*timeout_ms=*/1000);
        if (r == 1) {
            last_frame_time = std::chrono::steady_clock::now();
            last_frame_epoch_ms_.store(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    last_frame_time.time_since_epoch()).count(),
                std::memory_order_release);
        }
        if (r != 1) {
            // r==0: read timed out or a resync/checksum-fail happened
            // (both expected, not errors) -- r==-1: a real fatal I/O
            // error/EOF, or the fd was closed by the destructor.
            // Reconnect on a real error rather than spinning on a dead
            // fd forever; running_ being cleared (destructor) makes
            // reconnect() itself a harmless no-op-ish reopen that the
            // loop's own exit condition catches on the next check.
            if (r == -1 && running_.load(std::memory_order_acquire)) {
                reconnect();
            }
            continue;
        }

        // 2026-08-31: capture every successfully parsed frame (any cmd)
        // into the live diagnostic log, before per-cmd dispatch below --
        // see get_recent_frames()'s own header comment for why.
        log_frame(cmd, payload, len);

        if (cmd == 0x01 && len >= 1) {
            // CMD 0x01: Headlights / Illumination status broadcast from MCU (len=6)
            // payload[0]: 0x11 = Lights OFF, 0x13 = Lights ON (bit 1 is the illumination bit)
            bool lights_on = (payload[0] & 0x02) != 0;
            bool prev_light = night_mode_.exchange(lights_on, std::memory_order_acq_rel);
            if (lights_on != prev_light && !first_light) {
                // Real change, not just the initial boot-time seed --
                // see the CMD 0x12 branch below, this is one of the
                // real transition points its log line correlates against.
                last_headlight_change = std::chrono::steady_clock::now();
            }
            if (first_light || lights_on != prev_light) {
                first_light = false;
                std::printf("%s [HAL:MCU] Headlights: %s (CMD 0x01 payload[0]=0x%02X -> night_mode=%d)\n",
                            core::log_timestamp().c_str(), lights_on ? "ON" : "OFF", payload[0], lights_on ? 1 : 0);
            }

            // bit 2 (payload[0] & 0x04): reverse-gear status -- PRIMARY
            // source for reverse_gear_ as of 2026-09-03, replacing CMD
            // 0x12 below (which now only cross-checks, see that branch).
            //
            // RE-CONFIRMED 2026-09-03, real hardware, decisive test: the
            // earlier conclusion here ("symmetric transient pulse, no
            // held state") didn't survive a stricter check. Prior
            // captures were all single-line, moment-of-transition
            // observations -- equally consistent with a pulse or a held
            // level, never actually distinguishing the two. This test
            // closed that gap directly: exactly one CMD 0x01 frame
            // arrived on ENTERING reverse (payload[0]=0x15, bit set),
            // and critically, NO further CMD 0x01 frame arrived while
            // still sitting in reverse -- no spontaneous mid-reverse
            // reversion, the one thing a real pulse would produce. The
            // bit only cleared (payload[0]=0x11) on the frame marking
            // the real EXIT. That's a real, held, level-encoded field --
            // the same transmission convention as bit 1 (headlights):
            // re-sent only when the underlying level actually changes,
            // not on a timer or a self-clearing pulse.
            //
            // This also directly answers the original motivating
            // question (does custom_ui have any way to learn the real
            // current reverse-gear state at load time, e.g. if the
            // vehicle is already in reverse when it starts) -- yes: this
            // same CMD 0x01 frame is confirmed (real capture, see git
            // history) to arrive unprompted right after connecting, with
            // real current bit1/bit2 values, same as headlights. The
            // first_reverse flag below applies that first frame as
            // authoritative the same way first_light does for
            // headlights just above.
            //
            // CMD 0x12 is demoted to a corroboration-only cross-check:
            // it's separately confirmed to false-trigger from headlights
            // alone with zero gear involvement (see this file's CMD 0x12
            // branch and docs/MCU_COMMAND_REFERENCE.md's conflict
            // section), so it was never a trustworthy source of truth on
            // its own -- CMD 0x01 doesn't share that failure mode.
            bool reversing = (payload[0] & 0x04) != 0;
            bool prev_reverse = reverse_gear_.exchange(reversing, std::memory_order_acq_rel);
            if (reversing != prev_reverse && !first_reverse) {
                last_reverse_change = std::chrono::steady_clock::now();
            }
            if (first_reverse || reversing != prev_reverse) {
                first_reverse = false;
                std::printf("%s [HAL:MCU] Reverse gear: %s (CMD 0x01 payload[0]=0x%02X)\n",
                            core::log_timestamp().c_str(), reversing ? "ENGAGED" : "DISENGAGED", payload[0]);
            }
        } else if (cmd == 0x04) {
            // CMD 0x04 is real, disassembly-confirmed parking radar/distance
            // telemetry (transRadarLevel), NOT a reverse-gear boolean -- its
            // old "presence == engaged" mapping was never disassembly-
            // verified and correlated only because parking sensors happen
            // to activate around the same time as reversing. Demoted
            // 2026-09-01: no longer touches reverse_gear_ at all. See
            // docs/MCU_COMMAND_REFERENCE.md's "reverse-gear command
            // conflict" section. Kept here only as a no-op placeholder so
            // future work doesn't have to rediscover this dead end.
        } else if (cmd == 0x12 && len >= 1) {
            // CMD 0x12: real LCD-source state broadcast -- payload[0]
            // states which LCD is currently active, not a reverse-gear
            // direction. RE-UNDERSTOOD 2026-09-04, user-driven: earlier
            // theories here (direction-blind, then direction-based, then
            // "some other internal mode/state flag") all missed this --
            // 0x01 = Factory LCD mode (OEM feed active), 0x02 =
            // Aftermarket LCD mode (custom_ui feed active). Explains
            // every false-trigger source found so far in one shot: a
            // real reverse-gear engage genuinely switches to the Factory
            // LCD (OEM camera relay) and disengage switches back, but so
            // does a HOME-button long-press (user-confirmed: switches
            // factory<->custom_ui screens directly) and, per earlier
            // captures, a plain headlights toggle -- all three are real
            // LCD-source switches from the MCU's point of view, this
            // command just reports which one is now active, regardless
            // of what caused it. See docs/MCU_COMMAND_REFERENCE.md for
            // the full history of superseded theories on this command.
            //
            // DEMOTED 2026-09-03 as a reverse_gear_ source: CMD 0x01 bit
            // 2 (see that branch above) is the real, held, level-encoded
            // reverse-gear field -- this command was never a reliable
            // reverse-gear signal on its own (it reports LCD source, not
            // gear state), kept here purely as a diagnostic log now.
            uint8_t dir = payload[0];
            if (dir == 0x01 || dir == 0x02) {
                auto now = std::chrono::steady_clock::now();
                bool inStartupGrace = (now - run_start) < kStartupGraceWindow;
                const char * lcd_mode = (dir == 0x01) ? "Factory LCD mode" : "Aftermarket LCD mode";
                if (!inStartupGrace) {
                    // 2026-09-04: user-driven correlation finding -- this
                    // LCD-mode switch coincides with a real transition
                    // point (a headlights change, a real reverse-gear
                    // change, or a HOME-button switch), not arbitrary MCU
                    // activity. Label the log line with whichever tracked
                    // event happened most recently, if within a real
                    // correlation window -- closes the loop this doc's
                    // own investigation kept needing (manually cross-
                    // referencing separate log lines by timestamp)
                    // directly in the log itself.
                    constexpr auto kCorrelationWindow = std::chrono::milliseconds(3000);
                    const char * label = "unexplained (no tracked event in the last 3s)";
                    auto best_delta = std::chrono::steady_clock::duration::max();
                    if (last_headlight_change.time_since_epoch().count() != 0 &&
                        (now - last_headlight_change) < kCorrelationWindow &&
                        (now - last_headlight_change) < best_delta) {
                        best_delta = now - last_headlight_change;
                        label = "near a headlights change";
                    }
                    if (last_reverse_change.time_since_epoch().count() != 0 &&
                        (now - last_reverse_change) < kCorrelationWindow &&
                        (now - last_reverse_change) < best_delta) {
                        best_delta = now - last_reverse_change;
                        label = "near a real CMD 0x01-driven reverse-gear change";
                    }
                    if (last_home_button_event.time_since_epoch().count() != 0 &&
                        (now - last_home_button_event) < kCorrelationWindow &&
                        (now - last_home_button_event) < best_delta) {
                        best_delta = now - last_home_button_event;
                        label = "near a HOME-button display-mode switch";
                    }
                    std::printf("%s [HAL:MCU] CMD 0x12 payload[0]=0x%02X -> %s -- %s -- logged only, not acted on\n",
                                core::log_timestamp().c_str(), dir, lcd_mode, label);
                }
            }
        } else if (cmd == 0x20 && len >= 5) {
            unsigned char b3 = payload[0];
            unsigned char b4 = payload[1];
            unsigned char b5 = payload[2];
            unsigned char b6 = payload[3];
            unsigned char b7 = payload[4];

            // All-zero payload == release/idle, confirmed directly in
            // the corner-touch capture (docs/MCU_ADAPTERS.md) -- every
            // touch event was bounded by one of these on each side.
            bool released = (b3 == 0 && b4 == 0 && b5 == 0 && b6 == 0 && b7 == 0);

            if (released) {
                touch_pressed_.store(false, std::memory_order_release);
                continue;
            }

            int32_t x = (static_cast<int32_t>(b4) << 8) | b3;
            int32_t y = (static_cast<int32_t>(b6) << 8) | b5;

            x_.store(x, std::memory_order_relaxed);
            y_.store(y, std::memory_order_relaxed);
            touch_pressed_.store(true, std::memory_order_release);
            auto now = std::chrono::steady_clock::now();
            uint64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
            last_touch_ms_.store(ms, std::memory_order_relaxed);
        } else if (cmd == 0x02 && len >= 2) {
            unsigned char b3 = payload[0];
            unsigned char b4 = payload[1];

            if (b3 == kKnobClockwise && b4 == 1) {
                knob_ticks_.fetch_add(1, std::memory_order_relaxed);
            } else if (b3 == kKnobCounterClockwise && b4 == 1) {
                knob_ticks_.fetch_sub(1, std::memory_order_relaxed);
            } else if (b3 == kKnobPush) {
                knob_pressed_.store(b4 == 1, std::memory_order_release);
            } else if (b3 == kBtnHome) {
                if (b4 == 1) {
                    static bool s_drawer_open = false;
                    static auto s_last_press_time = std::chrono::steady_clock::now();
                    auto now = std::chrono::steady_clock::now();
                    last_home_button_event = now;
                    auto elapsed_s = std::chrono::duration_cast<std::chrono::seconds>(now - s_last_press_time).count();
                    s_last_press_time = now;
                    if (elapsed_s > 10) {
                        s_drawer_open = false;
                    }

                    AndroidAutoClient client;
                    if (!s_drawer_open) {
                        std::printf("%s [HAL:MCU] Button: HOME -> Open App Launcher (KEYCODE_HOME=3)\n", core::log_timestamp().c_str());
                        client.sendKey(3 /* KEYCODE_HOME */);
                        s_drawer_open = true;
                    } else {
                        std::printf("%s [HAL:MCU] Button: HOME -> Return to Navigation/Map (KEYCODE_NAVIGATION=65538)\n", core::log_timestamp().c_str());
                        client.sendKey(65538 /* KEYCODE_NAVIGATION */);
                        s_drawer_open = false;
                    }
                }
            } else if (b3 == kBtnNextTrack) {
                std::printf("%s [HAL:MCU] Button: NEXT_TRACK (b3=3 b4=%u)\n", core::log_timestamp().c_str(), b4);
                AndroidAutoClient client;
                client.sendKey(87 /* KEYCODE_MEDIA_NEXT */);
            } else if (b3 == kBtnPrevTrack) {
                std::printf("%s [HAL:MCU] Button: PREV_TRACK (b3=4 b4=%u)\n", core::log_timestamp().c_str(), b4);
                AndroidAutoClient client;
                client.sendKey(88 /* KEYCODE_MEDIA_PREVIOUS */);
            } else if (b3 == kBtnAnswer) {
                std::printf("%s [HAL:MCU] Button: ANSWER_CALL (b3=8 b4=%u)\n", core::log_timestamp().c_str(), b4);
                AndroidAutoClient client;
                client.sendKey(5 /* KEYCODE_CALL */);
            } else if (b3 == kBtnHangup) {
                std::printf("%s [HAL:MCU] Button: HANGUP_CALL (b3=9 b4=%u)\n", core::log_timestamp().c_str(), b4);
                AndroidAutoClient client;
                client.sendKey(6 /* KEYCODE_ENDCALL */);
            } else {
                std::printf("%s [HAL:MCU] Unhandled cmd=0x02 b3=0x%02X (%u) b4=0x%02X (%u)\n",
                            core::log_timestamp().c_str(), b3, b3, b4, b4);
            }
        } else if (cmd == 0x7F && len > 0) {
            std::string ver(reinterpret_cast<const char*>(payload), len);
            {
                std::lock_guard<std::mutex> lock(version_mutex_);
                mcu_version_ = ver;
            }
            std::printf("%s [HAL:MCU] MCU Firmware Version: \"%s\" (CMD 0x7F len=%u)\n",
                        core::log_timestamp().c_str(), ver.c_str(), len);
        } else if (cmd == 0x30 && len >= 1) {
            float v = static_cast<float>(payload[0]) + (len >= 2 ? static_cast<float>(payload[1]) / 100.0f : 0.0f);
            battery_voltage_.store(v, std::memory_order_relaxed);
            std::printf("%s [HAL:MCU] Vehicle Battery Voltage: %.2fV (CMD 0x30)\n",
                        core::log_timestamp().c_str(), v);
        }
        // 2026-09-02: no more generic "unhandled command" dump here --
        // log_frame() above now unconditionally prints every frame's
        // raw cmd+payload to console, so every command (handled or not)
        // already gets a raw dump line before any of this function's
        // own interpreted output. This `else` block would have just
        // duplicated that line for whichever commands fall through
        // without their own specific case.
    }
}

McuTouchState McuInputHal::get_touch_state() const {
    McuTouchState s;
    bool pressed = touch_pressed_.load(std::memory_order_acquire);
    if (pressed) {
        auto now = std::chrono::steady_clock::now();
        uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        uint64_t last_ms = last_touch_ms_.load(std::memory_order_relaxed);
        if (now_ms > last_ms + 200) {
            // Auto-release watchdog: finger lifted beyond digitizer active area
            touch_pressed_.store(false, std::memory_order_release);
            pressed = false;
        }
    }
    s.pressed = pressed;
    s.x = x_.load(std::memory_order_relaxed);
    s.y = y_.load(std::memory_order_relaxed);
    return s;
}

int32_t McuInputHal::consume_knob_ticks() {
    return knob_ticks_.exchange(0, std::memory_order_relaxed);
}

bool McuInputHal::get_knob_pressed() const {
    return knob_pressed_.load(std::memory_order_acquire);
}

bool McuInputHal::get_night_mode() const {
    return night_mode_.load(std::memory_order_acquire);
}

bool McuInputHal::get_reverse_gear() const {
    return reverse_gear_.load(std::memory_order_acquire);
}

bool McuInputHal::is_link_alive() const {
    int64_t last_ms = last_frame_epoch_ms_.load(std::memory_order_acquire);
    if (last_ms == 0) {
        return false; // Never received a single frame yet
    }
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    // 2026-09-04: NOT tied to run()'s own reconnect logic anymore (see
    // run()'s big comment on why the staleness-triggered reconnect was
    // removed) -- purely informational now. Widened well past the old
    // 15s to something that should never legitimately fire during real
    // idle (two real hardware captures showed a healthy link going
    // 15+ seconds with zero frames), while still catching a genuinely
    // dead link eventually. This threshold itself is a rough, honest
    // guess, not hardware-confirmed -- this project doesn't actually
    // know the true maximum legitimate idle gap on this hardware.
    constexpr int64_t kLinkStaleTimeoutMs = 60000;
    return (now_ms - last_ms) < kLinkStaleTimeoutMs;
}

std::string McuInputHal::get_mcu_version() const {
    std::lock_guard<std::mutex> lock(version_mutex_);
    return mcu_version_;
}

float McuInputHal::get_battery_voltage() const {
    return battery_voltage_.load(std::memory_order_relaxed);
}

void McuInputHal::log_frame(unsigned char cmd, const unsigned char * payload, unsigned char len) {
    char buf[16 + 3 * 256];
    char sub[6];
    // sub = payload[0], the byte most commands treat as a sub-type/state/
    // direction selector -- printed explicitly (matching tools/mcu-handshake's
    // own log_frame() convention, 2026-09-03) alongside the full raw payload,
    // which was already shown here for every frame unconditionally.
    if (len >= 1) {
        std::snprintf(sub, sizeof(sub), "0x%02X", payload[0]);
    } else {
        std::snprintf(sub, sizeof(sub), "n/a");
    }
    int n = std::snprintf(buf, sizeof(buf), "%s [HAL:MCU] Frame cmd=0x%02X len=%u sub=%s payload=[",
                           core::log_timestamp().c_str(), cmd, len, sub);
    for (unsigned char i = 0; i < len && n < static_cast<int>(sizeof(buf)) - 4; ++i) {
        n += std::snprintf(buf + n, sizeof(buf) - n, "%02X%s", payload[i], (i + 1 < len) ? " " : "");
    }
    std::snprintf(buf + n, sizeof(buf) - n, "]");

    // 2026-09-02: real hardware need -- a false reverse-gear trigger
    // happened with the vehicle stationary and only the headlights
    // touched, and diagnosing it needed the raw cmd+payload for EVERY
    // frame (not just the ones an unhandled-command falls through to
    // the generic dump below), pulled straight from the console/serial
    // log the user already has open -- not just the ring buffer this
    // function already fed into the MCU Live Log screen, which needs
    // touching the touchscreen to view and wasn't reachable while
    // stuck on a false-triggered reverse-camera screen. Print every
    // frame here unconditionally, in addition to storing it, so the
    // regular boot/console log carries full CMD+payload detail for
    // every frame without needing the on-screen viewer at all.
    std::printf("%s\n", buf);

    std::lock_guard<std::mutex> lock(frame_log_mutex_);
    frame_log_.emplace_back(buf);
    while (frame_log_.size() > kFrameLogCapacity) {
        frame_log_.pop_front();
    }
}

std::vector<std::string> McuInputHal::get_recent_frames() const {
    std::lock_guard<std::mutex> lock(frame_log_mutex_);
    return std::vector<std::string>(frame_log_.begin(), frame_log_.end());
}

void McuInputHal::sync_setting(uint8_t setting_id, uint8_t value) {
    if (fd_ >= 0) {
        unsigned char payload[2] = {setting_id, value};
        send_mcu_frame(fd_, 0xA0, payload, 2);
        std::printf("%s [HAL:MCU] Synced setting to MCU via CMD 0xA0 (id=0x%02X, val=0x%02X)\n",
                    core::log_timestamp().c_str(), setting_id, value);
    }
}

void send_mcu_setting(uint8_t setting_id, uint8_t value) {
    if (g_mcu_instance) {
        g_mcu_instance->sync_setting(setting_id, value);
    }
}

void McuInputHal::sync_audio_route(uint8_t value) {
    if (fd_ >= 0) {
        unsigned char payload[1] = {value};
        send_mcu_frame(fd_, 0x84, payload, 1);
        std::printf("%s [HAL:MCU] Sent CMD 0x84 Audio Route (val=0x%02X)\n",
                    core::log_timestamp().c_str(), value);
    }
}

void McuInputHal::sync_video_relay(bool oem) {
    if (fd_ < 0) {
        return;
    }
    /* CORRECTED 2026-08-31 (real hardware bug report: toggle had no
     * effect, video stayed stuck on whatever the real stock app last
     * set, even though a live-hardware toggle from the previous
     * MCUAdapter_BoxP300::syncSettingDataToMcu()-derived id=0x01 sent
     * cleanly). Re-checked id=0x01 directly against can_app.bin's own
     * CMD 0xA0 dispatch table -- it's a confirmed, unconditional no-op
     * on the real MCU firmware: ids 0x01-0x06 and 0x0e all resolve to
     * the exact same handler at 0x08008b88, which is `nop; nop; pop
     * {r4,pc}`, verified via the real TBB jump-table bytes, not
     * assumed from the disassembly listing's own (separately known to
     * sometimes misdecode inline data) linear output. The earlier
     * comment below is still real and accurate about what the STOCK
     * APP itself sends for this setting (a genuine, disassembly-
     * confirmed finding) -- it just turns out the real MCU firmware
     * ignores that command entirely, a real stock-vendor bug this
     * project inherited by faithfully replicating the app-side
     * behavior without also checking the MCU-side effect.
     *
     * main.cpp's own startup sync and reverse-gear transition handler
     * were never subject to this bug -- they already send CMD 0xA0
     * id=0x11 for this same logical setting (hardware/MCU/source's
     * own uart_protocol.h struct comment: id=0x11's real target is
     * GPIOC Pin 13, the actual camera/video relay -- gated by an
     * internal flag whose real trigger condition is still unconfirmed,
     * but real-hardware behavior indicates it's satisfied in practice).
     * Switched this function to match, closing the inconsistency
     * between the two code paths for the same setting.
     *
     * Original finding, preserved for the record -- real and accurate
     * about what the stock app sends, just not sufficient on its own:
     * direct disassembly of MCUAdapter_BoxP300::syncSettingDataToMcu(int)
     * (usr/lib/libMcuCenter.so, 0x38df8), the confirmed-active MCU adapter
     * class's own real "send this setting to the MCU" function. Traced
     * byte-for-byte, not inferred:
     *   - CMD byte passed to makeMCUProtocol() is literally 0xA0 (r2=160
     *     at 0x38f4c).
     *   - payload[0] is (uint8_t)idx, i.e. the setting id, UNMODIFIED for
     *     idx=1 (only idx 10/11/12 get special remapping in this function;
     *     1 isn't one of them) -- confirmed at 0x38f34/0x38f54.
     *   - MCUAdapter_BoxP300::getSetItemValueTexts(1) (0x36750) appends
     *     exactly 4 real strings in this order: "AfterMarket Camera",
     *     "Factory Camera", "AfterMarket 360", "Factory 360" -- a Qt
     *     combobox's value list, so list order == value order (0/1/2/3).
     * This supersedes and REPLACES the retracted CanBus_Raise_Toyota lead
     * (usr/lib/libCanBus.so is confirmed dead code on this hardware, never
     * dlopen'd by anything -- see MCU_FIRMWARE_VERIFIED_FINDINGS.md's
     * "RETRACTION" section). The stock app sends CMD 0xA0 id=0x01: value
     * 0 = AfterMarket Camera, value 1 = Factory (OEM) Camera -- but per
     * the correction above, id=0x01 doesn't do anything on this
     * firmware, so this function now sends id=0x11 instead. */
    unsigned char payload[2] = {0x11, static_cast<unsigned char>(oem ? 0x01 : 0x00)};
    send_mcu_frame(fd_, 0xA0, payload, 2);
    std::printf("%s [HAL:MCU] Synced Camera Type to MCU via CMD 0xA0 (id=0x11, val=0x%02X, %s)\n",
                core::log_timestamp().c_str(), payload[1], oem ? "Factory/OEM" : "AfterMarket");

    /* Added (2026-09-02, real hardware bug report): "OEM camera stuck
     * after a settings change, SoC reboot doesn't fix it, but booting
     * into stock MsnCoreApp does." The id=0x11 send above is gated on
     * flag_5e (struct offset 0x5e) already reading 1 -- per the
     * arm-then-trigger design, it only *immediately* forces the
     * GPIOC13/PC2 relay while the MCU currently thinks it's in reverse;
     * the rest of the time (the common case, including right after
     * boot or a settings change made while not reversing) it only
     * updates the stored preference, and the relay itself doesn't move
     * until the MCU's own next real GPIOB Pin 2 edge. Since the MCU is
     * a separate, continuously-powered chip (confirmed unaffected by
     * any SoC-side reboot), a stale relay state from before a settings
     * change can persist indefinitely with nothing to give it a fresh
     * edge to correct itself on.
     *
     * CMD 0x84 (see docs/MCU_COMMAND_REFERENCE.md's own entry) drives
     * the exact same relay dispatcher via a real, separate,
     * disassembly-confirmed value mapping (0=state0/LVGL/Aftermarket,
     * 3=state1/OEM) with its own gate polarity that's the opposite of
     * id=0x11's -- stock MsnCoreApp very plausibly sends this
     * unconditionally as part of its own settings sync, which is the
     * most likely reason booting into it fixes what an id=0x11-only
     * sync cannot. sync_audio_route() already existed as a real HAL
     * wrapper for this exact command but was never called anywhere --
     * wiring it in here gives this the same immediate-force capability
     * stock apparently has. */
    sync_audio_route(oem ? 0x03 : 0x00);

    /* Added (2026-09-02, real hardware finding -- CONFIRMED, not a
     * hypothesis like CMD 0x84 above): user methodically toggled
     * CMD 0xA0 id=0x00 (the row this UI mislabeled "Microphone Source
     * (OEM/AfterMarket)" -- turned out to be a real indexing error in
     * this project's own earlier analysis, not a vendor bug:
     * getSetItemValueTexts(0)'s real strings are "AfterMarket Camera"/
     * "Factory Camera"/"AfterMarket 360"/"Factory 360", independently
     * re-derived 2026-09-02 -- id=0x00 is consistently, correctly the
     * camera setting by both name and function, see
     * docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md's own correction section)
     * and tested real reverse gear after each toggle, with id=0x11
     * held FIXED throughout to rule out an interaction effect --
     * id=0x00 alone reliably controlled
     * whether the OEM relay engaged. Follow-up, also real: with
     * id=0x00 held fixed instead and id=0x11 toggled/tested the same
     * way, id=0x11 "didn't seem to do anything" -- id=0x00 is the one
     * lever confirmed to actually work on this vehicle's real wiring.
     * Kept id=0x11's own send above rather than removing it (harmless
     * either way per this same testing, and it's still the one thing
     * this doc's own MCU-side disassembly independently confirms has a
     * real GPIOC13/PC2 effect under its own gate condition) -- adding
     * id=0x00 here rather than replacing anything, since this is the
     * confirmed-working signal, not a guess. Real MCU-side handler for
     * id=0x00 (hardware/MCU/source/src/uart_protocol.c) drives GPIOB
     * Pin 1: value 1 = HIGH, value 0/3 = LOW -- inverted polarity from
     * id=0x11 (0=AfterMarket/1=Factory there), matching the polarity
     * this project's own now-removed standalone toggle already used
     * (oem ? 0x00 : 0x01). */
    unsigned char id0_payload[2] = {0x00, static_cast<unsigned char>(oem ? 0x00 : 0x01)};
    send_mcu_frame(fd_, 0xA0, id0_payload, 2);
    std::printf("%s [HAL:MCU] Synced Camera Type to MCU via CMD 0xA0 (id=0x00, val=0x%02X, %s) -- confirmed-working lever\n",
                core::log_timestamp().c_str(), id0_payload[1], oem ? "Factory/OEM" : "AfterMarket");
}

void send_mcu_audio_route(uint8_t value) {
    if (g_mcu_instance) {
        g_mcu_instance->sync_audio_route(value);
    }
}

void send_mcu_video_relay(bool oem) {
    if (g_mcu_instance) {
        g_mcu_instance->sync_video_relay(oem);
    }
}

McuInputHal * get_mcu_instance() {
    return g_mcu_instance;
}

std::string get_mcu_version() {
    if (g_mcu_instance) {
        return g_mcu_instance->get_mcu_version();
    }
    return "Unknown (Standalone)";
}

float get_mcu_battery_voltage() {
    if (g_mcu_instance) {
        return g_mcu_instance->get_battery_voltage();
    }
    return 0.0f;
}

std::vector<std::string> get_mcu_recent_frames() {
    if (g_mcu_instance) {
        return g_mcu_instance->get_recent_frames();
    }
    return {};
}

}  // namespace hal
