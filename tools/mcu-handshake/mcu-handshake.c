/*
 * mcu-handshake -- emulates MsnCoreApp's side of the SoC<->MCU protocol
 * on /dev/ttyHS0, to test/trigger whatever makes the MCU close the
 * CBT16211A touch-bus switch (see README.md) without needing the full
 * Qt/MsnCoreApp stack running.
 *
 * Protocol details below are from direct disassembly of
 * MCUAdapter_BoxP300 in libMcuCenter.so (Prado's real active MCU
 * adapter, McuType=6) -- not guessed. Three passes this session:
 *
 * 1. 2026-07-18: MCUAdapter_BoxP300::getPortSettings() -- confirmed
 *    38400 baud, 8N1 (a static struct copy in .rodata, no computation).
 * 2. 2026-07-18: onRecvMcuProtocol(), makeMCUProtocol(), onInited() --
 *    corrected two wrong assumptions an earlier version of this tool
 *    made:
 *
 *    - The real MCU-wire frame is [0x2E][cmd][len][payload...][checksum]
 *      -- no 0xFA header, no 0xAF terminator. The 0xFA...0xAF format
 *      this tool used to build IS real and correctly reverse-engineered
 *      (makeProtocolPackage()/getProtocolCheckSum() in libMsnCommons.so),
 *      but it's MsnCoreApp's *internal* IPC format between its own
 *      subsystems (CanBus/Setting/FMRadio/McuCenter apps), sent via
 *      sendProtocolToCoreApp() -- never written to the wire. Answering
 *      the MCU's CMD 0x02/0x20 with that format was reaching nowhere.
 *    - The wire checksum is a plain byte SUM, one's-complemented
 *      (`(~sum) & 0xFF`), computed over cmd+len+payload (NOT including
 *      the leading 0x2E signature) -- confirmed from
 *      MCUAdapter_BoxP300::getPackageCheckSum(). Not XOR.
 *    - MsnCoreApp speaks FIRST: onInited() proactively sends
 *      cmd=0x81, payload=[0x01] over the wire immediately at startup,
 *      before ever waiting to receive anything.
 * 3. 2026-07-18 (later): onModeAppChanged(), showApp(), msnAppStateChange()
 *    fully decoded two more real proactive frames beyond the 0x81 hello,
 *    and corrected an earlier misread of msnAppStateChange's payload
 *    (it's [len=2][0x00,0x03], not payload=[2,3] as first thought --
 *    the 2/3 were the len field and a payload byte read together).
 *    Since it's still unknown which of these frames (if any beyond the
 *    hello) actually triggers the MCU to close the CBT16211A touch
 *    switch, this tool now sends all three on startup -- see
 *    send_startup_sequence() below and README.md for the full detail.
 *
 * Confirmed real outgoing wire commands from app->MCU, all implemented
 * in send_startup_sequence(): 0x81 (hello/init, payload=[1]), 0x82
 * (onModeAppChanged mode=4, 9-byte payload), 0x84 (msnAppStateChange
 * bit26/27 state-change, payload=[0x00,0x03]). showApp's own 0x82 case
 * (mode=0xCC, a different 4-byte payload) is a separate, externally-
 * triggered path not reachable from inside libMcuCenter.so and not
 * sent here -- see README.md.
 *
 * Since CMD 0x02 (handshake request) and 0x20 (status query) from the
 * MCU don't get a wire reply in real firmware, this tool no longer
 * sends one either -- it just logs what it receives. If the MCU
 * genuinely expects an ACK we haven't found yet, that's an open item,
 * not something to guess at again.
 *
 * 4. 2026-07-22: found via strace (docs/logs/directfb_strace.txt, see
 *    docs/MCU_ADAPTERS.md's "/dev/ttyS2" section) that MsnCoreApp also
 *    opens a SECOND, separate serial port -- /dev/ttyS2 at 4800 baud --
 *    and writes real frames using the 0xFA...0xAF format this tool used
 *    to build (see point 2 above) before it was corrected: that framing
 *    IS real (makeProtocolPackage()/getProtocolCheckSum() in
 *    libMsnCommons.so), just never sent to ttyHS0 -- it goes out ttyS2
 *    instead. Verified byte-exact against two frames captured live:
 *      FA 00 13 59 02 02 00 B0 AF                            (heartbeat, repeated)
 *      FA 00 FF FF 08 6B BF FD 39 20 A1 86 57 B2 AF           (one-off status)
 *    Structure: [0xFA][arg1][arg2][arg3][len][payload...][chk][0xAF],
 *    chk = plain XOR over bytes[0 .. 4+len) (i.e. FA..last payload byte
 *    inclusive, NOT including chk/0xAF themselves) -- confirmed against
 *    both captures above, byte-exact. What's physically on the other end
 *    of ttyS2 is NOT yet known (not MCUAdapter_BoxP300 -- that's
 *    confirmed 38400 baud on ttyHS0 -- likely a separate peripheral, see
 *    docs/MCU_ADAPTERS.md). --ttys2 mode below replays the two known
 *    frames and listens for more, using this confirmed encode/decode.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <sys/select.h>
#include <time.h>

/* MCUAdapter_BoxP300::getPortSettings() (libMcuCenter.so @ 0x35a8c) --
 * confirmed real baud: 0x00009600 = 38400, 8 data bits, no parity,
 * 1 stop bit. Not a guess, kept as this tool's default. */
#define DEFAULT_BAUD 38400

/* /dev/ttyS2's real, live-confirmed baud (2026-07-22, from strace --
 * see docs/MCU_ADAPTERS.md's "/dev/ttyS2" section and the note above). */
#define TTYS2_DEFAULT_PORT "/dev/ttyS2"
#define TTYS2_BAUD 4800

static const int SCAN_BAUD_CANDIDATES[] = {
    38400, 115200, 9600, 19200, 57600,
#ifdef B230400
    230400,
#endif
#ifdef B460800
    460800,
#endif
#ifdef B921600
    921600,
#endif
};
#define NUM_SCAN_CANDIDATES (int)(sizeof(SCAN_BAUD_CANDIDATES) / sizeof(SCAN_BAUD_CANDIDATES[0]))

/* MCUAdapter_BoxP300::getPackageCheckSum() (0x35cc8): plain byte sum,
 * one's-complemented. Covers cmd+len+payload only -- caller passes the
 * buffer starting at cmd (offset 1 in the full frame), not the 0x2E
 * signature byte. */
unsigned char calc_mcu_checksum(const unsigned char *data, int len) {
    unsigned int sum = 0;
    for (int i = 0; i < len; i++)
        sum += data[i];
    return (unsigned char)(~sum & 0xFF);
}

/* ttyS2's 0xFA...0xAF frame checksum: plain XOR over [0xFA .. last
 * payload byte] inclusive (NOT including the checksum/terminator
 * bytes themselves). Confirmed byte-exact against two frames captured
 * live off the real wire -- see the top-of-file note. Distinct from
 * calc_mcu_checksum(), which is ttyHS0's different one's-complement-
 * sum algorithm. */
unsigned char calc_fa_checksum(const unsigned char *data, int len) {
    unsigned char chk = 0;
    for (int i = 0; i < len; i++)
        chk ^= data[i];
    return chk;
}

speed_t get_baud_rate(int speed) {
    switch (speed) {
        case 9600: return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 115200: return B115200;
#ifdef B230400
        case 230400: return B230400;
#endif
#ifdef B460800
        case 460800: return B460800;
#endif
#ifdef B921600
        case 921600: return B921600;
#endif
        default: return B38400;
    }
}

int set_interface_attribs(int fd, int speed) {
    struct termios tty;
    if (tcgetattr(fd, &tty) < 0) {
        fprintf(stderr, "Error from tcgetattr: %s\n", strerror(errno));
        return -1;
    }

    speed_t baud = get_baud_rate(speed);
    cfsetospeed(&tty, baud);
    cfsetispeed(&tty, baud);

    tty.c_cflag |= (CLOCAL | CREAD);    /* ignore modem controls */
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;         /* 8-bit characters */
    tty.c_cflag &= ~PARENB;     /* no parity bit */
    tty.c_cflag &= ~CSTOPB;     /* only need 1 stop bit */
    tty.c_cflag &= ~CRTSCTS;    /* no hardware flowcontrol */

    /* setup for non-canonical mode */
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tty.c_oflag &= ~OPOST;

    /* fetch bytes immediately */
    tty.c_cc[VMIN] = 1;
    tty.c_cc[VTIME] = 1;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        fprintf(stderr, "Error from tcsetattr: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

/* Builds and sends a real MCU-wire frame: [0x2E][cmd][len][payload...]
 * [checksum]. Matches MCUAdapter_BoxP300::makeMCUProtocol() (0x35ec0)
 * exactly -- no header/terminator bytes beyond the signature. */
void send_mcu_frame(int fd, unsigned char cmd, const unsigned char *payload,
                     int payload_len, int verbose) {
    unsigned char frame[256];
    unsigned char cksum_buf[256];
    int idx = 0;

    frame[idx++] = 0x2E;
    frame[idx++] = cmd;
    frame[idx++] = (unsigned char)payload_len;
    if (payload_len > 0)
        memcpy(&frame[idx], payload, payload_len);

    /* checksum covers cmd+len+payload, i.e. frame[1..idx+payload_len) */
    memcpy(cksum_buf, &frame[1], 2 + payload_len);
    unsigned char chk = calc_mcu_checksum(cksum_buf, 2 + payload_len);
    idx += payload_len;
    frame[idx++] = chk;

    if (write(fd, frame, idx) < 0) {
        fprintf(stderr, "Error writing to serial port: %s\n", strerror(errno));
        return;
    }

    if (verbose) {
        printf("[TX] cmd=%02X (%d bytes):", cmd, idx);
        for (int i = 0; i < idx; i++)
            printf(" %02X", frame[i]);
        printf("\n");
        fflush(stdout);
    }
}

/* Sends the three real, disassembly-confirmed frames MsnCoreApp sends
 * proactively around startup/app-state changes -- not just the 0x81
 * hello. We don't yet know which of these (if any beyond the hello) is
 * what actually makes the MCU close the CBT16211A touch switch, so we
 * send all three: cheap, harmless (they're legitimate frames real
 * firmware sends anyway), and maximizes the chance of triggering it on
 * a single test run instead of needing several manual re-tests.
 *
 * 1. cmd=0x81, payload=[0x01] -- MCUAdapter_BoxP300::onInited(), sent
 *    unconditionally at startup before receiving anything.
 * 2. cmd=0x82, payload=[0x01,0x08,0,0,0,0,0,0,0] (9 bytes) --
 *    onModeAppChanged(mode=4), the only mode reachable from inside
 *    libMcuCenter.so, via msnAppStateChange's bit24/25 path.
 * 3. cmd=0x84, payload=[0x00,0x03] -- msnAppStateChange's bit26/27
 *    "state changed" path.
 * All three traced from MCUAdapter_BoxP300 in libMcuCenter.so,
 * 2026-07-18 -- see tools/mcu-handshake/README.md for full detail. */
void send_startup_sequence(int fd, int verbose) {
    unsigned char hello_payload = 0x01;
    unsigned char mode4_payload[9] = { 0x01, 0x08, 0, 0, 0, 0, 0, 0, 0 };
    unsigned char state_payload[2] = { 0x00, 0x03 };

    if (verbose)
        printf("[*] Sending startup sequence: hello (0x81), mode-4 app-state (0x82), "
               "bit26/27 state-change (0x84)\n");

    send_mcu_frame(fd, 0x81, &hello_payload, 1, verbose);
    usleep(50000);
    send_mcu_frame(fd, 0x82, mode4_payload, 9, verbose);
    usleep(50000);
    send_mcu_frame(fd, 0x84, state_payload, 2, verbose);
}

/* Reads and validates one frame from fd, blocking. Returns 1 with cmd/
 * payload/length filled in on success, 0 on a checksum mismatch (already
 * logged if verbose), -1 on I/O error. Does NOT send any reply -- real
 * MsnCoreApp doesn't reply to CMD 0x02/0x20 on the wire either. */
int read_mcu_frame(int fd, unsigned char *out_cmd, unsigned char *out_payload,
                    unsigned char *out_len, int verbose) {
    unsigned char sig;
    if (read(fd, &sig, 1) <= 0)
        return -1;
    if (sig != 0x2E)
        return 0;

    unsigned char header[2];
    int header_read = 0;
    while (header_read < 2) {
        int n = read(fd, &header[header_read], 2 - header_read);
        if (n > 0) header_read += n;
        else if (n < 0 && errno != EAGAIN && errno != EINTR) break;
    }
    if (header_read < 2)
        return 0;

    unsigned char cmd = header[0];
    unsigned char length = header[1];

    unsigned char remaining[256];
    int req_len = length + 1; /* payload + checksum */
    int rem_read = 0;
    while (rem_read < req_len) {
        int n = read(fd, &remaining[rem_read], req_len - rem_read);
        if (n > 0) rem_read += n;
        else if (n < 0 && errno != EAGAIN && errno != EINTR) break;
    }
    if (rem_read < req_len)
        return 0;

    unsigned char chk_recv = remaining[length];
    unsigned char check_buf[256];
    check_buf[0] = cmd;
    check_buf[1] = length;
    memcpy(&check_buf[2], remaining, length);
    unsigned char chk_calc = calc_mcu_checksum(check_buf, length + 2);

    if (chk_calc != chk_recv) {
        if (verbose)
            printf("[-] Checksum mismatch: calc %02X, recv %02X (cmd=%02X len=%d)\n",
                   chk_calc, chk_recv, cmd, length);
        return 0;
    }

    if (verbose) {
        printf("[RX] cmd=%02X (len=%d):", cmd, length);
        for (int i = 0; i < length + 1; i++)
            printf(" %02X", remaining[i]);
        printf("\n");
        fflush(stdout);
    }

    *out_cmd = cmd;
    *out_len = length;
    memcpy(out_payload, remaining, length);
    return 1;
}

void log_frame(unsigned char cmd, const unsigned char *payload, unsigned char len) {
    if (cmd == 0x02 && len >= 2) {
        printf("[+] CMD 0x02 (handshake request) from MCU: b3=%u b4=%u -- "
               "no wire reply sent (real MsnCoreApp doesn't send one either)\n",
               payload[0], payload[1]);
    } else if (cmd == 0x20 && len >= 5) {
        printf("[+] CMD 0x20 (status query) from MCU: b3=%u b4=%u b5=%u b6=%u b7=%u -- "
               "no wire reply sent\n", payload[0], payload[1], payload[2], payload[3], payload[4]);
    } else {
        printf("[+] CMD 0x%02X from MCU, len=%u\n", cmd, len);
    }
    fflush(stdout);
}

void listen_forever(int fd, int verbose) {
    unsigned char cmd, payload[256], len;
    while (1) {
        int r = read_mcu_frame(fd, &cmd, payload, &len, verbose);
        if (r == 1)
            log_frame(cmd, payload, len);
        /* r==0: resync/checksum-fail, r==-1: I/O hiccup -- both just loop */
    }
}

/* ---- /dev/ttyS2 (0xFA...0xAF format) probing --------------------------
 * Separate protocol from ttyHS0's [0x2E] frames above -- see the
 * top-of-file 2026-07-22 note and docs/MCU_ADAPTERS.md's "/dev/ttyS2"
 * section. Frame: [0xFA][arg1][arg2][arg3][len][payload...][chk][0xAF].
 * We don't know the semantic meaning of arg1/arg2/arg3 or what's on the
 * other end -- this just replays the two frames confirmed live off the
 * wire and logs whatever comes back, the same "probe and observe"
 * approach as --scan above.
 */
void send_fa_frame(int fd, unsigned char arg1, unsigned char arg2, unsigned char arg3,
                    const unsigned char *payload, int payload_len, int verbose) {
    unsigned char frame[256];
    int idx = 0;

    frame[idx++] = 0xFA;
    frame[idx++] = arg1;
    frame[idx++] = arg2;
    frame[idx++] = arg3;
    frame[idx++] = (unsigned char)payload_len;
    if (payload_len > 0) {
        memcpy(&frame[idx], payload, payload_len);
        idx += payload_len;
    }

    unsigned char chk = calc_fa_checksum(frame, idx);
    frame[idx++] = chk;
    frame[idx++] = 0xAF;

    if (write(fd, frame, idx) < 0) {
        fprintf(stderr, "Error writing to serial port: %s\n", strerror(errno));
        return;
    }

    if (verbose) {
        printf("[TX] (%d bytes):", idx);
        for (int i = 0; i < idx; i++)
            printf(" %02X", frame[i]);
        printf("\n");
        fflush(stdout);
    }
}

/* Replays the two frames confirmed live on ttyS2 (see top-of-file note):
 * a repeated short "heartbeat"-looking frame and a longer one-off status
 * frame. Their real trigger/meaning is unknown -- replaying known-good
 * traffic is cheap and won't confuse a real peripheral any more than
 * the app's own normal traffic would. */
void send_ttys2_probe(int fd, int verbose) {
    unsigned char heartbeat_payload[2] = { 0x02, 0x00 };
    unsigned char status_payload[8] = { 0x6B, 0xBF, 0xFD, 0x39, 0x20, 0xA1, 0x86, 0x57 };

    if (verbose)
        printf("[*] Replaying two frames captured live on ttyS2\n");

    send_fa_frame(fd, 0x00, 0x13, 0x59, heartbeat_payload, 2, verbose);
    usleep(50000);
    send_fa_frame(fd, 0x00, 0xFF, 0xFF, status_payload, 8, verbose);
}

/* Reads and validates one 0xFA...0xAF frame, blocking. Returns 1 with
 * arg1/arg2/arg3/payload/length filled in on success, 0 on a checksum
 * mismatch (already logged if verbose), -1 on I/O error. */
int read_fa_frame(int fd, unsigned char *out_arg1, unsigned char *out_arg2,
                   unsigned char *out_arg3, unsigned char *out_payload,
                   unsigned char *out_len, int verbose) {
    unsigned char sig;
    if (read(fd, &sig, 1) <= 0)
        return -1;
    if (sig != 0xFA)
        return 0;

    unsigned char header[4]; /* arg1, arg2, arg3, len */
    int header_read = 0;
    while (header_read < 4) {
        int n = read(fd, &header[header_read], 4 - header_read);
        if (n > 0) header_read += n;
        else if (n < 0 && errno != EAGAIN && errno != EINTR) break;
    }
    if (header_read < 4)
        return 0;

    unsigned char arg1 = header[0], arg2 = header[1], arg3 = header[2];
    unsigned char length = header[3];

    unsigned char remaining[256];
    int req_len = length + 2; /* payload + checksum + terminator */
    int rem_read = 0;
    while (rem_read < req_len) {
        int n = read(fd, &remaining[rem_read], req_len - rem_read);
        if (n > 0) rem_read += n;
        else if (n < 0 && errno != EAGAIN && errno != EINTR) break;
    }
    if (rem_read < req_len)
        return 0;

    unsigned char chk_recv = remaining[length];
    unsigned char terminator = remaining[length + 1];

    unsigned char check_buf[256];
    check_buf[0] = 0xFA;
    check_buf[1] = arg1;
    check_buf[2] = arg2;
    check_buf[3] = arg3;
    check_buf[4] = length;
    memcpy(&check_buf[5], remaining, length);
    unsigned char chk_calc = calc_fa_checksum(check_buf, 5 + length);

    if (terminator != 0xAF || chk_calc != chk_recv) {
        if (verbose)
            printf("[-] ttyS2 frame invalid: term=%02X (want AF), calc %02X, recv %02X\n",
                   terminator, chk_calc, chk_recv);
        return 0;
    }

    if (verbose) {
        printf("[RX] arg1=%02X arg2=%02X arg3=%02X len=%d:", arg1, arg2, arg3, length);
        for (int i = 0; i < length; i++)
            printf(" %02X", remaining[i]);
        printf("\n");
        fflush(stdout);
    }

    *out_arg1 = arg1;
    *out_arg2 = arg2;
    *out_arg3 = arg3;
    *out_len = length;
    memcpy(out_payload, remaining, length);
    return 1;
}

void listen_forever_ttys2(int fd, int verbose) {
    unsigned char arg1, arg2, arg3, payload[256], len;
    while (1) {
        int r = read_fa_frame(fd, &arg1, &arg2, &arg3, payload, &len, verbose);
        if (r == 1) {
            printf("[+] ttyS2 frame: arg1=%02X arg2=%02X arg3=%02X len=%d\n",
                   arg1, arg2, arg3, len);
            fflush(stdout);
        }
        /* r==0: resync/checksum-fail, r==-1: I/O hiccup -- both just loop */
    }
}

struct scan_result {
    int baud;
    int valid_frames;
};

struct scan_result scan_one_baud(const char *port, int baud, int duration_sec, int verbose) {
    struct scan_result res = { baud, 0 };

    int fd = open(port, O_RDWR | O_NOCTTY | O_NDELAY);
    if (fd < 0) {
        fprintf(stderr, "[-] %d baud: failed to open %s: %s\n", baud, port, strerror(errno));
        return res;
    }
    fcntl(fd, F_SETFL, 0);
    if (set_interface_attribs(fd, baud) < 0) {
        close(fd);
        return res;
    }

    printf("[*] Scanning %d baud for %ds...\n", baud, duration_sec);
    fflush(stdout);
    send_startup_sequence(fd, verbose);

    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += duration_sec;

    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double remaining = (deadline.tv_sec - now.tv_sec) + (deadline.tv_nsec - now.tv_nsec) / 1e9;
        if (remaining <= 0)
            break;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv;
        tv.tv_sec = (long)remaining;
        tv.tv_usec = (long)((remaining - tv.tv_sec) * 1e6);

        int sel = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (sel <= 0)
            continue;

        unsigned char cmd, payload[256], len;
        int r = read_mcu_frame(fd, &cmd, payload, &len, verbose);
        if (r == 1) {
            res.valid_frames++;
            log_frame(cmd, payload, len);
        }
    }

    close(fd);
    printf("[*] %d baud: %d valid frame(s)\n", baud, res.valid_frames);
    fflush(stdout);
    return res;
}

void run_scan(const char *port, int duration_sec, int verbose) {
    struct scan_result results[NUM_SCAN_CANDIDATES];

    printf("[*] Scanning %d candidate baud rate(s), %ds each -- sends the proactive "
           "hello frame on each, then listens. Toggle an input (reverse/ACC/a button) "
           "on the vehicle to prompt more MCU traffic.\n", NUM_SCAN_CANDIDATES, duration_sec);
    fflush(stdout);

    for (int i = 0; i < NUM_SCAN_CANDIDATES; i++)
        results[i] = scan_one_baud(port, SCAN_BAUD_CANDIDATES[i], duration_sec, verbose);

    printf("\n[*] Scan summary:\n");
    printf("%10s %14s\n", "baud", "valid_frames");
    int best = -1;
    for (int i = 0; i < NUM_SCAN_CANDIDATES; i++) {
        printf("%10d %14d\n", results[i].baud, results[i].valid_frames);
        if (results[i].valid_frames > 0 && (best == -1 || results[i].valid_frames > results[best].valid_frames))
            best = i;
    }
    if (best >= 0) {
        printf("\n[+] %d baud produced valid frames -- this is the real MCU baud rate. "
               "Run with -b %d for normal mode.\n", results[best].baud, results[best].baud);
    } else {
        printf("\n[-] No valid frames received at any candidate baud. Check that "
               "MsnCoreApp is stopped (killall MsnCoreApp), that the MCU/vehicle is "
               "powered, and toggle an input to prompt traffic. The hello frame (cmd "
               "0x81) was sent on each rate regardless -- if the touch switch closes "
               "even with zero frames received back, the hello alone may be enough.\n");
    }
}

int main(int argc, char **argv) {
    char *port = "/dev/ttyHS0";
    int baud = DEFAULT_BAUD;
    int verbose = 0;
    int scan = 0;
    int scan_duration = 5;
    int no_hello = 0;
    int ttys2_mode = 0;
    int port_given = 0;
    int baud_given = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) {
            if (i + 1 < argc) { port = argv[++i]; port_given = 1; }
        } else if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--baud") == 0) {
            if (i + 1 < argc) { baud = atoi(argv[++i]); baud_given = 1; }
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "--scan") == 0) {
            scan = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                scan_duration = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--no-hello") == 0) {
            no_hello = 1;
        } else if (strcmp(argv[i], "--ttys2") == 0) {
            ttys2_mode = 1;
        } else {
            fprintf(stderr, "Usage: %s [-p port] [-b baud] [-v] [--scan [seconds_per_baud]] "
                             "[--no-hello] [--ttys2]\n", argv[0]);
            return 1;
        }
    }

    if (ttys2_mode) {
        if (!port_given) port = TTYS2_DEFAULT_PORT;
        if (!baud_given) baud = TTYS2_BAUD;

        printf("[*] --ttys2: opening %s at %d baud (separate protocol from ttyHS0, "
               "see docs/MCU_ADAPTERS.md)...\n", port, baud);
        int fd = open(port, O_RDWR | O_NOCTTY | O_NDELAY);
        if (fd < 0) {
            fprintf(stderr, "[-] Failed to open serial port %s: %s\n", port, strerror(errno));
            return 1;
        }
        fcntl(fd, F_SETFL, 0);
        if (set_interface_attribs(fd, baud) < 0) {
            close(fd);
            return 1;
        }

        if (!no_hello)
            send_ttys2_probe(fd, verbose);
        else if (verbose)
            printf("[*] --no-hello given, skipping the two known-frame replay\n");

        printf("[*] Listening for ttyS2 frames. Press Ctrl+C to stop.\n");
        fflush(stdout);
        listen_forever_ttys2(fd, verbose);
        close(fd);
        return 0;
    }

    if (scan) {
        run_scan(port, scan_duration, verbose);
        return 0;
    }

    printf("[*] Opening %s at %d baud...\n", port, baud);
    int fd = open(port, O_RDWR | O_NOCTTY | O_NDELAY);
    if (fd < 0) {
        fprintf(stderr, "[-] Failed to open serial port %s: %s\n", port, strerror(errno));
        return 1;
    }

    fcntl(fd, F_SETFL, 0);
    if (set_interface_attribs(fd, baud) < 0) {
        close(fd);
        return 1;
    }

    if (!no_hello)
        send_startup_sequence(fd, verbose);
    else if (verbose)
        printf("[*] --no-hello given, skipping proactive startup sequence\n");

    printf("[*] Listening for MCU frames. Press Ctrl+C to stop.\n");
    fflush(stdout);

    listen_forever(fd, verbose);

    close(fd);
    return 0;
}
