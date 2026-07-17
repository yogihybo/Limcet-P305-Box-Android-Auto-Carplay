#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <sys/select.h>
#include <time.h>

// BoxP300 (McuType=6) Handshake Mapping
// Maps incoming byte[3] (subcommand) to outbound byte[6] (ip)
static unsigned char map_byte3_to_ip(unsigned char b3) {
    switch (b3) {
        case 3: return 9;
        case 4: return 8;
        case 7: return 24;
        case 8: return 21;
        case 9: return 22;
        case 10: return 1;
        case 11: return 2;
        case 12: return 7;
        case 13: return 5;
        case 14: return 13;
        case 15: return 12;
        case 16: return 6;
        default: return 0xFF; // Unmapped
    }
}

unsigned char calc_xor_checksum(const unsigned char *data, int len) {
    unsigned char chk = 0;
    for (int i = 0; i < len; i++) {
        chk ^= data[i];
    }
    return chk;
}

/* MCUAdapter_BoxP300::getPortSettings() (libMcuCenter.so @ 0x35a8c) was
 * disassembled to find this project's own confirmed real baud rate: it
 * copies a static 16-byte PortSettings struct straight out of .rodata
 * (@ 0xb2c70: bytes 00 96 00 00 08 00 00 00 00 00 00 00 00 00 00 00) --
 * 0x00009600 = 38400 baud, 8 data bits, no parity, 1 stop bit. 38400 is
 * the confirmed value, not a guess -- kept as the default here. The
 * wider candidate list below is for --scan, in case a different MCU
 * firmware revision or product variant differs from this trace. */
#define DEFAULT_BAUD 38400

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

void build_and_send_response(int fd, unsigned char arg1, unsigned char arg2, unsigned char arg3, const unsigned char *payload, int payload_len, int verbose) {
    unsigned char frame[256];
    frame[0] = 0xFA;
    frame[1] = arg1;
    frame[2] = arg2;
    frame[3] = arg3;
    
    int idx = 4;
    if (payload_len > 0) {
        frame[idx++] = (unsigned char)payload_len;
        memcpy(&frame[idx], payload, payload_len);
        idx += payload_len;
    }
    
    unsigned char chk = calc_xor_checksum(frame, idx);
    frame[idx++] = chk;
    frame[idx++] = 0xAF;
    
    if (write(fd, frame, idx) < 0) {
        fprintf(stderr, "Error writing to serial port: %s\n", strerror(errno));
        return;
    }
    
    if (verbose) {
        printf("[TX] Response sent (%d bytes):", idx);
        for (int i = 0; i < idx; i++) {
            printf(" %02X", frame[i]);
        }
        printf("\n");
        fflush(stdout);
    }
}

void parse_and_respond(int fd, int verbose) {
    unsigned char sig;
    while (1) {
        // Find start signature 0x2E
        if (read(fd, &sig, 1) <= 0) {
            continue;
        }
        if (sig != 0x2E) {
            continue;
        }
        
        // Read header: [cmd] [length]
        unsigned char header[2];
        int header_read = 0;
        while (header_read < 2) {
            int n = read(fd, &header[header_read], 2 - header_read);
            if (n > 0) {
                header_read += n;
            } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
                break;
            }
        }
        if (header_read < 2) continue;
        
        unsigned char cmd = header[0];
        unsigned char length = header[1];
        
        // Read payload + checksum
        // Total bytes remaining = length + 1 (checksum)
        unsigned char remaining[256];
        int req_len = length + 1;
        int rem_read = 0;
        while (rem_read < req_len) {
            int n = read(fd, &remaining[rem_read], req_len - rem_read);
            if (n > 0) {
                rem_read += n;
            } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
                break;
            }
        }
        if (rem_read < req_len) continue;
        
        unsigned char chk_recv = remaining[length];
        
        // Validate checksum
        unsigned char check_buf[256];
        check_buf[0] = 0x2E;
        check_buf[1] = cmd;
        check_buf[2] = length;
        memcpy(&check_buf[3], remaining, length);
        
        unsigned char chk_calc = calc_xor_checksum(check_buf, length + 3);
        if (chk_calc != chk_recv) {
            if (verbose) {
                printf("[-] Checksum mismatch: calc %02X, recv %02X\n", chk_calc, chk_recv);
                fflush(stdout);
            }
            continue;
        }
        
        if (verbose) {
            printf("[RX] CMD %02X (len=%d): 2E %02X %02X", cmd, length, cmd, length);
            for (int i = 0; i < length + 1; i++) {
                printf(" %02X", remaining[i]);
            }
            printf("\n");
            fflush(stdout);
        }
        
        if (cmd == 0x02) {
            if (length < 2) {
                printf("[-] CMD 0x02 payload too short\n");
                fflush(stdout);
                continue;
            }
            unsigned char b3 = remaining[0];
            unsigned char b4 = remaining[1];
            
            unsigned char r6 = (b4 != 0) ? b4 : 3;
            unsigned char ip = map_byte3_to_ip(b3);
            if (ip == 0xFF) {
                printf("[-] Received unmapped byte[3]=%u in CMD 0x02\n", b3);
                fflush(stdout);
                continue;
            }
            
            unsigned char resp_payload[2];
            resp_payload[0] = r6;
            resp_payload[1] = ip;
            
            build_and_send_response(fd, 0x00, 0x13, 0x21, resp_payload, 2, verbose);
            if (!verbose) {
                printf("[+] Handshake completed (b3=%u -> ip=%u, b4=%u -> r6=%u)\n", b3, ip, b4, r6);
                fflush(stdout);
            }
        } else if (cmd == 0x20) {
            if (length < 5) {
                printf("[-] CMD 0x20 payload too short\n");
                fflush(stdout);
                continue;
            }
            unsigned char b3 = remaining[0];
            unsigned char b4 = remaining[1];
            unsigned char b5 = remaining[2];
            unsigned char b6 = remaining[3];
            unsigned char b7 = remaining[4];
            
            unsigned char resp_payload[5];
            resp_payload[0] = b7;
            resp_payload[1] = b4;
            resp_payload[2] = b3;
            resp_payload[3] = b6;
            resp_payload[4] = b5;
            
            build_and_send_response(fd, 0x00, 0x13, 0x23, resp_payload, 5, verbose);
        }
    }
}

/* Bounded-duration listen at one baud rate, for --scan. Reports raw byte
 * traffic (proof something is reaching the port at all, even if framing
 * is wrong for this baud) and any fully valid, checksum-passing frames
 * (proof this is the right baud) -- responding to any it finds, same as
 * normal mode, so a handshake can complete opportunistically mid-scan. */
struct scan_result {
    int baud;
    long bytes_seen;
    int sync_bytes_seen;   /* raw 0x2E byte count, before framing is checked */
    int valid_frames;
};

struct scan_result scan_one_baud(const char *port, int baud, int duration_sec, int verbose) {
    struct scan_result res = { baud, 0, 0, 0 };

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

    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += duration_sec;

    unsigned char buf[256];
    int buf_len = 0;

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
            continue; /* timeout or interrupted -- loop re-checks deadline */

        unsigned char chunk[64];
        int n = read(fd, chunk, sizeof(chunk));
        if (n <= 0)
            continue;

        res.bytes_seen += n;
        if (verbose) {
            printf("[RX %d] ", baud);
            for (int i = 0; i < n; i++)
                printf("%02X ", chunk[i]);
            printf("\n");
            fflush(stdout);
        }

        for (int i = 0; i < n; i++) {
            unsigned char b = chunk[i];
            if (buf_len == 0) {
                if (b != 0x2E)
                    continue;
                res.sync_bytes_seen++;
            }
            if (buf_len < (int)sizeof(buf))
                buf[buf_len++] = b;

            /* Need at least [sig][cmd][len] before we know the full length */
            if (buf_len < 3)
                continue;
            unsigned char cmd = buf[1];
            unsigned char length = buf[2];
            int frame_len = 3 + length + 1; /* sig+cmd+len + payload + checksum */
            if (frame_len > (int)sizeof(buf) || buf_len < frame_len)
                continue;

            unsigned char calc = calc_xor_checksum(buf, frame_len - 1);
            unsigned char recv = buf[frame_len - 1];
            if (calc == recv) {
                res.valid_frames++;
                printf("[+] %d baud: VALID FRAME cmd=%02X len=%d -- this looks like the right baud rate\n",
                       baud, cmd, length);
                fflush(stdout);
                if (cmd == 0x02 && length >= 2) {
                    unsigned char b3 = buf[3], b4 = buf[4];
                    unsigned char r6 = (b4 != 0) ? b4 : 3;
                    unsigned char ip = map_byte3_to_ip(b3);
                    if (ip != 0xFF) {
                        unsigned char resp[2] = { r6, ip };
                        build_and_send_response(fd, 0x00, 0x13, 0x21, resp, 2, verbose);
                        printf("[+] Handshake response sent at %d baud\n", baud);
                        fflush(stdout);
                    }
                } else if (cmd == 0x20 && length >= 5) {
                    unsigned char resp[5] = { buf[7], buf[4], buf[3], buf[6], buf[5] };
                    build_and_send_response(fd, 0x00, 0x13, 0x23, resp, 5, verbose);
                }
            }
            buf_len = 0; /* frame consumed (valid or not), resync on next 0x2E */
        }
    }

    close(fd);
    printf("[*] %d baud: %ld byte(s), %d sync byte(s), %d valid frame(s)\n",
           baud, res.bytes_seen, res.sync_bytes_seen, res.valid_frames);
    fflush(stdout);
    return res;
}

void run_scan(const char *port, int duration_sec, int verbose) {
    struct scan_result results[NUM_SCAN_CANDIDATES];

    printf("[*] Scanning %d candidate baud rate(s), %ds each -- toggle an input "
           "(reverse/ACC/a button) on the vehicle to prompt MCU traffic.\n",
           NUM_SCAN_CANDIDATES, duration_sec);
    fflush(stdout);

    for (int i = 0; i < NUM_SCAN_CANDIDATES; i++)
        results[i] = scan_one_baud(port, SCAN_BAUD_CANDIDATES[i], duration_sec, verbose);

    printf("\n[*] Scan summary:\n");
    printf("%10s %12s %12s %12s\n", "baud", "bytes", "sync_bytes", "valid_frames");
    int best = -1;
    for (int i = 0; i < NUM_SCAN_CANDIDATES; i++) {
        printf("%10d %12ld %12d %12d\n", results[i].baud, results[i].bytes_seen,
               results[i].sync_bytes_seen, results[i].valid_frames);
        if (results[i].valid_frames > 0 && (best == -1 || results[i].valid_frames > results[best].valid_frames))
            best = i;
    }
    if (best >= 0) {
        printf("\n[+] %d baud produced valid frames -- this is the real MCU baud rate. "
               "Run with -b %d for normal handshake mode.\n",
               results[best].baud, results[best].baud);
    } else {
        int any_bytes = 0;
        for (int i = 0; i < NUM_SCAN_CANDIDATES; i++)
            if (results[i].bytes_seen > 0)
                any_bytes = 1;
        if (any_bytes)
            printf("\n[-] No valid frames at any candidate baud, but raw bytes were seen at "
                   "some rate(s) above -- framing/checksum logic may need review, or try "
                   "-b with an exact non-standard rate.\n");
        else
            printf("\n[-] Zero bytes received at any candidate baud rate. Check wiring, "
                   "that the port isn't held open by MsnCoreApp (killall MsnCoreApp first), "
                   "and that the MCU/vehicle is actually powered.\n");
    }
}

int main(int argc, char **argv) {
    char *port = "/dev/ttyHS0";
    int baud = DEFAULT_BAUD;
    int verbose = 0;
    int scan = 0;
    int scan_duration = 5;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) {
            if (i + 1 < argc) {
                port = argv[++i];
            }
        } else if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--baud") == 0) {
            if (i + 1 < argc) {
                baud = atoi(argv[++i]);
            }
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "--scan") == 0) {
            scan = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                scan_duration = atoi(argv[++i]);
            }
        } else {
            fprintf(stderr, "Usage: %s [-p port] [-b baud] [-v] [--scan [seconds_per_baud]]\n", argv[0]);
            return 1;
        }
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

    // Clear flags and enable blocking reads
    fcntl(fd, F_SETFL, 0);

    if (set_interface_attribs(fd, baud) < 0) {
        close(fd);
        return 1;
    }

    printf("[*] Listening for MCU frames. Press Ctrl+C to stop.\n");
    fflush(stdout);

    parse_and_respond(fd, verbose);

    close(fd);
    return 0;
}
