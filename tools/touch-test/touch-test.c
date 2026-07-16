/*
 * ark-ts-test — live diagnostic tool for the ARK1680 resistive ADC
 * touchscreen (Limcet Hardware/ark1680_ts.c), same purpose/style as
 * tools/i2c-scan for the touch bus investigation: a static ARM binary to
 * run at the live root shell since the rootfs has no equivalent tooling
 * (no devmem2/evtest). See docs/ARK1680_TS_REVERSE_ENGINEERING.md.
 *
 * Two independent modes:
 *
 *   ark-ts-test regs
 *       Dumps the ADC/TSC (0xe4500000) and syscon/pinmux (0xe4900000)
 *       registers directly via /dev/mem — works with or without the
 *       ark1680_ts driver loaded/probed, so it can confirm the hardware
 *       itself is alive before blaming the driver.
 *
 *   ark-ts-test events /dev/input/eventN
 *       Opens the given evdev node and prints every event (like a
 *       minimal evtest) — use this once the driver is loaded to confirm
 *       ABS_X/ABS_Y/ABS_PRESSURE/BTN_TOUCH/SYN_REPORT actually come out
 *       when the panel is touched.
 *
 * Requires root (/dev/mem access) for the `regs` mode.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/input.h>

#define ADC_BASE_PHYS	0xe4500000UL
#define SYS_BASE_PHYS	0xe4900000UL
#define MAP_SIZE	0x1000UL

/* ADC/TSC register offsets, see docs/ARK1680_TS_REVERSE_ENGINEERING.md */
#define ARK_TS_CH_ENABLE	0x00
#define ARK_TS_CH_CONFIG	0x08
#define ARK_TS_IRQ_STATUS	0x0c
#define ARK_TS_ADC_RESULT	0x14
#define ARK_TS_RAW_X		0x24
#define ARK_TS_RAW_Y		0x28
#define ARK_TS_DBCNT		0x2c
#define ARK_TS_DETINTER	0x30

/* Syscon/pinmux register offsets */
#define ARK_SYS_CLKEN		0x48
#define ARK_SYS_PADCFG		0x50
#define ARK_SYS_ADCCLKDIV	0x64
#define ARK_SYS_PINMUX0	0x140
#define ARK_SYS_PINMUX1	0x144

static volatile unsigned char *map_phys(int fd, unsigned long phys)
{
	void *m = mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, phys);
	if (m == MAP_FAILED)
		return NULL;
	return (volatile unsigned char *)m;
}

static unsigned int rd(volatile unsigned char *base, unsigned int off)
{
	return *(volatile unsigned int *)(base + off);
}

static void print_reg(const char *name, unsigned int off, unsigned int val)
{
	printf("  [+0x%03x] %-12s = 0x%08x\n", off, name, val);
}

static int cmd_regs(void)
{
	int fd;
	volatile unsigned char *adc, *sys;

	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) {
		fprintf(stderr, "open /dev/mem failed: %s (are you root?)\n", strerror(errno));
		return 1;
	}

	adc = map_phys(fd, ADC_BASE_PHYS);
	if (!adc) {
		fprintf(stderr, "mmap ADC/TSC block (0x%lx) failed: %s\n", ADC_BASE_PHYS, strerror(errno));
		close(fd);
		return 1;
	}

	sys = map_phys(fd, SYS_BASE_PHYS);
	if (!sys) {
		fprintf(stderr, "mmap syscon block (0x%lx) failed: %s\n", SYS_BASE_PHYS, strerror(errno));
		close(fd);
		return 1;
	}

	printf("ADC/TSC block (phys 0x%08lx):\n", ADC_BASE_PHYS);
	print_reg("ch_enable", ARK_TS_CH_ENABLE, rd(adc, ARK_TS_CH_ENABLE));
	print_reg("ch_config", ARK_TS_CH_CONFIG, rd(adc, ARK_TS_CH_CONFIG));
	print_reg("irq_status", ARK_TS_IRQ_STATUS, rd(adc, ARK_TS_IRQ_STATUS));
	print_reg("adc_result", ARK_TS_ADC_RESULT, rd(adc, ARK_TS_ADC_RESULT));
	print_reg("raw_x", ARK_TS_RAW_X, rd(adc, ARK_TS_RAW_X));
	print_reg("raw_y", ARK_TS_RAW_Y, rd(adc, ARK_TS_RAW_Y));
	print_reg("dbcnt", ARK_TS_DBCNT, rd(adc, ARK_TS_DBCNT));
	print_reg("detinter", ARK_TS_DETINTER, rd(adc, ARK_TS_DETINTER));

	printf("\nSyscon/pinmux block (phys 0x%08lx):\n", SYS_BASE_PHYS);
	print_reg("clken", ARK_SYS_CLKEN, rd(sys, ARK_SYS_CLKEN));
	print_reg("padcfg", ARK_SYS_PADCFG, rd(sys, ARK_SYS_PADCFG));
	print_reg("adcclkdiv", ARK_SYS_ADCCLKDIV, rd(sys, ARK_SYS_ADCCLKDIV));
	print_reg("pinmux0", ARK_SYS_PINMUX0, rd(sys, ARK_SYS_PINMUX0));
	print_reg("pinmux1", ARK_SYS_PINMUX1, rd(sys, ARK_SYS_PINMUX1));

	printf("\nraw_x/raw_y update live even without the driver bound --\n"
	       "run this a few times while touching the panel to sanity-check\n"
	       "the hardware is responding before debugging the driver.\n");

	munmap((void *)adc, MAP_SIZE);
	munmap((void *)sys, MAP_SIZE);
	close(fd);
	return 0;
}

static const char *ev_type_name(unsigned short type)
{
	switch (type) {
	case EV_SYN: return "EV_SYN";
	case EV_KEY: return "EV_KEY";
	case EV_ABS: return "EV_ABS";
	default: return "EV_?";
	}
}

static const char *ev_code_name(unsigned short type, unsigned short code)
{
	if (type == EV_SYN && code == SYN_REPORT) return "SYN_REPORT";
	if (type == EV_KEY && code == BTN_TOUCH) return "BTN_TOUCH";
	if (type == EV_ABS && code == ABS_X) return "ABS_X";
	if (type == EV_ABS && code == ABS_Y) return "ABS_Y";
	if (type == EV_ABS && code == ABS_PRESSURE) return "ABS_PRESSURE";
	return "?";
}

static int cmd_events(const char *devpath)
{
	int fd = open(devpath, O_RDONLY);
	struct input_event ev;
	char name[256] = "?";

	if (fd < 0) {
		fprintf(stderr, "open %s failed: %s\n", devpath, strerror(errno));
		return 1;
	}

	int version;
	if (ioctl(fd, EVIOCGVERSION, &version) < 0) {
		fprintf(stderr, "error: %s is not a valid evdev input device\n", devpath);
		close(fd);
		return 1;
	}

	if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0)
		strcpy(name, "?");
	printf("Listening on %s (\"%s\") -- touch the panel, Ctrl-C to stop\n", devpath, name);

	while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
		printf("type=%-6s code=%-11s value=%d\n",
		       ev_type_name(ev.type), ev_code_name(ev.type, ev.code), ev.value);
	}

	close(fd);
	return 0;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s regs\n"
		"       %s events /dev/input/eventN\n",
		argv0, argv0);
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		usage(argv[0]);
		return 1;
	}
	if (strcmp(argv[1], "regs") == 0)
		return cmd_regs();
	if (strcmp(argv[1], "events") == 0 && argc >= 3)
		return cmd_events(argv[2]);

	usage(argv[0]);
	return 1;
}
