#include "hal/mcu_touch.h"

#include <cerrno>
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
        perror("hal::McuTouchHal: tcgetattr");
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
        perror("hal::McuTouchHal: tcsetattr");
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
        perror("hal::McuTouchHal: write (send_mcu_frame)");
    }
}

// Matches send_startup_sequence() in mcu-handshake.c -- see that file's
// header comment for full provenance (disassembly of
// MCUAdapter_BoxP300 in libMcuCenter.so). Sent here because every real
// capture of CMD 0x20 touch traffic this project has was taken with
// this sequence already sent (mcu-handshake's default) -- whether it's
// strictly required for the MCU to report touch isn't confirmed, but
// it's cheap/harmless and matches the only known-working configuration.
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

// Matches read_mcu_frame() in mcu-handshake.c. Blocking; returns 1 with
// cmd/payload/length filled in on a validated frame, 0 on a
// resync/checksum failure, -1 on I/O error/EOF.
int read_mcu_frame(int fd, unsigned char * out_cmd, unsigned char * out_payload,
                    unsigned char * out_len) {
    unsigned char sig;
    if (read(fd, &sig, 1) <= 0) return -1;
    if (sig != 0x2E) return 0;

    unsigned char header[2];
    int header_read = 0;
    while (header_read < 2) {
        int n = read(fd, &header[header_read], 2 - header_read);
        if (n > 0) {
            header_read += n;
        } else if (n == 0) {
            return -1;
        } else if (errno != EAGAIN && errno != EINTR) {
            break;
        }
    }
    if (header_read < 2) return 0;

    unsigned char cmd = header[0];
    unsigned char length = header[1];

    unsigned char remaining[256];
    int req_len = length + 1;
    int rem_read = 0;
    while (rem_read < req_len) {
        int n = read(fd, &remaining[rem_read], req_len - rem_read);
        if (n > 0) {
            rem_read += n;
        } else if (n == 0) {
            return -1;
        } else if (errno != EAGAIN && errno != EINTR) {
            break;
        }
    }
    if (rem_read < req_len) return 0;

    unsigned char chk_recv = remaining[length];
    unsigned char check_buf[256];
    check_buf[0] = cmd;
    check_buf[1] = length;
    std::memcpy(&check_buf[2], remaining, length);
    unsigned char chk_calc = mcu_checksum(check_buf, length + 2);

    if (chk_calc != chk_recv) return 0;

    *out_cmd = cmd;
    *out_len = length;
    std::memcpy(out_payload, remaining, length);
    return 1;
}

}  // namespace

McuTouchHal::McuTouchHal(std::string port) : port_(std::move(port)) {}

McuTouchHal::~McuTouchHal() {
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

bool McuTouchHal::start() {
    fd_ = open(port_.c_str(), O_RDWR | O_NOCTTY);
    if (fd_ < 0) {
        std::fprintf(stderr, "hal::McuTouchHal: couldn't open %s: %s\n", port_.c_str(),
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
    thread_ = std::thread(&McuTouchHal::run, this);
    return true;
}

void McuTouchHal::run() {
    unsigned char cmd, payload[256], len;
    while (running_.load(std::memory_order_acquire)) {
        int r = read_mcu_frame(fd_, &cmd, payload, &len);
        if (r != 1) {
            // r==0: resync/checksum-fail, r==-1: I/O hiccup or fd
            // closed by the destructor -- both just loop (or exit if
            // running_ was cleared).
            continue;
        }

        if (cmd != 0x20 || len < 5) continue;

        unsigned char b3 = payload[0];
        unsigned char b4 = payload[1];
        unsigned char b5 = payload[2];
        unsigned char b6 = payload[3];
        unsigned char b7 = payload[4];

        // All-zero payload == release/idle, confirmed directly in the
        // corner-touch capture (docs/MCU_ADAPTERS.md) -- every touch
        // event was bounded by one of these on each side.
        bool released = (b3 == 0 && b4 == 0 && b5 == 0 && b6 == 0 && b7 == 0);

        if (released) {
            pressed_.store(false, std::memory_order_release);
            continue;
        }

        int32_t x = (static_cast<int32_t>(b4) << 8) | b3;
        int32_t y = (static_cast<int32_t>(b6) << 8) | b5;

        x_.store(x, std::memory_order_relaxed);
        y_.store(y, std::memory_order_relaxed);
        pressed_.store(true, std::memory_order_release);
    }
}

McuTouchState McuTouchHal::get_state() const {
    McuTouchState s;
    s.pressed = pressed_.load(std::memory_order_acquire);
    s.x = x_.load(std::memory_order_relaxed);
    s.y = y_.load(std::memory_order_relaxed);
    return s;
}

}  // namespace hal
