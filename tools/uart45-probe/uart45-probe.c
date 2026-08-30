/*
 * uart45-probe -- active tester for the real, disassembly-confirmed
 * "0x55 sync byte" handshake protocol found on the STM32's own UART4/
 * UART5 peripherals this session (docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md,
 * "Broad security sweep" section). That protocol is confirmed real from
 * the MCU firmware side but its physical destination was left
 * undetermined -- it isn't the SoC link (USART2/ttyHS0) or the
 * Bluetooth module (USART3, confirmed via its own peripheral base
 * address), so it goes somewhere else entirely.
 *
 * Real candidate: docs/1.9_KERNEL_REFERENCE.md's own DTS-derived
 * hardware map labels the SoC's `&uart3` (`/dev/ttyS2`) as "STM32
 * companion MCU" -- a SEPARATE physical UART link from ttyHS0 (which
 * uses the SoC's `ark-hsuart` peripheral, a different controller
 * family entirely). `/dev/ttyS2` ("MSNEry") is independently confirmed
 * live (real traffic seen) but its peer was never identified
 * (docs/1.3_MCU_ADAPTERS.md, tools/uart-test/). This tool tests the
 * hypothesis that ttyS2 IS the SoC-side end of the STM32's UART4/UART5.
 *
 * Protocol under test (real, from disassembly, not guessed):
 *   sync byte 0x55, then a type byte selecting one of 5 real
 *   sub-messages (0x20, 0x32, 0x50, 0xD3, 0xD6). Types 0x20/0x32 are
 *   OUTBOUND from the MCU's perspective -- i.e. WE send [0x55, 0x20] or
 *   [0x55, 0x32] and the MCU streams a fixed field back, one byte per
 *   response. The flash-resident content of those two fields is known:
 *     0x20 -> 00 00 00 00 FF               (5 bytes, placeholder+sentinel)
 *     0x32 -> "   cD31" 00 93              (9 bytes, a real identifier
 *                                            string -- three spaces,
 *                                            "cD31", NUL, trailer byte)
 *   A response matching (or even resembling) either of these confirms
 *   the port under test really is wired to the STM32's UART4/UART5.
 *
 * Low risk, unlike --reboot-probe: this only sends 2 bytes of a
 * read-only "identify yourself" query with no confirmed write/erase
 * side effect anywhere in the traced handler. Still, stop whatever
 * normally holds the port open first (MsnCoreApp for ttyS2, custom_ui
 * for ttyHS0) the same way every other tool in tools/ requires.
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

#define DEFAULT_PORT "/dev/ttyS2"

static speed_t get_baud_rate(int speed) {
    switch (speed) {
        case 1200: return B1200;
        case 2400: return B2400;
        case 4800: return B4800;
        case 9600: return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 115200: return B115200;
        default: return B115200;
    }
}

static int set_interface_attribs(int fd, int speed) {
    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    if (tcgetattr(fd, &tty) != 0) {
        fprintf(stderr, "tcgetattr: %s\n", strerror(errno));
        return -1;
    }
    speed_t br = get_baud_rate(speed);
    cfsetospeed(&tty, br);
    cfsetispeed(&tty, br);

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_iflag &= ~IGNBRK;
    tty.c_lflag = 0;
    tty.c_oflag = 0;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 1;
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~(PARENB | PARODD);
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CRTSCTS;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        fprintf(stderr, "tcsetattr: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

/* Raw, unfiltered capture -- this protocol has no framing byte to
 * anchor on the way the BoxP300 protocol has 0x2E, so every byte
 * received is real data, printed as-is. Groups a printed line whenever
 * the link idles for >200ms. */
static int raw_listen(int fd, int window_ms) {
    int total = 0;
    unsigned char buf[256];
    int buf_len = 0;
    struct timespec deadline, last_rx, now;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    last_rx = deadline;
    deadline.tv_sec += window_ms / 1000;
    deadline.tv_nsec += (window_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }

    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        double remaining = (deadline.tv_sec - now.tv_sec) + (deadline.tv_nsec - now.tv_nsec) / 1e9;
        if (remaining <= 0)
            break;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv;
        double slice = remaining < 0.2 ? remaining : 0.2;
        tv.tv_sec = (long)slice;
        tv.tv_usec = (long)((slice - tv.tv_sec) * 1e6);

        if (select(fd + 1, &rfds, NULL, NULL, &tv) > 0) {
            unsigned char b;
            if (read(fd, &b, 1) == 1) {
                if (buf_len < (int)sizeof(buf)) buf[buf_len++] = b;
                total++;
                clock_gettime(CLOCK_MONOTONIC, &last_rx);
                continue;
            }
        }

        if (buf_len > 0) {
            clock_gettime(CLOCK_MONOTONIC, &now);
            double idle = (now.tv_sec - last_rx.tv_sec) + (now.tv_nsec - last_rx.tv_nsec) / 1e9;
            if (idle >= 0.2) {
                printf("  [RX]");
                for (int i = 0; i < buf_len; i++) printf(" %02X", buf[i]);
                printf("   (ascii: \"");
                for (int i = 0; i < buf_len; i++)
                    putchar((buf[i] >= 0x20 && buf[i] < 0x7f) ? buf[i] : '.');
                printf("\")\n");
                fflush(stdout);
                buf_len = 0;
            }
        }
    }
    if (buf_len > 0) {
        printf("  [RX]");
        for (int i = 0; i < buf_len; i++) printf(" %02X", buf[i]);
        printf("   (ascii: \"");
        for (int i = 0; i < buf_len; i++)
            putchar((buf[i] >= 0x20 && buf[i] < 0x7f) ? buf[i] : '.');
        printf("\")\n");
        fflush(stdout);
    }
    return total;
}

static void try_type(int fd, unsigned char type, const char *label, int window_ms) {
    unsigned char query[2] = { 0x55, type };
    printf("[*] Sending sync 0x55, type 0x%02X (%s)...\n", type, label);
    if (write(fd, query, 2) != 2) {
        fprintf(stderr, "write failed: %s\n", strerror(errno));
        return;
    }
    fflush(stdout);
    int n = raw_listen(fd, window_ms);
    if (n == 0)
        printf("  (silent)\n");
    printf("\n");
}

int main(int argc, char **argv) {
    const char *port = DEFAULT_PORT;
    int baud = 115200;
    int window_ms = 1500;
    int argi = 1;

    while (argi < argc) {
        if (strcmp(argv[argi], "-p") == 0 && argi + 1 < argc) { port = argv[++argi]; argi++; }
        else if (strcmp(argv[argi], "-b") == 0 && argi + 1 < argc) { baud = atoi(argv[++argi]); argi++; }
        else if (strcmp(argv[argi], "-w") == 0 && argi + 1 < argc) { window_ms = atoi(argv[++argi]); argi++; }
        else {
            fprintf(stderr,
                "Usage: %s [-p port] [-b baud] [-w window_ms]\n\n"
                "Sends the real 'identify' queries (sync 0x55, type 0x20 and\n"
                "0x32) from the STM32 UART4/UART5 protocol this project's own\n"
                "disassembly decoded, and raw-listens for a response after each.\n"
                "Default port /dev/ttyS2 (the ttyS2/\"MSNEry\" hypothesis --\n"
                "see this tool's own header comment); pass -p /dev/ttyHS0 or any\n"
                "other candidate port to test elsewhere.\n\n"
                "Expected response if this IS the right port/protocol:\n"
                "  type 0x20 -> 00 00 00 00 FF (5 bytes)\n"
                "  type 0x32 -> \"   cD31\" 00 93 (9 bytes, a real ID string)\n\n"
                "IMPORTANT: stop whatever normally holds the port open first\n"
                "(MsnCoreApp for ttyS2, custom_ui for ttyHS0).\n",
                argv[0]);
            return 1;
        }
    }

    int fd = open(port, O_RDWR | O_NOCTTY | O_NDELAY);
    if (fd < 0) {
        fprintf(stderr, "[-] Failed to open %s: %s\n", port, strerror(errno));
        return 1;
    }
    fcntl(fd, F_SETFL, 0);
    if (set_interface_attribs(fd, baud) < 0) { close(fd); return 1; }
    printf("[*] %s at %d baud, %dms listen window per query\n\n", port, baud, window_ms);

    try_type(fd, 0x20, "5-byte placeholder field", window_ms);
    try_type(fd, 0x32, "9-byte identifier field, expect \"cD31\"", window_ms);

    close(fd);
    return 0;
}
