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
 * (docs/historical/1.3_MCU_ADAPTERS.md, tools/uart-test/). This tool tests the
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

/* Wait up to timeout_ms for exactly one byte. Returns 1 and fills *out
 * on success, 0 on timeout, -1 on read error. */
static int read_one_byte(int fd, unsigned char *out, int timeout_ms) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int sel = select(fd + 1, &rfds, NULL, NULL, &tv);
    if (sel <= 0)
        return 0;
    int n = read(fd, out, 1);
    if (n == 1) return 1;
    return -1;
}

/* Real protocol mechanic, traced from the MCU's own ISR (0x8007780,
 * the state==0x20/0x32 handlers at 0x800788c/0x80078ba): field byte 0
 * is sent back immediately on receiving the type byte itself, but
 * every SUBSEQUENT byte of the field only gets sent after the querier
 * sends one more (arbitrary-value) byte -- a clocked, one-in/one-out
 * exchange, not a burst reply. A plain "send query, then just listen"
 * (this function's own first implementation) would only ever capture
 * field[0] and then see nothing, since nothing pumps the rest out.
 * This "pumps" the remaining field_len-1 bytes explicitly. */
static void try_type(int fd, unsigned char type, const char *label, int field_len, int byte_timeout_ms) {
    unsigned char query[2] = { 0x55, type };
    printf("[*] Sending sync 0x55, type 0x%02X (%s, expect %d bytes back)...\n",
           type, label, field_len);
    if (write(fd, query, 2) != 2) {
        fprintf(stderr, "write failed: %s\n", strerror(errno));
        return;
    }
    fflush(stdout);

    unsigned char resp[64];
    int got = 0;
    while (got < field_len && got < (int)sizeof(resp)) {
        unsigned char b;
        int r = read_one_byte(fd, &b, byte_timeout_ms);
        if (r != 1) {
            printf("  (timed out after %d of %d expected bytes)\n", got, field_len);
            break;
        }
        resp[got++] = b;
        if (got < field_len) {
            unsigned char pump = 0x00;
            if (write(fd, &pump, 1) != 1) {
                fprintf(stderr, "write (pump byte) failed: %s\n", strerror(errno));
                break;
            }
        }
    }

    if (got == 0) {
        printf("  (silent -- no response to the query byte at all)\n");
    } else {
        printf("  [RX %d/%d bytes]", got, field_len);
        for (int i = 0; i < got; i++) printf(" %02X", resp[i]);
        printf("   (ascii: \"");
        for (int i = 0; i < got; i++)
            putchar((resp[i] >= 0x20 && resp[i] < 0x7f) ? resp[i] : '.');
        printf("\")\n");
        if (got == field_len)
            printf("  -- got the FULL expected field. Strong confirmation this port speaks the real protocol.\n");
    }
    printf("\n");
}

int main(int argc, char **argv) {
    const char *port = DEFAULT_PORT;
    int baud = 115200;
    int byte_timeout_ms = 500;
    int argi = 1;

    while (argi < argc) {
        if (strcmp(argv[argi], "-p") == 0 && argi + 1 < argc) { port = argv[++argi]; argi++; }
        else if (strcmp(argv[argi], "-b") == 0 && argi + 1 < argc) { baud = atoi(argv[++argi]); argi++; }
        else if (strcmp(argv[argi], "-w") == 0 && argi + 1 < argc) { byte_timeout_ms = atoi(argv[++argi]); argi++; }
        else {
            fprintf(stderr,
                "Usage: %s [-p port] [-b baud] [-w byte_timeout_ms]\n\n"
                "Sends the real 'identify' queries (sync 0x55, type 0x20 and\n"
                "0x32) from the STM32 UART4/UART5 protocol this project's own\n"
                "disassembly decoded. Real protocol mechanic, traced from the\n"
                "MCU's own ISR: field byte 0 comes back immediately, but every\n"
                "later byte only arrives after this tool sends one more\n"
                "(arbitrary) byte to 'pump' it out -- a clocked, one-in/one-out\n"
                "exchange, not a burst reply. This tool drives that exchange\n"
                "properly rather than just listening once.\n\n"
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
    printf("[*] %s at %d baud, %dms timeout per byte\n\n", port, baud, byte_timeout_ms);

    try_type(fd, 0x20, "5-byte placeholder field", 5, byte_timeout_ms);
    try_type(fd, 0x32, "9-byte identifier field, expect \"cD31\"", 9, byte_timeout_ms);

    close(fd);
    return 0;
}
