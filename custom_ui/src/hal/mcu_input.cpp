#include "hal/mcu_input.h"
#include "hal/androidauto_client.h"
#include "core/log_timing.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
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
void send_startup_sequence(int fd) {
    unsigned char hello_payload = 0x01;
    unsigned char mode4_payload[9] = {0x01, 0x08, 0, 0, 0, 0, 0, 0, 0};
    unsigned char state_payload[2] = {0x00, 0x03};

    send_mcu_frame(fd, 0x81, &hello_payload, 1);
    usleep(50000);
    send_mcu_frame(fd, 0x82, mode4_payload, 9);
    usleep(50000);
    send_mcu_frame(fd, 0x84, state_payload, 2);
    usleep(50000);
    send_mcu_frame(fd, 0x85, nullptr, 0);
}

// Robust stream parser with byte-level synchronization and zero packet loss on line noise.
// Returns 1 on valid frame, 0 on no full frame yet, -1 on I/O error/EOF.
int read_mcu_frame(int fd, unsigned char * out_cmd, unsigned char * out_payload,
                    unsigned char * out_len) {
    static unsigned char ring_buf[1024];
    static size_t ring_len = 0;

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
    while (running_.load(std::memory_order_acquire)) {
        int r = read_mcu_frame(fd_, &cmd, payload, &len);
        if (r != 1) {
            // r==0: resync/checksum-fail, r==-1: I/O hiccup or fd
            // closed by the destructor -- both just loop (or exit if
            // running_ was cleared). Guard against tight 100% CPU spinning on I/O errors:
            if (r == -1) {
                usleep(20000);
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
            static bool first_light = true;
            bool prev_light = night_mode_.exchange(lights_on, std::memory_order_acq_rel);
            if (first_light || lights_on != prev_light) {
                first_light = false;
                std::printf("%s [HAL:MCU] Headlights: %s (CMD 0x01 payload[0]=0x%02X -> night_mode=%d)\n",
                            core::log_timestamp().c_str(), lights_on ? "ON" : "OFF", payload[0], lights_on ? 1 : 0);
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
            // CMD 0x12: real reverse-gear engage/disengage push, hardware-
            // confirmed 2026-09-01 -- payload[0] carries the direction,
            // not just the command byte's mere presence (the old "any 0x12
            // == disengaged" mapping was wrong; it fires on BOTH edges).
            // Confirmed via a real enter-then-exit test correlated against
            // MCU Live Log captures: payload=[01 04 00] on entering
            // (payload[0]==0x01), payload=[02 01 00] on exiting
            // (payload[0]==0x02). Pending a second real-world retest to
            // confirm this mapping before treating it as fully settled.
            if (payload[0] == 0x01) {
                bool prev = reverse_gear_.exchange(true, std::memory_order_acq_rel);
                if (!prev) {
                    std::printf("%s [HAL:MCU] Reverse gear: ENGAGED (CMD 0x12 payload[0]=0x01)\n",
                                core::log_timestamp().c_str());
                }
            } else if (payload[0] == 0x02) {
                bool prev = reverse_gear_.exchange(false, std::memory_order_acq_rel);
                if (prev) {
                    std::printf("%s [HAL:MCU] Reverse gear: DISENGAGED (CMD 0x12 payload[0]=0x02)\n",
                                core::log_timestamp().c_str());
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
        } else {
            std::printf("%s [HAL:MCU] Frame cmd=0x%02X len=%u payload=[",
                        core::log_timestamp().c_str(), cmd, len);
            for (unsigned char i = 0; i < len; ++i) {
                std::printf("%02X%s", payload[i], (i + 1 < len) ? " " : "");
            }
            std::printf("]\n");
        }
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

std::string McuInputHal::get_mcu_version() const {
    std::lock_guard<std::mutex> lock(version_mutex_);
    return mcu_version_;
}

float McuInputHal::get_battery_voltage() const {
    return battery_voltage_.load(std::memory_order_relaxed);
}

void McuInputHal::log_frame(unsigned char cmd, const unsigned char * payload, unsigned char len) {
    char buf[16 + 3 * 256];
    int n = std::snprintf(buf, sizeof(buf), "%s cmd=0x%02X len=%u payload=[",
                           core::log_timestamp().c_str(), cmd, len);
    for (unsigned char i = 0; i < len && n < static_cast<int>(sizeof(buf)) - 4; ++i) {
        n += std::snprintf(buf + n, sizeof(buf) - n, "%02X%s", payload[i], (i + 1 < len) ? " " : "");
    }
    std::snprintf(buf + n, sizeof(buf) - n, "]");

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
