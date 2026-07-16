/*
 * i2c-scan — minimal standalone i2c bus scanner for the live root prompt.
 *
 * Built to settle the "which bus does the GT911 touch controller actually
 * ACK on — hardware &i2c0 or bit-banged i2c-gpio-0?" question empirically,
 * since the target rootfs has no i2c-tools and inference from disassembly
 * (docs/boot_experiment_log.md) has produced two contradictory DTS commits
 * (7c7ce4c moved it to &i2c0, 0be21c7 moved it back). See
 * docs/boot_experiment_log.md "Systematic I2C bus verification".
 *
 * Usage: i2c-scan /dev/i2c-0 [/dev/i2c-1 ...]
 * For each bus, attempts a 1-byte read at every address 0x03-0x77 and
 * reports which addresses ACK. A device answering at 0x5d confirms that
 * bus carries the GT911.
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

static void scan_bus(const char *path)
{
	int fd = open(path, O_RDWR);
	if (fd < 0) {
		printf("%s: open failed: %s\n", path, strerror(errno));
		return;
	}

	printf("Scanning %s ...\n", path);
	printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");

	for (int row = 0; row < 0x80; row += 16) {
		printf("%02x: ", row);
		for (int col = 0; col < 16; col++) {
			int addr = row + col;
			if (addr < 0x03 || addr > 0x77) {
				printf("   ");
				continue;
			}
			int busy = (ioctl(fd, I2C_SLAVE, addr) < 0);
			if (busy) {
				if (ioctl(fd, I2C_SLAVE_FORCE, addr) < 0) {
					printf("XX ");
					continue;
				}
			}
			unsigned char buf;
			if (read(fd, &buf, 1) == 1) {
				if (busy)
					printf("%02x* ", addr);
				else
					printf("%02x  ", addr);
			} else {
				if (busy)
					printf("XX  ");
				else
					printf("--  ");
			}
		}
		printf("\n");
	}
	close(fd);
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s /dev/i2c-N [/dev/i2c-M ...]\n", argv[0]);
		return 1;
	}
	for (int i = 1; i < argc; i++)
		scan_bus(argv[i]);
	return 0;
}
