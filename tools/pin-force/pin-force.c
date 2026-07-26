/*
 * pin-force -- directly force one of the ARK1668 LCD RGB888 data pins
 * (r0-r7, pins 2-9) into GPIO mode at a specific level, or restore it
 * to LCD function, via raw physical register writes. Bypasses the
 * kernel entirely -- a deliberate, active test: if forcing (say) r7
 * low/high produces a predictable, visible change on the LCDTest grid,
 * that's direct proof the pad really is drivable via this path despite
 * appearing permanently LCD-muxed in a static debugfs snapshot (see
 * tools/pinmux-watch/ and docs/DISPLAY_SUBSYSTEM.md's
 * I2C_GPIO0_LCD_PIN_CONFLICT section).
 *
 * Registers (see tools/pinmux-watch/README.md for the derivation):
 *   Pinmux:    0xe4900000 + 0x1c0, one 4-bit nibble per pin 2-9
 *              (pin N -> bits [4*(N-2)+3 : 4*(N-2)]). 0=GPIO function,
 *              1=LCD function (ARK_PVAL_1, dt-bindings/pinctrl/ark-pinfunc.h).
 *   GPIO0 MOD:   0xe4600000 + 0x00, bit N = pin N direction (clear = output).
 *   GPIO0 RDATA: 0xe4600000 + 0x04, bit N = pin N output level when in
 *              output mode (also used for readback).
 *
 * DANGER: this writes raw hardware registers on a live system,
 * bypassing every kernel driver's own bookkeeping (pinctrl-ark.c,
 * gpio-ark.c, ark1668_lcdfb.c all stay unaware). Expected to be safe
 * and fully reversible (a reboot restores the real boot-time pinmux
 * either way, and "lcd" mode restores the documented function value),
 * but this is exactly the kind of tool that's only appropriate for
 * live, deliberate, watched experiments -- not something to leave
 * running or call from a script.
 *
 * Usage: pin-force <pin 2-9> <status|gpio-low|gpio-high|lcd>
 *   status:     print the pin's current pinmux nibble + GPIO MOD/RDATA
 *               bit, without changing anything.
 *   gpio-low:   force pinmux to GPIO (nibble=0), set direction=output,
 *               drive the pin low.
 *   gpio-high:  same, but drive the pin high.
 *   lcd:        restore the pinmux nibble to 1 (LCD/ARK_PVAL_1). Does
 *               NOT touch GPIO_MOD/RDATA -- irrelevant once re-muxed
 *               away from GPIO.
 *
 * Pin reference: r0=2 r1=3 r2=4 r3=5 r4=6 r5=7 r6=8 r7=9.
 * pin 2/3 = i2c-gpio-0 SCL/SDA (RN6752 camera). pin 9 = i2c-gpio-1 SDA
 * (BD37033 audio) -- the higher-impact one if r7 really is the R
 * channel's MSB.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>

#define PINCTRL_BASE 0xE4900000UL
#define REG_R_0_7    0x1c0UL
#define GPIO0_BASE   0xE4600000UL
#define GPIO_MOD     0x00UL
#define GPIO_RDATA   0x04UL

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "Usage: %s <pin 2-9> <status|gpio-low|gpio-high|lcd>\n", argv[0]);
		fprintf(stderr, "  pin: r0=2 r1=3 r2=4 r3=5 r4=6 r5=7 r6=8 r7=9\n");
		return 1;
	}

	int pin = atoi(argv[1]);
	const char *action = argv[2];
	if (pin < 2 || pin > 9) {
		fprintf(stderr, "pin must be 2-9 (r0-r7)\n");
		return 1;
	}

	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) {
		fprintf(stderr, "open(/dev/mem): %s (need root)\n", strerror(errno));
		return 1;
	}

	long ps = sysconf(_SC_PAGESIZE);
	unsigned long pinctrl_page = PINCTRL_BASE & ~(unsigned long)(ps - 1);
	unsigned long gpio0_page = GPIO0_BASE & ~(unsigned long)(ps - 1);

	void *pinctrl_map = mmap(NULL, ps, PROT_READ | PROT_WRITE, MAP_SHARED, fd, pinctrl_page);
	void *gpio0_map = mmap(NULL, ps, PROT_READ | PROT_WRITE, MAP_SHARED, fd, gpio0_page);
	if (pinctrl_map == MAP_FAILED || gpio0_map == MAP_FAILED) {
		fprintf(stderr, "mmap failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	volatile unsigned int *reg_pinmux =
		(volatile unsigned int *)((char *)pinctrl_map + (PINCTRL_BASE - pinctrl_page) + REG_R_0_7);
	volatile unsigned int *reg_mod =
		(volatile unsigned int *)((char *)gpio0_map + (GPIO0_BASE - gpio0_page) + GPIO_MOD);
	volatile unsigned int *reg_rdata =
		(volatile unsigned int *)((char *)gpio0_map + (GPIO0_BASE - gpio0_page) + GPIO_RDATA);

	int nibble_shift = (pin - 2) * 4;

	if (strcmp(action, "status") == 0) {
		unsigned int mux = (*reg_pinmux >> nibble_shift) & 0xf;
		unsigned int mod = (*reg_mod >> pin) & 1;
		unsigned int rdata = (*reg_rdata >> pin) & 1;
		printf("pin %d (r%d): pinmux_nibble=0x%x (%s) gpio_mod=%u (%s) gpio_rdata=%u\n",
		       pin, pin - 2, mux, mux == 0 ? "GPIO" : (mux == 1 ? "LCD" : "other"),
		       mod, mod ? "input" : "output", rdata);
	} else if (strcmp(action, "gpio-low") == 0 || strcmp(action, "gpio-high") == 0) {
		int level = (strcmp(action, "gpio-high") == 0);

		unsigned int mux = *reg_pinmux;
		mux &= ~(0xfU << nibble_shift);
		*reg_pinmux = mux;

		unsigned int mod = *reg_mod;
		mod &= ~(1U << pin);
		*reg_mod = mod;

		unsigned int rdata = *reg_rdata;
		if (level) rdata |= (1U << pin);
		else rdata &= ~(1U << pin);
		*reg_rdata = rdata;

		printf("pin %d (r%d) forced to GPIO, driven %s. pinmux_nibble now 0x%x\n",
		       pin, pin - 2, level ? "HIGH" : "LOW", (*reg_pinmux >> nibble_shift) & 0xf);
	} else if (strcmp(action, "lcd") == 0) {
		unsigned int mux = *reg_pinmux;
		mux &= ~(0xfU << nibble_shift);
		mux |= (1U << nibble_shift);
		*reg_pinmux = mux;

		printf("pin %d (r%d) restored to LCD function. pinmux_nibble now 0x%x\n",
		       pin, pin - 2, (*reg_pinmux >> nibble_shift) & 0xf);
	} else {
		fprintf(stderr, "unknown action '%s'\n", action);
		munmap(pinctrl_map, ps);
		munmap(gpio0_map, ps);
		close(fd);
		return 1;
	}

	munmap(pinctrl_map, ps);
	munmap(gpio0_map, ps);
	close(fd);
	return 0;
}
