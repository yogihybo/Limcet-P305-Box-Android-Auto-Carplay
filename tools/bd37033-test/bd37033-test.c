/*
 * bd37033-test — Hardware diagnostic, probing, and testing tool for the
 * Rohm BD37033FV 5.1-channel digital sound processor on the Limcet BoxP300 board.
 *
 * Capabilities:
 *  1. GPIO 34 standby/power control (export, set dir out, drive 0 / 1).
 *  2. Multi-bus I2C scan for BD37033 slave addresses (0x40 / 0x41) across GPIO states.
 *  3. Power-on burst initialization (20 bytes 0x01-0x14 matching stock bd37033_init()).
 *  4. Volume control (0-32) using the reverse-engineered libMsnSound curve.
 *  5. Mute toggle (reg 0x06 bit 7).
 *  6. Raw I2C sub-address read/write for interactive hardware debugging.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#define BD37033_ADDR_GND 0x40
#define BD37033_ADDR_VCC 0x41
#define BD37033_GPIO_PIN 34

/* Stock power-on register defaults (sub-address 0x01 .. 0x14, 20 bytes) */
static const uint8_t s_bd37033_init_data[20] = {
    0xA5, 0x03, 0x00, 0x00, 0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0x00, 0x00, 0x00, 0x80, 0x80, 0x80, 0x00, 0x82
};

static int gpio_write(const char *path, const char *val) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    ssize_t written = write(fd, val, strlen(val));
    close(fd);
    return (written > 0) ? 0 : -1;
}

static int gpio_read(const char *path, char *buf, size_t max_len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t rd = read(fd, buf, max_len - 1);
    close(fd);
    if (rd >= 0) {
        buf[rd] = '\0';
        while (rd > 0 && (buf[rd - 1] == '\n' || buf[rd - 1] == '\r')) {
            buf[--rd] = '\0';
        }
        return 0;
    }
    return -1;
}

static int set_gpio34_state(int state) {
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", BD37033_GPIO_PIN);

    /* Export if not already present */
    if (access(path, F_OK) != 0) {
        char pin_str[16];
        snprintf(pin_str, sizeof(pin_str), "%d", BD37033_GPIO_PIN);
        gpio_write("/sys/class/gpio/export", pin_str);
        usleep(50000); /* 50ms settling */
    }

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", BD37033_GPIO_PIN);
    gpio_write(path, "out");

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", BD37033_GPIO_PIN);
    return gpio_write(path, state ? "1" : "0");
}

static void print_gpio34_status(void) {
    char path[128];
    char val[32] = {0};
    char dir[32] = {0};

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", BD37033_GPIO_PIN);
    bool dir_ok = (gpio_read(path, dir, sizeof(dir)) == 0);

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", BD37033_GPIO_PIN);
    bool val_ok = (gpio_read(path, val, sizeof(val)) == 0);

    printf("[GPIO 34 Status]: %s (dir='%s', val='%s')\n",
           (dir_ok && val_ok) ? "Exported" : "Not Exported / Error",
           dir_ok ? dir : "N/A", val_ok ? val : "N/A");
}

static int i2c_write_bytes(const char *bus_path, uint8_t dev_addr, uint8_t sub_addr, const uint8_t *data, size_t len) {
    int fd = open(bus_path, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "[I2C] Failed to open %s: %s\n", bus_path, strerror(errno));
        return -1;
    }

    if (ioctl(fd, I2C_SLAVE, dev_addr) < 0) {
        fprintf(stderr, "[I2C] Failed to set slave addr 0x%02X on %s: %s\n", dev_addr, bus_path, strerror(errno));
        close(fd);
        return -1;
    }

    uint8_t buf[64];
    if (len + 1 > sizeof(buf)) {
        close(fd);
        return -1;
    }

    buf[0] = sub_addr;
    if (data && len > 0) {
        memcpy(buf + 1, data, len);
    }

    ssize_t written = write(fd, buf, len + 1);
    close(fd);
    return (written == (ssize_t)(len + 1)) ? 0 : -1;
}

static bool i2c_probe_addr(const char *bus_path, uint8_t dev_addr) {
    int fd = open(bus_path, O_RDWR);
    if (fd < 0) return false;

    if (ioctl(fd, I2C_SLAVE, dev_addr) < 0) {
        close(fd);
        return false;
    }

    /* Attempt a 1-byte read or a dummy sub-address write */
    uint8_t dummy = 0;
    bool ack = false;
    if (read(fd, &dummy, 1) == 1) {
        ack = true;
    } else {
        /* Some audio DSPs only respond to write cycles */
        uint8_t probe_byte = 0x01;
        if (write(fd, &probe_byte, 1) == 1) {
            ack = true;
        }
    }
    close(fd);
    return ack;
}

static void udelay_5(void) { usleep(5); }

static void gexport(int pin) {
    int fd = open("/sys/class/gpio/export", O_WRONLY);
    if (fd >= 0) {
        char b[16];
        int n = snprintf(b, sizeof(b), "%d", pin);
        write(fd, b, n);
        close(fd);
    }
}
static void gunexport(int pin) {
    int fd = open("/sys/class/gpio/unexport", O_WRONLY);
    if (fd >= 0) {
        char b[16];
        int n = snprintf(b, sizeof(b), "%d", pin);
        write(fd, b, n);
        close(fd);
    }
}
static void gdir(int pin, const char *d) {
    char p[64];
    snprintf(p, sizeof(p), "/sys/class/gpio/gpio%d/direction", pin);
    int fd = open(p, O_WRONLY);
    if (fd >= 0) {
        write(fd, d, strlen(d));
        close(fd);
    }
}
static int gopen(int pin) {
    char p[64];
    snprintf(p, sizeof(p), "/sys/class/gpio/gpio%d/value", pin);
    return open(p, O_RDWR);
}
static void gset(int fd, int v) { pwrite(fd, v ? "1" : "0", 1, 0); }
static int gget(int fd) {
    char c = ' ';
    pread(fd, &c, 1, 0);
    return c == '1';
}

static bool bitbang_probe_raw(int sda_pin, int scl_pin, uint8_t addr) {
    gexport(sda_pin);
    gexport(scl_pin);

    /* 1. Pull-up Verification: Both lines MUST be pulled HIGH when released */
    gdir(sda_pin, "in");
    gdir(scl_pin, "in");
    int sda_fd = gopen(sda_pin);
    int scl_fd = gopen(scl_pin);
    if (sda_fd < 0 || scl_fd < 0) {
        if (sda_fd >= 0) close(sda_fd);
        if (scl_fd >= 0) close(scl_fd);
        gunexport(sda_pin);
        gunexport(scl_pin);
        return false;
    }

    udelay_5();
    int sda_idle = gget(sda_fd);
    int scl_idle = gget(scl_fd);
    if (!sda_idle || !scl_idle) {
        /* Line is grounded, floating low, or lacks I2C pull-up resistors */
        close(sda_fd);
        close(scl_fd);
        gunexport(sda_pin);
        gunexport(scl_pin);
        return false;
    }

    /* 2. Configure for open-drain / output driving */
    gdir(scl_pin, "out");
    gdir(sda_pin, "out");
    gset(scl_fd, 1);
    gset(sda_fd, 1);
    udelay_5();

    /* START condition: SDA goes low while SCL is high */
    gset(sda_fd, 0); udelay_5();
    gset(scl_fd, 0); udelay_5();

    /* Write 8 bits: (addr << 1) | 0 (WRITE) */
    uint8_t byte = (addr << 1) | 0;
    for (int i = 7; i >= 0; i--) {
        gset(sda_fd, (byte & (1 << i)) ? 1 : 0);
        udelay_5();
        gset(scl_fd, 1); udelay_5();
        gset(scl_fd, 0); udelay_5();
    }

    /* 9th clock cycle: Release SDA to read ACK */
    gdir(sda_pin, "in");
    udelay_5();
    gset(scl_fd, 1); udelay_5();
    bool ack = !gget(sda_fd);
    gset(scl_fd, 0); udelay_5();
    gdir(sda_pin, "out");

    /* STOP condition: SDA goes high while SCL is high */
    gset(sda_fd, 0); udelay_5();
    gset(scl_fd, 1); udelay_5();
    gset(sda_fd, 1); udelay_5();

    close(sda_fd);
    close(scl_fd);
    gdir(sda_pin, "in");
    gdir(scl_pin, "in");
    gunexport(sda_pin);
    gunexport(scl_pin);
    return ack;
}

/* Strict probe: validates pull-ups, target address ACK, and bogus address NACK */
static bool bitbang_probe_addr(int sda_pin, int scl_pin, uint8_t addr) {
    if (!bitbang_probe_raw(sda_pin, scl_pin, addr)) {
        return false;
    }
    /* Anti-False-Positive Check: Bogus address 0x7B MUST return NACK */
    if (bitbang_probe_raw(sda_pin, scl_pin, 0x7B)) {
        /* Both target address and 0x7B ACKed -> line is stuck low */
        return false;
    }
    return true;
}

static bool is_gpio_forbidden(int pin) {
    if (pin >= 0 && pin <= 29) return true;   /* LCD RGB888 display data & timing */
    if (pin >= 39 && pin <= 52) return true;  /* NAND Flash */
    if (pin == 62 || pin == 63) return true;  /* Console UART0 */
    if (pin == 64 || pin == 65) return true;  /* UART1 */
    if (pin == 66 || pin == 67) return true;  /* MCU UART2 (/dev/ttyHS0) */
    if (pin == 70 || pin == 71) return true;  /* HW I2C0 DesignWare */
    if (pin >= 79 && pin <= 81) return true;  /* PWM1-3 / LCD Backlight Power */
    if (pin >= 82 && pin <= 84) return true;  /* I2S1 Audio DAC */
    if (pin == 95) return true;               /* Apple MFi Auth Reset */
    if (pin >= 97 && pin <= 99) return true;  /* SPI */
    if (pin == 119 || pin == 120) return true;/* UART4 */
    if (pin == 123 || pin == 124) return true;/* UART5 */
    if (pin == 127) return true;              /* I2S1 MCLK */
    if (pin == 130) return true;              /* I2S1 Microphone Capture */
    return false;
}

static const int s_safe_gpios[] = {
    30, 31, 32, 33, 34, 35, 36, 37, 38,
    53, 54, 55, 56, 57, 58, 59, 60, 61,
    72, 73, 74, 75, 76, 77, 78,
    85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 96,
    100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110,
    111, 112, 113, 114, 115, 116, 117, 118, 121, 122, 125, 126,
    128, 129, 131, 132, 133, 134, 135, 136
};

static const int s_power_enable_pins[] = {
    34, /* Stock BD37033 standby / reset */
    35, /* CarA300 peripheral enable */
    37, /* ZhongHang / BoxP700 enable */
    96, /* BoxP700 secondary rail */
    102 /* Bagoo audio/power enable */
};

static void set_power_rails(int state) {
    for (size_t i = 0; i < sizeof(s_power_enable_pins)/sizeof(s_power_enable_pins[0]); ++i) {
        int pin = s_power_enable_pins[i];
        if (is_gpio_forbidden(pin)) continue;
        char path[128];
        snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pin);
        if (access(path, F_OK) != 0) {
            char pstr[16];
            snprintf(pstr, sizeof(pstr), "%d", pin);
            gpio_write("/sys/class/gpio/export", pstr);
            usleep(10000);
        }
        snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", pin);
        gpio_write(path, "out");
        snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pin);
        gpio_write(path, state ? "1" : "0");
    }
}

static void do_scan(void) {
    const char *buses[] = {"/dev/i2c-0", "/dev/i2c-1", "/dev/i2c-2", "/dev/i2c-3"};
    const uint8_t addrs[] = {BD37033_ADDR_GND, BD37033_ADDR_VCC};

    printf("=== Scanning for Rohm BD37033 across I2C buses and GPIO 34 states ===\n\n");

    for (int gpio_state = 0; gpio_state <= 1; ++gpio_state) {
        printf("--- Testing with GPIO 34 = %d ---\n", gpio_state);
        set_gpio34_state(gpio_state);
        usleep(20000); /* 20ms */

        for (size_t b = 0; b < sizeof(buses) / sizeof(buses[0]); ++b) {
            if (access(buses[b], F_OK) != 0) continue;

            printf("  Kernel Bus %s:\n", buses[b]);
            for (size_t a = 0; a < sizeof(addrs) / sizeof(addrs[0]); ++a) {
                uint8_t addr = addrs[a];
                bool found = i2c_probe_addr(buses[b], addr);
                printf("    Address 0x%02X (8-bit 0x%02X): %s\n",
                       addr, addr << 1, found ? ">>> ACK RECEIVED (CHIP RESPONDED!) <<<" : "No ACK");
            }
        }

        /* Direct bit-bang test on stock audio I2C pins: SDA=GPIO9, SCL=GPIO121 */
        printf("  Direct Bit-Bang GPIO SDA=9 SCL=121 (Stock BD37033 physical pins):\n");
        for (size_t a = 0; a < sizeof(addrs) / sizeof(addrs[0]); ++a) {
            uint8_t addr = addrs[a];
            bool found = bitbang_probe_addr(9, 121, addr);
            printf("    Address 0x%02X (8-bit 0x%02X): %s\n",
                   addr, addr << 1, found ? ">>> ACK RECEIVED (CHIP RESPONDED!) <<<" : "No ACK");
        }

        printf("\n");
    }
}

static void do_sweep(void) {
    const uint8_t addrs[] = {BD37033_ADDR_GND, BD37033_ADDR_VCC};
    size_t num_safe = sizeof(s_safe_gpios) / sizeof(s_safe_gpios[0]);
    int acks_found = 0;

    printf("=== SAFE GPIO MATRIX SWEEP FOR ROHM BD37033 ===\n");
    printf("Protected Devices: LCD (0-29), NAND (39-52), UARTs (62-67, 119-124),\n");
    printf("                   I2C0 (70-71), PWM/Backlight (79-81), Audio DAC/ADC (82-84, 127, 130), MFi (95)\n");
    printf("Testing %zu safe spare GPIOs across power rail enable states...\n\n", num_safe);

    for (int pwr = 0; pwr <= 1; ++pwr) {
        printf(">>> Power Rails Enable State: %s (GPIO 34, 35, 37, 96, 102 = %d) <<<\n",
               pwr ? "ASSERTED / HIGH" : "DEASSERTED / LOW", pwr);
        set_power_rails(pwr);
        usleep(50000); /* 50ms rail settling */

        for (size_t i = 0; i < num_safe; ++i) {
            int sda = s_safe_gpios[i];
            if (is_gpio_forbidden(sda)) continue;

            for (size_t j = 0; j < num_safe; ++j) {
                int scl = s_safe_gpios[j];
                if (scl == sda || is_gpio_forbidden(scl)) continue;

                for (size_t a = 0; a < sizeof(addrs) / sizeof(addrs[0]); ++a) {
                    uint8_t addr = addrs[a];
                    bool ack = bitbang_probe_addr(sda, scl, addr);
                    if (ack) {
                        printf("\n**************************************************************\n");
                        printf(">>> SUCCESS: ACK RECEIVED FROM BD37033! <<<\n");
                        printf(">>> SDA Pin = GPIO %d, SCL Pin = GPIO %d, Addr = 0x%02X (8-bit 0x%02X)\n",
                               sda, scl, addr, addr << 1);
                        printf(">>> Power Enable State = %d\n", pwr);
                        printf("**************************************************************\n\n");
                        acks_found++;
                    }
                }
            }
        }
    }

    /* Reset power rails */
    set_power_rails(0);

    if (acks_found == 0) {
        printf("\n[SWEEP RESULT]: Completed full sweep across %zu candidate GPIOs and power states.\n", num_safe);
        printf("[SWEEP RESULT]: Zero ACKs received. The BD37033 I2C control bus is not routed to SoC GPIOs or is unpowered.\n");
    } else {
        printf("\n[SWEEP RESULT]: Found %d responsive I2C pin pair(s)!\n", acks_found);
    }
}

static uint8_t compute_volume_gain(uint8_t level) {
    if (level == 32) return 0x80;
    if (level == 0)  return 0xFF;
    if (level >= 16) {
        return (uint8_t)(160 - level);
    }
    float x = (float)(32 - level);
    float gain = (x + 128.0f) + ((x - 16.0f) / 10.0f) * 16.0f;
    int igain = (int)(gain + 0.5f);
    if (igain > 255) igain = 255;
    if (igain < 128) igain = 128;
    return (uint8_t)igain;
}

static void print_usage(const char *prog) {
    printf("Usage: %s <command> [options]\n\n", prog);
    printf("Commands:\n");
    printf("  --scan                      Scan kernel I2C buses with GPIO34=0 and GPIO34=1\n");
    printf("  --sweep                     Exhaustive safe GPIO matrix sweep across spare SoC pins & power rails\n");
    printf("  --gpio <0|1>                Set GPIO 34 output value\n");
    printf("  --status                    Display current GPIO 34 sysfs status\n");
    printf("  --init <bus> [addr]         Send 20-byte power-on burst (default addr: 0x40)\n");
    printf("  --volume <bus> <level>      Set volume (0..32) on sub-address 0x20\n");
    printf("  --mute <bus> <0|1>          Set hardware mute bit (reg 0x06 bit 7)\n");
    printf("  --input <bus> <ch>          Select input channel (0..31, reg 0x05)\n");
    printf("  --raw <bus> <addr> <subaddr> <hex_bytes...>\n");
    printf("\nExamples:\n");
    printf("  %s --scan\n", prog);
    printf("  %s --sweep\n", prog);
    printf("  %s --gpio 0\n", prog);
    printf("  %s --init /dev/i2c-1 0x40\n", prog);
    printf("  %s --volume /dev/i2c-1 25\n", prog);
    printf("  %s --mute /dev/i2c-1 0\n", prog);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "--scan") == 0) {
        do_scan();
        return 0;
    }

    if (strcmp(argv[1], "--sweep") == 0) {
        do_sweep();
        return 0;
    }

    if (strcmp(argv[1], "--status") == 0) {
        print_gpio34_status();
        return 0;
    }

    if (strcmp(argv[1], "--gpio") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: --gpio requires 0 or 1\n");
            return 1;
        }
        int val = atoi(argv[2]);
        if (set_gpio34_state(val) == 0) {
            printf("Successfully drove GPIO %d to %d\n", BD37033_GPIO_PIN, val);
            print_gpio34_status();
        } else {
            fprintf(stderr, "Failed to drive GPIO %d\n", BD37033_GPIO_PIN);
            return 1;
        }
        return 0;
    }

    if (strcmp(argv[1], "--init") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: --init requires <bus_path> [addr]\n");
            return 1;
        }
        const char *bus = argv[2];
        uint8_t addr = (argc >= 4) ? (uint8_t)strtoul(argv[3], NULL, 0) : BD37033_ADDR_GND;

        printf("Initializing BD37033 on %s (addr=0x%02X)...\n", bus, addr);
        printf("Driving GPIO 34 LOW...\n");
        set_gpio34_state(0);
        usleep(10000);

        printf("Sending 20-byte configuration burst (sub-address 0x01)...\n");
        int ret = i2c_write_bytes(bus, addr, 0x01, s_bd37033_init_data, sizeof(s_bd37033_init_data));
        if (ret == 0) {
            printf(">>> SUCCESS: 20-byte burst written and ACKed by BD37033! <<<\n");
        } else {
            fprintf(stderr, ">>> FAILED: I2C write error / No ACK from 0x%02X on %s <<<\n", addr, bus);
            return 1;
        }
        return 0;
    }

    if (strcmp(argv[1], "--volume") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Error: --volume requires <bus_path> <level 0-32>\n");
            return 1;
        }
        const char *bus = argv[2];
        int level = atoi(argv[3]);
        if (level < 0) level = 0;
        if (level > 32) level = 32;

        uint8_t gain = compute_volume_gain((uint8_t)level);
        printf("Setting volume level=%d (gain byte=0x%02X) on %s at reg 0x20...\n", level, gain, bus);
        int ret = i2c_write_bytes(bus, BD37033_ADDR_GND, 0x20, &gain, 1);
        if (ret == 0) {
            printf("Volume updated successfully\n");
        } else {
            fprintf(stderr, "Failed to write volume register\n");
            return 1;
        }
        return 0;
    }

    if (strcmp(argv[1], "--mute") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Error: --mute requires <bus_path> <0|1>\n");
            return 1;
        }
        const char *bus = argv[2];
        int mute = atoi(argv[3]);
        uint8_t reg_val = mute ? 0x80 : 0x00; /* Bit 7 = Mute */

        printf("%smuting BD37033 (reg 0x06 = 0x%02X) on %s...\n", mute ? "M" : "Un", reg_val, bus);
        int ret = i2c_write_bytes(bus, BD37033_ADDR_GND, 0x06, &reg_val, 1);
        if (ret == 0) {
            printf("Mute state updated successfully\n");
        } else {
            fprintf(stderr, "Failed to write mute register\n");
            return 1;
        }
        return 0;
    }

    if (strcmp(argv[1], "--input") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Error: --input requires <bus_path> <channel 0-31>\n");
            return 1;
        }
        const char *bus = argv[2];
        uint8_t ch = (uint8_t)atoi(argv[3]);

        printf("Selecting input channel %u (reg 0x05) on %s...\n", ch, bus);
        int ret = i2c_write_bytes(bus, BD37033_ADDR_GND, 0x05, &ch, 1);
        if (ret == 0) {
            printf("Input channel updated successfully\n");
        } else {
            fprintf(stderr, "Failed to write input channel register\n");
            return 1;
        }
        return 0;
    }

    if (strcmp(argv[1], "--raw") == 0) {
        if (argc < 5) {
            fprintf(stderr, "Error: --raw requires <bus_path> <dev_addr> <sub_addr> [byte1 byte2...]\n");
            return 1;
        }
        const char *bus = argv[2];
        uint8_t dev_addr = (uint8_t)strtoul(argv[3], NULL, 0);
        uint8_t sub_addr = (uint8_t)strtoul(argv[4], NULL, 0);

        uint8_t payload[32];
        size_t len = 0;
        for (int i = 5; i < argc && len < sizeof(payload); ++i) {
            payload[len++] = (uint8_t)strtoul(argv[i], NULL, 0);
        }

        printf("Writing raw I2C to %s addr=0x%02X sub_addr=0x%02X (len=%zu)...\n", bus, dev_addr, sub_addr, len);
        int ret = i2c_write_bytes(bus, dev_addr, sub_addr, payload, len);
        if (ret == 0) {
            printf("Raw write succeeded\n");
        } else {
            fprintf(stderr, "Raw write failed\n");
            return 1;
        }
        return 0;
    }

    print_usage(argv[0]);
    return 1;
}
