#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>

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

speed_t get_baud_rate(int speed) {
    switch (speed) {
        case 9600: return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 115200: return B115200;
        default: return B115200;
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

int main(int argc, char **argv) {
    char *port = "/dev/ttyHS0";
    int baud = 115200;
    int verbose = 0;
    
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
        } else {
            fprintf(stderr, "Usage: %s [-p port] [-b baud] [-v]\n", argv[0]);
            return 1;
        }
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
