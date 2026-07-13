/*
 * Bit-bangs I2C over two arbitrary sysfs GPIO pins and scans the full
 * 7-bit address range for ACKs. Written to hunt for the real physical
 * wiring of GT911/rn6752/BD37033 after the stock board file's own
 * candidate buses were all found conflicted or silent — see
 * docs/I2C_GPIO0_LCD_PIN_CONFLICT.md.
 *
 * CAUTION: only pass pins already verified free of any pinctrl DTS
 * claim (LCD, NAND, uart0/console, spi, i2s, existing i2c buses, PWM
 * backlight, usb_id/usb_pwr GPIOs). Driving a claimed pin can corrupt
 * NAND, kill the console, or glitch other live peripherals.
 *
 * No external pull-up is guaranteed on untested pins — a real device
 * with no pull-up may still show no ACK here. Absence of an ACK is
 * not proof a device isn't wired to that pin.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static int SDA, SCL;
static int sda_fd, scl_fd;

static void udelay_(void) { usleep(5); }

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

static void scl_hi(void) { gset(scl_fd, 1); udelay_(); }
static void scl_lo(void) { gset(scl_fd, 0); udelay_(); }
static void sda_hi(void) { gset(sda_fd, 1); udelay_(); }
static void sda_lo(void) { gset(sda_fd, 0); udelay_(); }

static void i2c_start(void) {
	sda_hi();
	scl_hi();
	sda_lo();
	scl_lo();
}
static void i2c_stop(void) {
	sda_lo();
	scl_hi();
	sda_hi();
}

/* returns 1 if ACK (device present), 0 if NACK/no response */
static int i2c_write_byte(unsigned char byte) {
	for (int i = 7; i >= 0; i--) {
		if (byte & (1 << i))
			sda_hi();
		else
			sda_lo();
		scl_hi();
		scl_lo();
	}
	gdir(SDA, "in");
	udelay_();
	scl_hi();
	int ack = !gget(sda_fd);
	scl_lo();
	gdir(SDA, "out");
	udelay_();
	return ack;
}

int main(int argc, char **argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: %s sda_pin scl_pin\n", argv[0]);
		return 1;
	}
	SDA = atoi(argv[1]);
	SCL = atoi(argv[2]);

	gexport(SDA);
	gexport(SCL);
	gdir(SCL, "out");
	gdir(SDA, "out");
	sda_fd = gopen(SDA);
	scl_fd = gopen(SCL);
	if (sda_fd < 0 || scl_fd < 0) {
		fprintf(stderr, "failed to open gpio value files for %d/%d (busy/claimed?)\n", SDA, SCL);
		return 2;
	}
	gset(scl_fd, 1);
	gset(sda_fd, 1);
	usleep(50);

	printf("Scanning SDA=gpio%d SCL=gpio%d ...\n", SDA, SCL);
	int found = 0;
	for (int addr = 0x03; addr <= 0x77; addr++) {
		i2c_start();
		int ack = i2c_write_byte((addr << 1) | 0);
		i2c_stop();
		if (ack) {
			printf("  ACK at 0x%02x\n", addr);
			found++;
		}
		usleep(500);
	}
	if (!found)
		printf("  (no ACKs)\n");

	close(sda_fd);
	close(scl_fd);
	/* leave direction as input (safe idle) before releasing */
	gdir(SDA, "in");
	gdir(SCL, "in");
	gunexport(SDA);
	gunexport(SCL);
	return 0;
}
