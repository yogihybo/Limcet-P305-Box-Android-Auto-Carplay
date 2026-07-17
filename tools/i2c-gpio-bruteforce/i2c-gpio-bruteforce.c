/*
 * i2c-gpio-bruteforce -- find the real SDA/SCL pins for a chip whose I2C
 * address is known but whose physical wiring isn't, by manually
 * bit-banging every candidate pin as SCL against every other candidate
 * pin as SDA and checking for a real ACK.
 *
 * Why this exists: the DTS's i2c-gpio-0 (RN6752 reversing-camera decoder,
 * addr 0x2c) and i2c-gpio-1's SDA half (BD37033 sound amp, addr 0x40 --
 * see docs/AUDIO_SUBSYSTEM_INVESTIGATION.md for how that address was
 * derived) were both found to sit on pins that a real stock pinmux
 * register readback proves are LCD pins instead (2026-07-17, see
 * docs/logs/archived/pindump stock.txt + tools/pin-dump/). No schematic
 * exists for this board, and disassembly/physical inspection both hit a
 * dead end on these two GPIOs specifically (docs/HARDWARE_AND_SOC_
 * REFERENCE.md §7's closing paragraph). This is the direct, empirical
 * alternative: since real hardware either ACKs or it doesn't, sweep every
 * pin gpio0's restored range and gpio1-3 (all pins NOT already claimed by
 * a named pinctrl function -- see tools/pin-dump/find-unclaimed-pins.py's
 * output) as a candidate SDA/SCL pair and look for the chip's known
 * address responding.
 *
 * Treats each pin as open-drain (matches this board's existing i2c-gpio
 * nodes: "i2c-gpio,scl-output-only" + kernel's own "enforced open drain"
 * warnings imply real external pull-ups) -- "release" = direction=in
 * (let the pull-up bring it high), "drive low" = direction=out, value=0.
 * Never drives a pin high directly, so a genuine output-only or
 * input-only pin used for something else is very unlikely to be damaged
 * by a handful of brief low pulses -- but this is still real hardware:
 * only run it with the target chip's power rail actually live, and
 * expect that pins already in active use for something else may log
 * kernel warnings or misbehave briefly during the sweep.
 *
 * Usage:
 *   i2c-gpio-bruteforce <addr7-hex> [<addr7-hex> ...] -- <pin> [<pin> ...]
 *
 * Example (RN6752 at 0x2c and BD37033 at 0x40, sweeping the unclaimed
 * pins tools/pin-dump/find-unclaimed-pins.py reported):
 *   i2c-gpio-bruteforce 2c 40 -- 0 1 30 31 32 ... 126
 *
 * Prints one line per confirmed ACK: "ACK addr=0x2c scl=<pin> sda=<pin>"
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#define GPIO_SYSFS "/sys/class/gpio"
#define DELAY_US 5

static int write_file(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;
	ssize_t n = write(fd, val, strlen(val));
	close(fd);
	return (n == (ssize_t)strlen(val)) ? 0 : -1;
}

static int gpio_export(int pin)
{
	char path[64], val[16];
	snprintf(path, sizeof(path), GPIO_SYSFS "/gpio%d", pin);
	if (access(path, F_OK) == 0)
		return 0; /* already exported */
	snprintf(val, sizeof(val), "%d", pin);
	return write_file(GPIO_SYSFS "/export", val);
}

static void gpio_unexport(int pin)
{
	char val[16];
	snprintf(val, sizeof(val), "%d", pin);
	write_file(GPIO_SYSFS "/unexport", val);
}

static int gpio_release(int pin) /* input = let pull-up float it high */
{
	char path[64];
	snprintf(path, sizeof(path), GPIO_SYSFS "/gpio%d/direction", pin);
	return write_file(path, "in");
}

static int gpio_drive_low(int pin)
{
	char path[64];
	snprintf(path, sizeof(path), GPIO_SYSFS "/gpio%d/direction", pin);
	if (write_file(path, "out") < 0)
		return -1;
	snprintf(path, sizeof(path), GPIO_SYSFS "/gpio%d/value", pin);
	return write_file(path, "0");
}

static int gpio_read(int pin)
{
	char path[64], buf[8];
	snprintf(path, sizeof(path), GPIO_SYSFS "/gpio%d/value", pin);
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	ssize_t n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	return buf[0] - '0';
}

/* Returns 1 if the target address ACKs (write direction), 0 if not,
 * -1 on a GPIO access error. */
static int try_address(int scl, int sda, unsigned char addr7)
{
	gpio_release(scl);
	gpio_release(sda);
	usleep(DELAY_US);

	/* START: SDA high->low while SCL high */
	gpio_release(sda);
	gpio_release(scl);
	usleep(DELAY_US);
	gpio_drive_low(sda);
	usleep(DELAY_US);
	gpio_drive_low(scl);
	usleep(DELAY_US);

	unsigned char byte = (addr7 << 1); /* write */
	for (int i = 7; i >= 0; i--) {
		int bit = (byte >> i) & 1;
		if (bit)
			gpio_release(sda);
		else
			gpio_drive_low(sda);
		usleep(DELAY_US);
		gpio_release(scl); /* clock high */
		usleep(DELAY_US);
		gpio_drive_low(scl);
		usleep(DELAY_US);
	}

	/* ACK bit: release SDA, pulse SCL, sample */
	gpio_release(sda);
	usleep(DELAY_US);
	gpio_release(scl);
	usleep(DELAY_US);
	int ack_bit = gpio_read(sda);
	gpio_drive_low(scl);
	usleep(DELAY_US);

	/* STOP: SDA low->high while SCL high */
	gpio_drive_low(sda);
	usleep(DELAY_US);
	gpio_release(scl);
	usleep(DELAY_US);
	gpio_release(sda);
	usleep(DELAY_US);

	if (ack_bit < 0)
		return -1;
	return ack_bit == 0 ? 1 : 0; /* I2C ACK is SDA pulled low */
}

int main(int argc, char **argv)
{
	unsigned char addrs[32];
	int naddrs = 0;
	int pins[256];
	int npins = 0;
	int i = 1;

	for (; i < argc && strcmp(argv[i], "--") != 0; i++) {
		if (naddrs >= (int)(sizeof(addrs) / sizeof(addrs[0]))) {
			fprintf(stderr, "too many addresses\n");
			return 1;
		}
		addrs[naddrs++] = (unsigned char)strtol(argv[i], NULL, 16);
	}
	if (i >= argc) {
		fprintf(stderr,
			"usage: %s <addr7-hex> [...] -- <pin> [<pin> ...]\n",
			argv[0]);
		return 1;
	}
	i++; /* skip -- */
	for (; i < argc; i++) {
		if (npins >= (int)(sizeof(pins) / sizeof(pins[0]))) {
			fprintf(stderr, "too many pins\n");
			return 1;
		}
		pins[npins++] = atoi(argv[i]);
	}
	if (naddrs == 0 || npins < 2) {
		fprintf(stderr, "need at least one address and two pins\n");
		return 1;
	}

	fprintf(stderr, "sweeping %d pins x %d pins x %d address(es) = %d attempts\n",
		npins, npins - 1, naddrs, npins * (npins - 1) * naddrs);

	for (int p = 0; p < npins; p++)
		if (gpio_export(pins[p]) < 0)
			fprintf(stderr, "warning: could not export pin %d (%s)\n",
				pins[p], strerror(errno));

	int found = 0;
	for (int a = 0; a < naddrs; a++) {
		for (int s = 0; s < npins; s++) {
			for (int d = 0; d < npins; d++) {
				if (s == d)
					continue;
				int result = try_address(pins[s], pins[d], addrs[a]);
				if (result == 1) {
					printf("ACK addr=0x%02x scl=%d sda=%d\n",
						addrs[a], pins[s], pins[d]);
					fflush(stdout);
					found++;
				}
			}
		}
	}

	for (int p = 0; p < npins; p++) {
		gpio_release(pins[p]);
		gpio_unexport(pins[p]);
	}

	fprintf(stderr, "done, %d ACK(s) found\n", found);
	return found > 0 ? 0 : 1;
}
