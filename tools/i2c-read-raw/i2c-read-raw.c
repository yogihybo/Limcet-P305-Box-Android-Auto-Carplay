/*
 * i2c-read-raw -- plain multi-byte I2C read, no register-address phase.
 *
 * Why this exists: i2c-dump does a write-register-then-read (repeated
 * start) transaction per byte -- the standard way to read a normal
 * addressable register file. Against the unidentified device(s) at
 * 0x10/0x11 on /dev/i2c-0 (found via i2c-scan, see tools/i2c-scan/), that
 * came back "XX" for every single register (2026-07-17 live test) even
 * though i2c-scan's own plain single-byte read ACKs fine at those
 * addresses. That combination -- ACKs a bare read, NAKs a register-
 * address write -- doesn't necessarily mean "no device"; it's also what
 * a chip without a normal addressable register file looks like (a
 * current-address/SMBus quick-read style device, a sequential-read
 * EEPROM, a fixed ID/status word). This tool does the plain read
 * i2c-scan already proved works, just for more than 1 byte, to see
 * what such a device actually streams out by default -- still fully
 * read-only, no register write attempted at all.
 *
 * Usage: i2c-read-raw /dev/i2c-N slave_addr [num_bytes]
 *   num_bytes defaults to 16, max 256.
 * Prints a hex dump plus an ASCII column (non-printable -> '.'), in case
 * the device streams out an identifiable ID/model string.
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s /dev/i2c-N slave_addr [num_bytes]\n", argv[0]);
		return 1;
	}

	const char *path = argv[1];
	int slave_addr = strtol(argv[2], NULL, 0);
	int n = (argc > 3) ? strtol(argv[3], NULL, 0) : 16;
	if (n <= 0 || n > 256)
		n = 16;

	int fd = open(path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
		return 1;
	}

	if (ioctl(fd, I2C_SLAVE, slave_addr) < 0) {
		fprintf(stderr, "ioctl I2C_SLAVE 0x%02x failed: %s\n",
			slave_addr, strerror(errno));
		close(fd);
		return 1;
	}

	unsigned char buf[256];
	ssize_t got = read(fd, buf, n);
	close(fd);

	if (got < 0) {
		fprintf(stderr, "plain read of %d bytes from 0x%02x on %s failed: %s\n",
			n, slave_addr, path, strerror(errno));
		return 1;
	}
	if (got == 0) {
		fprintf(stderr, "read() returned 0 bytes -- device ACKed the address but sent nothing\n");
		return 1;
	}

	printf("Plain read of %zd byte(s) from 0x%02x on %s (no register-address phase):\n",
		got, slave_addr, path);

	for (ssize_t row = 0; row < got; row += 16) {
		printf("%02zx: ", row);
		ssize_t rowend = row + 16 < got ? row + 16 : got;
		for (ssize_t i = row; i < row + 16; i++) {
			if (i < rowend)
				printf("%02x ", buf[i]);
			else
				printf("   ");
		}
		printf(" ");
		for (ssize_t i = row; i < rowend; i++)
			putchar(isprint(buf[i]) ? buf[i] : '.');
		printf("\n");
	}

	if (got < n)
		fprintf(stderr, "note: asked for %d bytes, only got %zd\n", n, got);

	return 0;
}
