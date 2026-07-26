/*
 * pinmux-watch -- tight-loop poller for the ARK1668 pad-mux control
 * registers covering the LCD RGB888 data pins, to catch a live
 * function-select change in the act (GPIO vs LCD) correlated with I2C
 * bus activity on the pins shared with it.
 *
 * Why: docs/DISPLAY_SUBSYSTEM.md's I2C_GPIO0_LCD_PIN_CONFLICT section
 * found -- via a single debugfs snapshot of
 * /sys/kernel/debug/pinctrl/e4900000.pinctrl/pinmux-pins -- that pins
 * 2/3 (LCD r0/r1, shared with i2c-gpio-0's SCL/SDA for the RN6752
 * camera decoder) stay muxed to LCD function even after i2c-gpio-0
 * successfully gpio_request()s them, concluding the conflict only
 * breaks I2C (pure software bookkeeping, no real electrical effect on
 * the pad) -- not the LCD. But that was one static snapshot taken at
 * an arbitrary idle moment, not during active I2C bit-toggling.
 *
 * Since then, real hardware testing showed a color tint that visibly
 * MOVES across the screen in sync with I2C message activity -- which
 * only makes sense if the pad's actual drive source really does
 * change, at least momentarily, during a transaction. This tool
 * settles it empirically: poll the exact register as fast as possible
 * and log every value change with a monotonic timestamp, so a change
 * can be correlated against I2C traffic triggered in another terminal
 * (e.g. adjusting volume to hit BD37033, which lives on the i2c-gpio-1
 * bus sharing pin 9 = LCD r7 -- see below).
 *
 * Register layout (drivers/pinctrl/pinctrl-ark.c's ark1668_pin_map[],
 * indexed by global pin number == array index for pins < gpio_mux_pins):
 *   pin 2 (r0) .. pin 9 (r7): all packed into ONE 32-bit register at
 *   pinctrl0's base (0xe4900000, from ark1668.dtsi) + 0x1c0, one 4-bit
 *   nibble per pin, in pin order (pin2 = bits[3:0], pin3 = bits[7:4],
 *   ..., pin9 = bits[31:28]). ark_gpio_request_enable() clears a pin's
 *   nibble to 0 for GPIO function; nonzero means some peripheral
 *   function is selected (LCD's real value is whatever ARK_PVAL_1
 *   resolves to in the pin's own nibble encoding).
 *
 *   pin 9 specifically = LCD r7 AND i2c-gpio-1's SDA (BD37033 audio
 *   chip bus) -- a second, independent conflict beyond the already-
 *   documented pin2/3 one, and a much higher-impact one if r7 is the
 *   R channel's most-significant bit (128 of 255) rather than r0/r1's
 *   two least-significant bits (at most +/-3).
 *
 *   g0-g7 (pins 10-17) and b0-b7 (pins 18-25) are packed the same way
 *   into the next two registers (+0x1c4, +0x1c8) -- included too, in
 *   case interference isn't limited to the R channel alone.
 *
 * Usage: pinmux-watch [duration_sec]
 *   duration_sec: how long to poll, default 30. Trigger I2C activity
 *   (adjust volume, wait for an rn6752 retry, etc.) in another
 *   terminal/session while this runs.
 *
 * Output: one line per detected change,
 *   [<seq>] t=+<usec> RGB_0_7=0x<val> (was 0x<val>) GRN_0_7=... BLU_0_7=...
 * A change in RGB_0_7 during I2C activity, especially in the nibble
 * matching whichever pin is mid-transaction, is the smoking gun.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <time.h>

#define PINCTRL_BASE 0xE4900000UL
#define REG_R_0_7    0x1c0   /* pins 2-9:  r0-r7 */
#define REG_G_0_7    0x1c4   /* pins 10-17: g0-g7 */
#define REG_B_0_7    0x1c8   /* pins 18-25: b0-b7 */
#define MAP_LEN      0x200UL /* covers all three regs plus headroom */

static long now_usec(struct timespec *t0)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (t.tv_sec - t0->tv_sec) * 1000000L + (t.tv_nsec - t0->tv_nsec) / 1000L;
}

int main(int argc, char **argv)
{
	int duration_sec = (argc >= 2) ? atoi(argv[1]) : 30;

	int fd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (fd < 0) {
		fprintf(stderr, "open(/dev/mem): %s\n", strerror(errno));
		return 1;
	}

	void *map = mmap(NULL, MAP_LEN, PROT_READ, MAP_SHARED, fd, PINCTRL_BASE);
	if (map == MAP_FAILED) {
		fprintf(stderr, "mmap(0x%lx): %s\n", PINCTRL_BASE, strerror(errno));
		close(fd);
		return 1;
	}

	volatile unsigned int *reg_r = (volatile unsigned int *)((char *)map + REG_R_0_7);
	volatile unsigned int *reg_g = (volatile unsigned int *)((char *)map + REG_G_0_7);
	volatile unsigned int *reg_b = (volatile unsigned int *)((char *)map + REG_B_0_7);

	unsigned int last_r = *reg_r, last_g = *reg_g, last_b = *reg_b;

	fprintf(stderr,
		"pinmux-watch: base=0x%08lx R@+0x%03x=0x%08x G@+0x%03x=0x%08x B@+0x%03x=0x%08x\n"
		"polling for %ds -- trigger I2C activity (volume change, etc.) now.\n"
		"R nibbles (LSB first) = pin2(r0) pin3(r1) pin4(r2) pin5(r3) pin6(r4) pin7(r5) pin8(r6) pin9(r7)\n"
		"pin2/3 = i2c-gpio-0 SCL/SDA (RN6752 camera). pin9 = i2c-gpio-1 SDA (BD37033 audio).\n",
		PINCTRL_BASE, REG_R_0_7, last_r, REG_G_0_7, last_g, REG_B_0_7, last_b, duration_sec);

	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	long seq = 0;

	while (now_usec(&t0) < (long)duration_sec * 1000000L) {
		unsigned int r = *reg_r;
		unsigned int g = *reg_g;
		unsigned int b = *reg_b;

		if (r != last_r || g != last_g || b != last_b) {
			seq++;
			printf("[%ld] t=+%ldus RGB_0_7=0x%08x (was 0x%08x) GRN_0_7=0x%08x (was 0x%08x) "
			       "BLU_0_7=0x%08x (was 0x%08x)\n",
			       seq, now_usec(&t0), r, last_r, g, last_g, b, last_b);
			fflush(stdout);
			last_r = r; last_g = g; last_b = b;
		}
	}

	fprintf(stderr, "done -- %ld change(s) detected in %ds.\n", seq, duration_sec);

	munmap(map, MAP_LEN);
	close(fd);
	return 0;
}
