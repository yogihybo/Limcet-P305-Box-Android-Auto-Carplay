#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

static int i2c_read_reg(int fd, unsigned char slave_addr, unsigned char reg, unsigned char *val) {
	struct i2c_msg msgs[2];
	struct i2c_rdwr_ioctl_data msgset;

	// Msg 1: Write register address to read from
	msgs[0].addr = slave_addr;
	msgs[0].flags = 0; // Write
	msgs[0].len = 1;
	msgs[0].buf = &reg;

	// Msg 2: Read 1 byte from that register
	msgs[1].addr = slave_addr;
	msgs[1].flags = I2C_M_RD; // Read
	msgs[1].len = 1;
	msgs[1].buf = val;

	msgset.msgs = msgs;
	msgset.nmsgs = 2;

	if (ioctl(fd, I2C_RDWR, &msgset) < 0) {
		return -1;
	}
	return 0;
}

int main(int argc, char **argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: %s /dev/i2c-N slave_addr [num_registers]\n", argv[0]);
		return 1;
	}

	const char *path = argv[1];
	int slave_addr = strtol(argv[2], NULL, 0);
	int limit = (argc > 3) ? strtol(argv[3], NULL, 0) : 256;

	if (limit <= 0 || limit > 256) limit = 256;

	int fd = open(path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
		return 1;
	}

	printf("Dumping registers for device 0x%02x on %s:\n", slave_addr, path);
	printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");

	for (int row = 0; row < limit; row += 16) {
		printf("%02x: ", row);
		for (int col = 0; col < 16; col++) {
			int reg = row + col;
			if (reg >= limit) {
				printf("   ");
				continue;
			}
			unsigned char val;
			if (i2c_read_reg(fd, slave_addr, reg, &val) == 0) {
				printf("%02x ", val);
			} else {
				printf("XX ");
			}
		}
		printf("\n");
	}

	close(fd);
	return 0;
}
