/*
 * i2c-write -- raw single-register I2C write via /dev/i2c-N, bypassing any
 * kernel client driver for the target address.
 *
 * Written to test whether BD37033 writes issued directly from userspace
 * (matching how stock's own audio-control code reaches this chip, per the
 * disassembled arki2c_open()/I2COperator ioctl(I2C_SLAVE, addr) pattern in
 * libMsnCommons.so / libMsnSound.so) succeed where the kernel's BD37033.c
 * driver's i2c_transfer() calls have always failed with
 * "bd37033_write_byte timeout" -- see docs/AUDIO_SUBSYSTEM_INVESTIGATION.md.
 * Both paths ultimately call i2c_transfer() on the same adapter, so this
 * doesn't bypass the electrical bus -- but it does bypass the in-kernel
 * i2c_client for that address (which can matter if that client's own
 * probe-time state, retry policy, or a stale bus lock is involved), and
 * gives a byte-identical write to what the working vendor path sends.
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

int main(int argc, char **argv) {
	if (argc < 5) {
		fprintf(stderr, "usage: %s /dev/i2c-N slave_addr reg val [retries]\n", argv[0]);
		fprintf(stderr, "  writes a single [reg, val] byte pair via I2C_RDWR\n");
		return 1;
	}

	const char *path = argv[1];
	unsigned char slave_addr = (unsigned char)strtol(argv[2], NULL, 0);
	unsigned char reg = (unsigned char)strtol(argv[3], NULL, 0);
	unsigned char val = (unsigned char)strtol(argv[4], NULL, 0);
	int retries = (argc > 5) ? strtol(argv[5], NULL, 0) : 5;

	int fd = open(path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
		return 1;
	}

	unsigned char buf[2] = { reg, val };
	struct i2c_msg msg = {
		.addr = slave_addr,
		.flags = 0,
		.len = 2,
		.buf = buf,
	};
	struct i2c_rdwr_ioctl_data msgset = { .msgs = &msg, .nmsgs = 1 };

	int attempt;
	int ret = -1;
	for (attempt = 0; attempt < retries; attempt++) {
		ret = ioctl(fd, I2C_RDWR, &msgset);
		if (ret >= 0)
			break;
		fprintf(stderr, "attempt %d: ioctl(I2C_RDWR) failed: %s (errno=%d)\n",
			attempt + 1, strerror(errno), errno);
	}

	if (ret >= 0) {
		printf("OK: wrote reg=0x%02x val=0x%02x to 0x%02x on %s (attempt %d)\n",
			reg, val, slave_addr, path, attempt + 1);
	} else {
		printf("FAIL: could not write reg=0x%02x val=0x%02x to 0x%02x on %s after %d attempts\n",
			reg, val, slave_addr, path, retries);
	}

	close(fd);
	return ret >= 0 ? 0 : 1;
}
