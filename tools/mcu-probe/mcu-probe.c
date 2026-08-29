/*
 * mcu-probe -- active testing tool for the companion STM32 MCU wire
 * protocol on /dev/ttyHS0. Reuses tools/mcu-handshake/mcu-handshake.c's
 * already hardware-confirmed frame format/checksum verbatim (real
 * MCUAdapter_BoxP300 protocol: [0x2E][cmd][len][payload...][checksum],
 * checksum = one's-complement of a plain byte sum over cmd+len+payload)
 * -- not re-derived here. mcu-handshake answers "does the handshake
 * work"; this tool answers "what does sending an arbitrary command do."
 *
 * Built for the BD37033 investigation (docs/MCU_FIRMWARE_VERIFIED_
 * FINDINGS.md): real, disassembly-confirmed CMD 0xA0 [settingId, value]
 * "UI settings sync" channel, 18 valid setting IDs (0x00-0x11), traced
 * against real GPIO port/pin targets for several of them. This tool
 * lets any of them be sent directly from the SoC side and the result
 * (an MCU response frame, if any, or just the real-world hardware
 * effect you can see/hear) observed -- no internal board probing
 * needed, everything goes out over the already-accessible UART.
 *
 * IMPORTANT: this project's own custom_ui (hal/mcu_input.cpp) normally
 * holds /dev/ttyHS0 open exclusively. Stop it first (same requirement
 * mcu-handshake's own README already documents for MsnCoreApp):
 *   killall custom_ui   # or however it's supervised in rcS
 *
 * Real commands confirmed to have observable, physical effects
 * (buzzer, backlight, relays, etc. per MCU_FIRMWARE_VERIFIED_FINDINGS.md)
 * -- treat --sweep-cmds especially as a live test with someone watching/
 * listening to the vehicle, not something to run unattended.
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

#define DEFAULT_BAUD 38400
#define DEFAULT_PORT "/dev/ttyHS0"

/* ---- Verbatim from tools/mcu-handshake/mcu-handshake.c ---- */

speed_t get_baud_rate(int speed) {
    switch (speed) {
        case 9600: return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 115200: return B115200;
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
        fprintf(stderr, "Error from tcsetattr: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

/* MCUAdapter_BoxP300::getPackageCheckSum() -- one's-complement of a
 * plain byte sum over cmd+len+payload (not the 0x2E signature byte). */
unsigned char calc_mcu_checksum(const unsigned char *data, int len) {
    unsigned int sum = 0;
    for (int i = 0; i < len; i++)
        sum += data[i];
    return (unsigned char)(~sum & 0xFF);
}

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

    memcpy(cksum_buf, &frame[1], 2 + payload_len);
    unsigned char chk = calc_mcu_checksum(cksum_buf, 2 + payload_len);
    idx += payload_len;
    frame[idx++] = chk;

    if (write(fd, frame, idx) < 0) {
        fprintf(stderr, "Error writing to serial port: %s\n", strerror(errno));
        return;
    }

    printf("[TX] cmd=%02X (%d bytes):", cmd, idx);
    for (int i = 0; i < idx; i++)
        printf(" %02X", frame[i]);
    printf("\n");
    fflush(stdout);
    (void)verbose;
}

int read_mcu_frame(int fd, unsigned char *out_cmd, unsigned char *out_payload,
                    unsigned char *out_len) {
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
        else if (n == 0) return -1;
        else if (errno != EAGAIN && errno != EINTR) break;
    }
    if (header_read < 2)
        return 0;

    unsigned char cmd = header[0];
    unsigned char length = header[1];

    unsigned char remaining[256];
    int req_len = length + 1;
    int rem_read = 0;
    while (rem_read < req_len) {
        int n = read(fd, &remaining[rem_read], req_len - rem_read);
        if (n > 0) rem_read += n;
        else if (n == 0) return -1;
        else if (errno != EAGAIN && errno != EINTR) break;
    }
    if (rem_read < req_len)
        return 0;

    unsigned char chk_recv = remaining[length];
    unsigned char check_buf[256];
    check_buf[0] = cmd;
    check_buf[1] = length;
    memcpy(&check_buf[2], remaining, length);
    unsigned char chk_calc = calc_mcu_checksum(check_buf, length + 2);

    if (chk_calc != chk_recv)
        return 0;

    *out_cmd = cmd;
    *out_len = length;
    memcpy(out_payload, remaining, length);
    return 1;
}

/* Listens for up to window_ms milliseconds, printing any valid frames
 * received. Returns the number of valid frames seen. */
int listen_window(int fd, int window_ms) {
    int count = 0;
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += window_ms / 1000;
    deadline.tv_nsec += (window_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }

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
        int r = read_mcu_frame(fd, &cmd, payload, &len);
        if (r == 1) {
            count++;
            printf("      [RX] cmd=%02X len=%u payload:", cmd, len);
            for (int i = 0; i < len; i++)
                printf(" %02X", payload[i]);
            printf("\n");
            fflush(stdout);
        }
    }
    return count;
}

/* ---- mcu-probe's own commands ---- */

/* Real, disassembly-confirmed CMD 0xA0 setting IDs -- see
 * docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md. 0x01-0x06 and 0x0e share a
 * no-op/unimplemented target, kept in the list anyway since "no
 * observable effect" is itself a useful data point when probing. */
#define NUM_SETTING_IDS 18
static const unsigned char kSettingIds[NUM_SETTING_IDS] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11
};

void cmd_setting(int fd, int id, int value) {
    unsigned char payload[2] = { (unsigned char)id, (unsigned char)value };
    printf("[*] CMD 0xA0 [settingId=0x%02X, value=0x%02X]\n", id, value);
    send_mcu_frame(fd, 0xA0, payload, 2, 1);
    listen_window(fd, 300);
}

/* CMD 0x84 (Audio Route) -- the real, confirmed OEM-bypass relay control,
 * disassembled 2026-08-29 (see docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md's
 * "CMD 0x84 (Audio Route) real handler found" section). Real firmware
 * (0x08008808) masks the payload byte to 4 bits and only acts on two
 * values:
 *   value=0x00: sends "AT+AUDROUTE=1" to the audio/BT module over its own
 *               USART3, then (if this handler's own internal gate is
 *               clear -- which it is by default, unlike the CMD 0xA0
 *               id=0x11 path below) drives the shared GPIOC13/PC2 relay
 *               pair to dispatcher state 0 (Pin13=LOW, Pin2=LOW).
 *   value=0x03: sends "AT+AUDROUTE=2", drives the relay to dispatcher
 *               state 1 (Pin13=LOW, Pin2=HIGH).
 * Values 1/2/4/5 update internal state only, no observable effect.
 * Deliberately NOT labeled "aftermarket"/"OEM" here -- which physical
 * routing each state corresponds to is exactly the open question this
 * command exists to help answer; watch/listen to both the stock and
 * aftermarket displays and audio output, and record what each value does. */
void cmd_audio_route(int fd, int value) {
    int masked = value & 0x0F;
    printf("[*] CMD 0x84 (Audio Route) [value=0x%02X, masked=0x%X]", value, masked);
    if (masked == 0x00) {
        printf(" -- expect \"AT+AUDROUTE=1\" over USART3 + relay dispatcher state 0\n");
    } else if (masked == 0x03) {
        printf(" -- expect \"AT+AUDROUTE=2\" over USART3 + relay dispatcher state 1\n");
    } else if (masked >= 0x06) {
        printf(" -- WARNING: masked value >=6 is out of range, real firmware ignores it entirely\n");
    } else {
        printf(" -- real firmware: state stored only, no observable effect expected\n");
    }
    unsigned char payload[1] = { (unsigned char)value };
    send_mcu_frame(fd, 0x84, payload, 1, 1);
    listen_window(fd, 300);
}

/* CMD 0xA0 id=0x11 -- the OTHER real path to the same GPIOC13/PC2 relay
 * pair (see cmd_audio_route() above for the shared dispatcher). Real
 * firmware only acts if its own internal gate byte == 1 -- a condition
 * whose real trigger is UNCONFIRMED, so this command may have zero
 * observable effect even though the frame itself sends correctly. Kept
 * as a thin, clearly-labeled wrapper around --setting rather than a new
 * code path, since it's the exact same CMD 0xA0 mechanism. */
void cmd_video_relay(int fd, int value) {
    printf("[*] CMD 0xA0 id=0x11 (the other real path to the GPIOC13/PC2 relay) "
           "[value=0x%02X] -- real firmware only acts if an internal gate byte "
           "== 1, a condition NOT confirmed to ever be true in practice. May "
           "have no effect even if this command sends correctly; "
           "cmd_audio_route()/--audio-route is the more reliably-triggered "
           "path to the same relay.\n", value);
    cmd_setting(fd, 0x11, value);
}

void cmd_send_raw(int fd, int cmd, const unsigned char *payload, int len) {
    printf("[*] Raw CMD 0x%02X, %d payload byte(s)\n", cmd, len);
    send_mcu_frame(fd, (unsigned char)cmd, payload, len, 1);
    listen_window(fd, 300);
}

/* Sweeps every known-real CMD 0xA0 setting ID with one test value,
 * pausing between each and listening briefly for a response. This is
 * NOT a hardware-effect detector (most settings have no wire reply at
 * all, per docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md -- they only update
 * an in-RAM struct the MCU polls internally) -- it's a controlled,
 * one-at-a-time way to trigger each one while a person watches/listens
 * to the vehicle for a real-world effect (buzzer, relay click, display
 * change, backlight, or -- the actual goal -- a BD37033 I2C ACK if run
 * back-to-back with bd37033-test). */
void run_sweep_settings(int fd, int value, int pause_ms) {
    printf("[*] Sweeping all %d known CMD 0xA0 setting IDs with value=0x%02X, "
           "%dms pause between each. Watch/listen to the vehicle -- Ctrl+C to "
           "stop early if anything unexpected happens.\n\n",
           NUM_SETTING_IDS, value, pause_ms);
    for (int i = 0; i < NUM_SETTING_IDS; i++) {
        cmd_setting(fd, kSettingIds[i], value);
        usleep(pause_ms * 1000);
        printf("\n");
    }
    printf("[*] Sweep complete. If BD37033 was the target, re-run "
           "bd37033-test --pinforce-verify now to check for an ACK.\n");
}

/* Sweeps raw CMD byte values (payload-less) across a range. Broader
 * and less targeted than run_sweep_settings -- real commands beyond
 * the already-catalogued set (0x01-0x05, 0x12, 0x20, 0x81, 0x82, 0x84,
 * 0xA0) are genuinely unknown territory. Requires explicit --yes-i-am-
 * sure since an arbitrary cmd byte could plausibly map to something
 * with a real, unexpected physical effect (see MCU_FIRMWARE_VERIFIED_
 * FINDINGS.md's buzzer/relay findings for why this isn't hypothetical). */
void run_sweep_cmds(int fd, int start, int end, int pause_ms) {
    printf("[*] Sweeping raw CMD bytes 0x%02X..0x%02X (empty payload), %dms "
           "pause between each. This covers UNKNOWN command bytes, not just "
           "the known CMD 0xA0 settings -- watch/listen closely, Ctrl+C to "
           "stop immediately if anything unexpected happens.\n\n",
           start, end, pause_ms);
    for (int c = start; c <= end; c++) {
        /* CMD 0xE1 = confirmed real "reboot to bootloader" (writes magic
         * 0x5555AAAA to SRAM 0x20004004, resets into firmware-update mode
         * -- see docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md). A blind range
         * sweep must never include it -- send it deliberately via
         * --send 0xe1 if that's genuinely what's being tested, with a
         * real recovery plan (USB YMODEM re-flash) in hand first. */
        if (c == 0xE1) {
            printf("[*] Skipping 0xE1 (confirmed real reboot-to-bootloader command -- "
                   "use --send 0xe1 deliberately if you really mean to test this)\n\n");
            continue;
        }
        cmd_send_raw(fd, c, NULL, 0);
        usleep(pause_ms * 1000);
        printf("\n");
    }
    printf("[*] Sweep complete.\n");
}

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [-p port] [-b baud] <command>\n\n"
        "Commands:\n"
        "  --setting <id_hex> <value_hex>   Send CMD 0xA0 [id, value] once\n"
        "  --send <cmd_hex> [byte_hex...]   Send an arbitrary raw frame once\n"
        "  --audio-route <value_hex>        Send CMD 0x84 (OEM-bypass relay control,\n"
        "                                   real: value 0x00/0x03 = the two confirmed\n"
        "                                   AT+AUDROUTE states, others no-op)\n"
        "  --video-relay <value_hex>        Send CMD 0xA0 id=0x11 (the other real path\n"
        "                                   to the same relay -- may have no effect,\n"
        "                                   its own gate condition is unconfirmed)\n"
        "  --sweep-settings [value_hex] [pause_ms]\n"
        "                                   Send CMD 0xA0 for all 18 known setting\n"
        "                                   IDs (0x00-0x11) with the given value\n"
        "                                   (default 0x01), one at a time\n"
        "  --sweep-cmds <start_hex> <end_hex> --yes-i-am-sure [pause_ms]\n"
        "                                   Send raw, empty-payload frames across a\n"
        "                                   CMD byte range -- unknown-command probing,\n"
        "                                   requires the confirmation flag\n"
        "  --listen [seconds]               Just listen for MCU frames (default 10s)\n\n"
        "Examples:\n"
        "  %s --setting 0x0b 0x00           # test the id=0x0b 3-pin-enable candidate\n"
        "  %s --setting 0x00 0x01           # test the id=0x00 (GPIOB Pin1) candidate\n"
        "  %s --sweep-settings 0x01 500\n"
        "  %s --send 0x81 0x01              # replay the known hello frame\n"
        "  %s --sweep-cmds 0x00 0x1f --yes-i-am-sure 400\n"
        "  %s --audio-route 0x00            # relay dispatcher state 0 (real, reliable)\n"
        "  %s --audio-route 0x03            # relay dispatcher state 1 (real, reliable)\n"
        "  %s --video-relay 0x01            # same relay via id=0x11 (gate unconfirmed)\n\n"
        "IMPORTANT: stop custom_ui first (it holds /dev/ttyHS0 exclusively).\n",
        prog, prog, prog, prog, prog, prog, prog, prog, prog);
}

int main(int argc, char **argv) {
    const char *port = DEFAULT_PORT;
    int baud = DEFAULT_BAUD;
    int argi = 1;

    while (argi < argc && (strcmp(argv[argi], "-p") == 0 || strcmp(argv[argi], "-b") == 0)) {
        if (strcmp(argv[argi], "-p") == 0 && argi + 1 < argc) {
            port = argv[argi + 1];
            argi += 2;
        } else if (strcmp(argv[argi], "-b") == 0 && argi + 1 < argc) {
            baud = atoi(argv[argi + 1]);
            argi += 2;
        } else {
            break;
        }
    }

    if (argi >= argc) {
        print_usage(argv[0]);
        return 1;
    }

    int fd = open(port, O_RDWR | O_NOCTTY | O_NDELAY);
    if (fd < 0) {
        fprintf(stderr, "[-] Failed to open %s: %s (is custom_ui still running and "
                         "holding the port?)\n", port, strerror(errno));
        return 1;
    }
    fcntl(fd, F_SETFL, 0);
    if (set_interface_attribs(fd, baud) < 0) {
        close(fd);
        return 1;
    }
    printf("[*] %s at %d baud\n", port, baud);

    const char *command = argv[argi++];

    if (strcmp(command, "--setting") == 0) {
        if (argi + 1 >= argc) { print_usage(argv[0]); close(fd); return 1; }
        int id = (int)strtoul(argv[argi], NULL, 0);
        int value = (int)strtoul(argv[argi + 1], NULL, 0);
        cmd_setting(fd, id, value);

    } else if (strcmp(command, "--audio-route") == 0) {
        if (argi >= argc) { print_usage(argv[0]); close(fd); return 1; }
        int value = (int)strtoul(argv[argi], NULL, 0);
        cmd_audio_route(fd, value);

    } else if (strcmp(command, "--video-relay") == 0) {
        if (argi >= argc) { print_usage(argv[0]); close(fd); return 1; }
        int value = (int)strtoul(argv[argi], NULL, 0);
        cmd_video_relay(fd, value);

    } else if (strcmp(command, "--send") == 0) {
        if (argi >= argc) { print_usage(argv[0]); close(fd); return 1; }
        int cmd = (int)strtoul(argv[argi++], NULL, 0);
        unsigned char payload[256];
        int len = 0;
        while (argi < argc && len < (int)sizeof(payload))
            payload[len++] = (unsigned char)strtoul(argv[argi++], NULL, 0);
        cmd_send_raw(fd, cmd, payload, len);

    } else if (strcmp(command, "--sweep-settings") == 0) {
        int value = 0x01, pause_ms = 500;
        if (argi < argc) value = (int)strtoul(argv[argi++], NULL, 0);
        if (argi < argc) pause_ms = atoi(argv[argi++]);
        run_sweep_settings(fd, value, pause_ms);

    } else if (strcmp(command, "--sweep-cmds") == 0) {
        if (argi + 1 >= argc) { print_usage(argv[0]); close(fd); return 1; }
        int start = (int)strtoul(argv[argi++], NULL, 0);
        int end = (int)strtoul(argv[argi++], NULL, 0);
        int confirmed = 0, pause_ms = 500;
        while (argi < argc) {
            if (strcmp(argv[argi], "--yes-i-am-sure") == 0) { confirmed = 1; argi++; }
            else { pause_ms = atoi(argv[argi]); argi++; }
        }
        if (!confirmed) {
            fprintf(stderr, "[-] --sweep-cmds requires --yes-i-am-sure (this probes "
                             "unknown command bytes, real physical effects possible -- "
                             "see the tool's own header comment)\n");
            close(fd);
            return 1;
        }
        run_sweep_cmds(fd, start, end, pause_ms);

    } else if (strcmp(command, "--listen") == 0) {
        int secs = 10;
        if (argi < argc) secs = atoi(argv[argi++]);
        printf("[*] Listening for %ds...\n", secs);
        listen_window(fd, secs * 1000);

    } else {
        print_usage(argv[0]);
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}
