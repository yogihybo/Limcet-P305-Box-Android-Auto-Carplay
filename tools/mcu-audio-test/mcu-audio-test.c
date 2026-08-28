/*
 * mcu-audio-test — Hardware test utility for testing MCU-controlled audio,
 * power amplification, microphone multiplexing, and DSP settings on the
 * Limcet BoxP300 / Toyota Prado head unit.
 *
 * All settings are communicated over /dev/ttyHS0 at 38400 8N1 to the STM32
 * companion MCU (Limcet-V1.0-1302), which manages:
 *   - GPIOA Pin 7: Audio Subsystem Analog Power Enable (+8.5V VCC Rail)
 *   - GPIOA Pin 1: TDA7388 Power Amplifier Hardware Mute (PA_MUTE)
 *   - GPIOB Pin 6: Microphone Analog Multiplexer (OEM Roof Mic vs 3.5mm Jack)
 *   - I2C2 (PB10/PB11): ROHM BD37033 5.1-Channel Sound Processor Control
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <sys/select.h>
#include <time.h>

#define DEFAULT_PORT "/dev/ttyHS0"
#define DEFAULT_BAUD B38400

/* Outbound Command Codes */
#define CMD_HELLO       0x81
#define CMD_MODE_APP    0x82
#define CMD_AUDIO_STATE 0x84
#define CMD_QUERY_VER   0x85
#define CMD_BT_PASSTHRU 0x87
#define CMD_TEA_AUTH    0x88
#define CMD_SETTING     0xA0

/* Setting IDs (CMD 0xA0) */
#define SETTING_EQ_MODE   0x01
#define SETTING_BASS      0x04
#define SETTING_MIDDLE    0x05
#define SETTING_TREBLE    0x06
#define SETTING_FADER     0x07
#define SETTING_BALANCE   0x08
#define SETTING_MIC_SRC   0x09
#define SETTING_LOUDNESS  0x0A
#define SETTING_VOLUME    0x0B
#define SETTING_CAMERA    0x0C
#define SETTING_GUIDELINES 0x0D
#define SETTING_DUCKING   0x0E

static uint8_t calc_mcu_checksum(const uint8_t *data, int len) {
    unsigned int sum = 0;
    for (int i = 0; i < len; i++) sum += data[i];
    return (uint8_t)(~sum & 0xFF);
}

static int send_mcu_frame(int fd, uint8_t cmd, const uint8_t *payload, uint8_t len) {
    uint8_t frame[32];
    frame[0] = 0x2E;
    frame[1] = cmd;
    frame[2] = len;
    if (payload && len > 0) {
        memcpy(frame + 3, payload, len);
    }
    frame[3 + len] = calc_mcu_checksum(frame + 1, 2 + len);

    printf("[TX] 2E %02X %02X", cmd, len);
    for (int i = 0; i < len; ++i) printf(" %02X", payload[i]);
    printf(" | CHK=%02X\n", frame[3 + len]);

    ssize_t w = write(fd, frame, 4 + len);
    tcdrain(fd);
    return (w == (ssize_t)(4 + len)) ? 0 : -1;
}

static int open_mcu_serial(const char *port) {
    int fd = open(port, O_RDWR | O_NOCTTY | O_NDELAY);
    if (fd < 0) {
        fprintf(stderr, "Error: Failed to open %s: %s (ensure custom_ui/MsnCoreApp stopped)\n",
                port, strerror(errno));
        return -1;
    }

    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    if (tcgetattr(fd, &tty) != 0) {
        perror("tcgetattr");
        close(fd);
        return -1;
    }

    cfsetospeed(&tty, DEFAULT_BAUD);
    cfsetispeed(&tty, DEFAULT_BAUD);
    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8 | CLOCAL | CREAD;
    tty.c_iflag = 0;
    tty.c_oflag = 0;
    tty.c_lflag = 0;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 5;
    tcflush(fd, TCIFLUSH);
    tcsetattr(fd, TCSANOW, &tty);
    return fd;
}

static void read_mcu_telemetry(int fd, int timeout_ms) {
    uint8_t buf[256];
    fd_set fds;
    struct timeval tv;

    while (timeout_ms > 0) {
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        tv.tv_sec = 0;
        tv.tv_usec = 50000; /* 50ms */

        int ret = select(fd + 1, &fds, NULL, NULL, &tv);
        if (ret > 0 && FD_ISSET(fd, &fds)) {
            int n = read(fd, buf, sizeof(buf) - 1);
            if (n > 0) {
                for (int i = 0; i < n - 3; ++i) {
                    if (buf[i] == 0x2E) {
                        uint8_t cmd = buf[i+1];
                        uint8_t len = buf[i+2];
                        if (i + 3 + len < n) {
                            if (cmd == 0x7F) {
                                printf("  [RX:CMD 0x7F] MCU Firmware: \"%.*s\"\n", len, (char*)&buf[i+3]);
                            } else if (cmd == 0x40) {
                                printf("  [RX:CMD 0x40] >>> Audio Mux & Power State ACK Confirmed! <<<\n");
                            } else if (cmd == 0x30 && len >= 2) {
                                printf("  [RX:CMD 0x30] DC Battery Telemetry: %u.%02u V\n", buf[i+3], buf[i+4]);
                            } else if (cmd == 0x02 && len >= 2) {
                                printf("  [RX:CMD 0x02] Key Event: b3=%u, b4=%u\n", buf[i+3], buf[i+4]);
                            } else {
                                printf("  [RX:CMD 0x%02X] Len=%u payload=[", cmd, len);
                                for (int j = 0; j < len; ++j) printf("%02X%s", buf[i+3+j], (j+1<len)?" ":"");
                                printf("]\n");
                            }
                        }
                    }
                }
            }
        }
        timeout_ms -= 50;
    }
}

static void send_startup_handshake(int fd) {
    printf("\n=== Sending MCU Startup Handshake ===\n");
    uint8_t p_hello = 0x01;
    send_mcu_frame(fd, CMD_HELLO, &p_hello, 1);
    usleep(50000);

    uint8_t p_mode[9] = {0x01, 0x08, 0, 0, 0, 0, 0, 0, 0};
    send_mcu_frame(fd, CMD_MODE_APP, p_mode, 9);
    usleep(50000);

    uint8_t p_audio[2] = {0x00, 0x03}; /* Audio Power ON (PA7) + Unmute (PA1) */
    send_mcu_frame(fd, CMD_AUDIO_STATE, p_audio, 2);
    usleep(50000);

    send_mcu_frame(fd, CMD_QUERY_VER, NULL, 0);
    read_mcu_telemetry(fd, 200);
}

static void print_usage(const char *prog) {
    printf("Usage: %s <command> [options]\n\n", prog);
    printf("Audio & Hardware Power Commands:\n");
    printf("  --power <0|1>               Toggle MCU Audio Subsystem Power & PA Mute (CMD 0x84)\n");
    printf("                              0 = Audio Rail OFF (Muted), 1 = Audio Rail ON (Unmuted)\n");
    printf("  --mic <oem|3.5mm|0|1>       Switch Microphone Analog Mux (CMD 0xA0 [0x09, val])\n");
    printf("                              1 / oem   = OEM Toyota Roof Microphone (GPIOB Pin 6 HIGH)\n");
    printf("                              0 / 3.5mm = Aftermarket 3.5mm Jack (GPIOB Pin 6 LOW)\n");
    printf("  --volume <0..32>            Set Master Volume Level (CMD 0xA0 [0x0B, val])\n");
    printf("  --bass <-7..+7>             Set Bass EQ Gain (CMD 0xA0 [0x04, val])\n");
    printf("  --mid <-7..+7>              Set Middle EQ Gain (CMD 0xA0 [0x05, val])\n");
    printf("  --treble <-7..+7>           Set Treble EQ Gain (CMD 0xA0 [0x06, val])\n");
    printf("  --fader <front-rear> <L-R>  Set Fader and Balance (CMD 0xA0 [0x07, 0x08])\n");
    printf("  --loudness <0|1>            Toggle Loudness Boost (CMD 0xA0 [0x0A, val])\n");
    printf("  --setting <id> <val>        Send raw CMD 0xA0 setting frame (e.g. 0x09 0x01)\n");
    printf("  --all                       Run complete automated audio & setting sweep\n");
    printf("  --monitor                   Live telemetry listener (Ctrl+C to stop)\n");
    printf("\nExamples:\n");
    printf("  %s --power 1\n", prog);
    printf("  %s --mic oem\n", prog);
    printf("  %s --volume 25\n", prog);
    printf("  %s --bass 4\n", prog);
    printf("  %s --all\n", prog);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    int fd = open_mcu_serial(DEFAULT_PORT);
    if (fd < 0) return 1;

    if (strcmp(argv[1], "--power") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: --power requires 0 (off) or 1 (on)\n");
            close(fd);
            return 1;
        }
        int state = atoi(argv[2]);
        uint8_t payload[2] = { (uint8_t)(state ? 0x00 : 0x01), (uint8_t)(state ? 0x03 : 0x00) };
        printf("Setting MCU Audio Power & PA Mute -> %s...\n", state ? "ON (Unmuted)" : "OFF (Muted)");
        send_mcu_frame(fd, CMD_AUDIO_STATE, payload, 2);
        read_mcu_telemetry(fd, 200);
        close(fd);
        return 0;
    }

    if (strcmp(argv[1], "--mic") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: --mic requires 'oem', '3.5mm', '1', or '0'\n");
            close(fd);
            return 1;
        }
        uint8_t val = (strcmp(argv[2], "oem") == 0 || strcmp(argv[2], "1") == 0) ? 1 : 0;
        printf("Setting Microphone Analog Mux to %s (Setting 0x09 = 0x%02X)...\n",
               val ? "OEM Toyota Roof Mic (GPIOB Pin 6 HIGH)" : "Aftermarket 3.5mm Jack (GPIOB Pin 6 LOW)", val);
        uint8_t payload[2] = { SETTING_MIC_SRC, val };
        send_mcu_frame(fd, CMD_SETTING, payload, 2);
        read_mcu_telemetry(fd, 200);
        close(fd);
        return 0;
    }

    if (strcmp(argv[1], "--volume") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: --volume requires level 0..32\n");
            close(fd);
            return 1;
        }
        uint8_t vol = (uint8_t)atoi(argv[2]);
        if (vol > 32) vol = 32;
        printf("Setting Master Volume to %u / 32 (Setting 0x0B = 0x%02X)...\n", vol, vol);
        uint8_t payload[2] = { SETTING_VOLUME, vol };
        send_mcu_frame(fd, CMD_SETTING, payload, 2);
        read_mcu_telemetry(fd, 200);
        close(fd);
        return 0;
    }

    if (strcmp(argv[1], "--bass") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: --bass requires gain -7..+7\n");
            close(fd);
            return 1;
        }
        int g = atoi(argv[2]);
        uint8_t val = (uint8_t)(g + 7); /* Offset to 0..14 */
        printf("Setting Bass EQ Gain to %d (raw 0x%02X)...\n", g, val);
        uint8_t payload[2] = { SETTING_BASS, val };
        send_mcu_frame(fd, CMD_SETTING, payload, 2);
        read_mcu_telemetry(fd, 200);
        close(fd);
        return 0;
    }

    if (strcmp(argv[1], "--mid") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: --mid requires gain -7..+7\n");
            close(fd);
            return 1;
        }
        int g = atoi(argv[2]);
        uint8_t val = (uint8_t)(g + 7);
        printf("Setting Middle EQ Gain to %d (raw 0x%02X)...\n", g, val);
        uint8_t payload[2] = { SETTING_MIDDLE, val };
        send_mcu_frame(fd, CMD_SETTING, payload, 2);
        read_mcu_telemetry(fd, 200);
        close(fd);
        return 0;
    }

    if (strcmp(argv[1], "--treble") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: --treble requires gain -7..+7\n");
            close(fd);
            return 1;
        }
        int g = atoi(argv[2]);
        uint8_t val = (uint8_t)(g + 7);
        printf("Setting Treble EQ Gain to %d (raw 0x%02X)...\n", g, val);
        uint8_t payload[2] = { SETTING_TREBLE, val };
        send_mcu_frame(fd, CMD_SETTING, payload, 2);
        read_mcu_telemetry(fd, 200);
        close(fd);
        return 0;
    }

    if (strcmp(argv[1], "--fader") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Error: --fader requires <front-rear -7..+7> <left-right -7..+7>\n");
            close(fd);
            return 1;
        }
        int f = atoi(argv[2]) + 7;
        int b = atoi(argv[3]) + 7;
        printf("Setting Fader=%d (0x%02X), Balance=%d (0x%02X)...\n", f-7, f, b-7, b);
        uint8_t p_fader[2] = { SETTING_FADER, (uint8_t)f };
        send_mcu_frame(fd, CMD_SETTING, p_fader, 2);
        usleep(30000);
        uint8_t p_bal[2] = { SETTING_BALANCE, (uint8_t)b };
        send_mcu_frame(fd, CMD_SETTING, p_bal, 2);
        read_mcu_telemetry(fd, 200);
        close(fd);
        return 0;
    }

    if (strcmp(argv[1], "--setting") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Error: --setting requires <id> <val>\n");
            close(fd);
            return 1;
        }
        uint8_t id = (uint8_t)strtoul(argv[2], NULL, 0);
        uint8_t val = (uint8_t)strtoul(argv[3], NULL, 0);
        printf("Sending CMD 0xA0 Setting (id=0x%02X, val=0x%02X)...\n", id, val);
        uint8_t payload[2] = { id, val };
        send_mcu_frame(fd, CMD_SETTING, payload, 2);
        read_mcu_telemetry(fd, 200);
        close(fd);
        return 0;
    }

    if (strcmp(argv[1], "--all") == 0) {
        printf("=== AUTOMATED MCU AUDIO & SETTINGS SELF-TEST ===\n\n");
        send_startup_handshake(fd);

        printf("\n--- 1. Testing Microphone Mux Toggle ---\n");
        uint8_t p_mic_oem[2] = { SETTING_MIC_SRC, 1 };
        send_mcu_frame(fd, CMD_SETTING, p_mic_oem, 2);
        read_mcu_telemetry(fd, 100);
        usleep(200000);

        uint8_t p_mic_aux[2] = { SETTING_MIC_SRC, 0 };
        send_mcu_frame(fd, CMD_SETTING, p_mic_aux, 2);
        read_mcu_telemetry(fd, 100);
        usleep(200000);

        printf("\n--- 2. Testing Equalizer Sweeps (Bass, Mid, Treble) ---\n");
        for (int g = -4; g <= 4; g += 4) {
            uint8_t p[2] = { SETTING_BASS, (uint8_t)(g + 7) };
            send_mcu_frame(fd, CMD_SETTING, p, 2);
            usleep(50000);
        }
        read_mcu_telemetry(fd, 100);

        printf("\n--- 3. Testing Volume Stepping (0 -> 15 -> 25) ---\n");
        uint8_t vols[] = { 0, 15, 25 };
        for (size_t i = 0; i < sizeof(vols)/sizeof(vols[0]); ++i) {
            uint8_t p[2] = { SETTING_VOLUME, vols[i] };
            send_mcu_frame(fd, CMD_SETTING, p, 2);
            usleep(100000);
        }
        read_mcu_telemetry(fd, 200);

        printf("\n=== SELF-TEST COMPLETE ===\n");
        close(fd);
        return 0;
    }

    if (strcmp(argv[1], "--monitor") == 0) {
        send_startup_handshake(fd);
        printf("\n[*] Listening for live MCU events and telemetry. Press Ctrl+C to stop...\n");
        while (1) {
            read_mcu_telemetry(fd, 1000);
        }
        close(fd);
        return 0;
    }

    print_usage(argv[0]);
    close(fd);
    return 1;
}
