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

McuInputHal::McuInputHal(std::string port) : port_(std::move(port)) {}

McuInputHal::~McuInputHal() {
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

        if (cmd == 0x01 && len >= 1) {
            // CMD 0x01: Headlights / Illumination status broadcast from MCU (len=6)
            std::printf("%s [HAL:MCU] Headlights (CMD 0x01) len=%u payload=[",
                        core::log_timestamp().c_str(), len);
            for (unsigned char i = 0; i < len; ++i) {
                std::printf("%02X%s", payload[i], (i + 1 < len) ? " " : "");
            }
            // If any lighting bit is set, headlights are ON
            bool lights_on = (len >= 4 && payload[3] != 0) || (payload[0] != 0);
            std::printf("] -> night_mode=%d\n", lights_on ? 1 : 0);
            night_mode_.store(lights_on, std::memory_order_release);
        } else if (cmd == 0x04) {
            std::printf("%s [HAL:MCU] Reverse gear: ENGAGED (CMD 0x04)\n", core::log_timestamp().c_str());
            reverse_gear_.store(true, std::memory_order_release);
        } else if (cmd == 0x12) {
            std::printf("%s [HAL:MCU] Reverse gear: DISENGAGED (CMD 0x12)\n", core::log_timestamp().c_str());
            reverse_gear_.store(false, std::memory_order_release);
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
                    std::printf("%s [HAL:MCU] Button: HOME (b3=12 b4=1)\n", core::log_timestamp().c_str());
                    AndroidAutoClient client;
                    client.sendKey(3 /* KEYCODE_HOME */);
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

}  // namespace hal
