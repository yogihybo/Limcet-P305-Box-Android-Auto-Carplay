/*
 * lcd-test — live LCD/framebuffer diagnostic tool, same purpose/style as
 * tools/i2c-scan and tools/touch-test: a static ARM binary to run at
 * the live root shell, testing the raw kernel framebuffer (/dev/fb0)
 * directly -- no Qt/QWS server involved, so it works even though
 * MsnCoreApp/LCDTest -qws currently segfault (see
 * docs/ARK1680_TS_REVERSE_ENGINEERING.md "MsnCoreApp segfault").
 *
 * Modes:
 *
 *   lcd-test info
 *       Dumps FBIOGET_VSCREENINFO / FBIOGET_FSCREENINFO from /dev/fb0
 *       (resolution, bpp, line length, mem size, pixel field layout),
 *       and — if present — /dev/ark_display's ARKDISP_GET_SCREEN_INFO
 *       reply (see Limcet Hardware/ark_display.c), so the two can be
 *       cross-checked against each other.
 *
 *   lcd-test fill <red|green|blue|white|black|r,g,b>
 *       Fills the whole visible framebuffer with a solid color -- the
 *       simplest possible "is the panel actually showing anything"
 *       test, independent of any userspace UI stack.
 *
 *   lcd-test bars
 *       Draws vertical color-bars (white/yellow/cyan/green/magenta/red/
 *       blue/black), classic test-pattern style, across the full width.
 *
 *   lcd-test gradient
 *       Draws a horizontal red->green gradient, useful for spotting
 *       banding/bit-depth or panel-timing artifacts a solid fill won't
 *       show.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

#define ARK_DISPLAY_IOC_MAGIC	0xa0
#define ARKDISP_GET_SCREEN_INFO	_IOWR(ARK_DISPLAY_IOC_MAGIC, 29, unsigned long)

struct ark_screen_info {
	unsigned int screen_id;
	unsigned int width_px;
	unsigned int height_px;
	unsigned int mm_width;
	unsigned int mm_height;
};

static void print_vinfo(const struct fb_var_screeninfo *v)
{
	printf("FBIOGET_VSCREENINFO:\n");
	printf("  xres=%u yres=%u xres_virtual=%u yres_virtual=%u\n",
	       v->xres, v->yres, v->xres_virtual, v->yres_virtual);
	printf("  xoffset=%u yoffset=%u%s\n", v->xoffset, v->yoffset,
	       (v->xoffset || v->yoffset) ?
	       "  <-- nonzero: fb is currently panned away from (0,0);"
	       " a naive write to mmap offset 0 would be invisible" : "");
	printf("  bits_per_pixel=%u grayscale=%u\n", v->bits_per_pixel, v->grayscale);
	printf("  red   offset=%u length=%u msb_right=%u\n",
	       v->red.offset, v->red.length, v->red.msb_right);
	printf("  green offset=%u length=%u msb_right=%u\n",
	       v->green.offset, v->green.length, v->green.msb_right);
	printf("  blue  offset=%u length=%u msb_right=%u\n",
	       v->blue.offset, v->blue.length, v->blue.msb_right);
	printf("  width_mm=%u height_mm=%u\n", v->width, v->height);
	printf("  pixclock=%u left_margin=%u right_margin=%u upper_margin=%u lower_margin=%u\n",
	       v->pixclock, v->left_margin, v->right_margin, v->upper_margin, v->lower_margin);
	printf("  hsync_len=%u vsync_len=%u\n", v->hsync_len, v->vsync_len);
}

static void print_finfo(const struct fb_fix_screeninfo *f)
{
	printf("FBIOGET_FSCREENINFO:\n");
	printf("  id=\"%.16s\"\n", f->id);
	printf("  smem_start=0x%08lx smem_len=%u\n", f->smem_start, f->smem_len);
	printf("  line_length=%u type=%u visual=%u\n", f->line_length, f->type, f->visual);
}

static void print_ark_display(void)
{
	int fd = open("/dev/ark_display", O_RDWR);
	struct ark_screen_info info;

	if (fd < 0) {
		printf("/dev/ark_display: open failed: %s\n", strerror(errno));
		return;
	}
	memset(&info, 0, sizeof(info));
	if (ioctl(fd, ARKDISP_GET_SCREEN_INFO, &info) < 0) {
		printf("/dev/ark_display: ARKDISP_GET_SCREEN_INFO ioctl failed: %s\n", strerror(errno));
		close(fd);
		return;
	}
	printf("/dev/ark_display ARKDISP_GET_SCREEN_INFO:\n");
	printf("  screen_id=%u width_px=%u height_px=%u mm_width=%u mm_height=%u\n",
	       info.screen_id, info.width_px, info.height_px, info.mm_width, info.mm_height);
	close(fd);
}

static int cmd_info(void)
{
	int fd = open("/dev/fb0", O_RDWR);
	struct fb_var_screeninfo vinfo;
	struct fb_fix_screeninfo finfo;

	if (fd < 0) {
		fprintf(stderr, "open /dev/fb0 failed: %s\n", strerror(errno));
		return 1;
	}
	if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
		fprintf(stderr, "FBIOGET_VSCREENINFO failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	if (ioctl(fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
		fprintf(stderr, "FBIOGET_FSCREENINFO failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	print_vinfo(&vinfo);
	printf("\n");
	print_finfo(&finfo);
	printf("\n");
	print_ark_display();
	close(fd);
	return 0;
}

/* Maps an fb_bitfield (offset/length within the pixel) to a packed pixel
 * value for an 8-bit channel value -- works for any bpp/field layout
 * FBIOGET_VSCREENINFO reports, no hardcoded RGB565/888 assumption. */
static unsigned int pack_channel(unsigned char val8, const struct fb_bitfield *f)
{
	unsigned int v = f->length >= 8 ? val8 : (val8 >> (8 - f->length));
	return v << f->offset;
}

static unsigned int pack_rgb(const struct fb_var_screeninfo *v, unsigned char r,
			      unsigned char g, unsigned char b)
{
	return pack_channel(r, &v->red) | pack_channel(g, &v->green) | pack_channel(b, &v->blue);
}

static void put_pixel(unsigned char *fbmem, const struct fb_var_screeninfo *v,
		       const struct fb_fix_screeninfo *f, int x, int y, unsigned int pixel)
{
	int bypp = v->bits_per_pixel / 8;
	unsigned char *p = fbmem + y * f->line_length + x * bypp;

	memcpy(p, &pixel, bypp);
}

static int open_fb(int *out_fd, unsigned char **out_mem, struct fb_var_screeninfo *v,
		    struct fb_fix_screeninfo *f)
{
	int fd = open("/dev/fb0", O_RDWR);
	unsigned char *mem;

	if (fd < 0) {
		fprintf(stderr, "open /dev/fb0 failed: %s\n", strerror(errno));
		return -1;
	}
	if (ioctl(fd, FBIOGET_VSCREENINFO, v) < 0 || ioctl(fd, FBIOGET_FSCREENINFO, f) < 0) {
		fprintf(stderr, "FBIOGET_*SCREENINFO failed: %s\n", strerror(errno));
		close(fd);
		return -1;
	}
	if (v->bits_per_pixel != 16 && v->bits_per_pixel != 24 && v->bits_per_pixel != 32) {
		fprintf(stderr, "unsupported bits_per_pixel=%u\n", v->bits_per_pixel);
		close(fd);
		return -1;
	}

	/* Force the visible page to (0,0) so our writes are guaranteed to
	 * land where they're actually displayed, regardless of whatever page
	 * a compositor/QWS server last panned to. If panning isn't supported
	 * by this driver, fall through and rely on writing at the current offset. */
	if (v->xoffset != 0 || v->yoffset != 0) {
		struct fb_var_screeninfo pv = *v;
		pv.xoffset = 0;
		pv.yoffset = 0;
		if (ioctl(fd, FBIOPAN_DISPLAY, &pv) == 0) {
			v->xoffset = 0;
			v->yoffset = 0;
		}
	}

	mem = malloc(f->smem_len);
	if (!mem) {
		fprintf(stderr, "malloc local fb buffer failed\n");
		close(fd);
		return -1;
	}
	memset(mem, 0, f->smem_len);

	*out_fd = fd;
	*out_mem = mem;
	return 0;
}

static void close_fb(int fd, unsigned char *mem, const struct fb_var_screeninfo *v, const struct fb_fix_screeninfo *f)
{
	int bypp = v->bits_per_pixel / 8;
	lseek(fd, v->yoffset * f->line_length + v->xoffset * bypp, SEEK_SET);
	write(fd, mem, f->line_length * v->yres);
	free(mem);
	close(fd);
}

static int cmd_fill(const char *colorspec)
{
	int fd;
	unsigned char *mem;
	struct fb_var_screeninfo v;
	struct fb_fix_screeninfo f;
	unsigned char r, g, b;
	unsigned int pixel;
	int x, y;

	if (strcmp(colorspec, "red") == 0) { r = 255; g = 0; b = 0; }
	else if (strcmp(colorspec, "green") == 0) { r = 0; g = 255; b = 0; }
	else if (strcmp(colorspec, "blue") == 0) { r = 0; g = 0; b = 255; }
	else if (strcmp(colorspec, "white") == 0) { r = 255; g = 255; b = 255; }
	else if (strcmp(colorspec, "black") == 0) { r = 0; g = 0; b = 0; }
	else if (sscanf(colorspec, "%hhu,%hhu,%hhu", &r, &g, &b) != 3) {
		fprintf(stderr, "unknown color '%s' (use red/green/blue/white/black or r,g,b)\n", colorspec);
		return 1;
	}

	if (open_fb(&fd, &mem, &v, &f) < 0)
		return 1;

	pixel = pack_rgb(&v, r, g, b);
	for (y = 0; y < (int)v.yres; y++)
		for (x = 0; x < (int)v.xres; x++)
			put_pixel(mem, &v, &f, x, y, pixel);

	printf("Filled %ux%u @ %ubpp with (%u,%u,%u)\n", v.xres, v.yres, v.bits_per_pixel, r, g, b);
	close_fb(fd, mem, &v, &f);
	return 0;
}

static int cmd_bars(void)
{
	int fd;
	unsigned char *mem;
	struct fb_var_screeninfo v;
	struct fb_fix_screeninfo f;
	static const unsigned char bars[][3] = {
		{255,255,255}, {255,255,0}, {0,255,255}, {0,255,0},
		{255,0,255}, {255,0,0}, {0,0,255}, {0,0,0},
	};
	int nbars = sizeof(bars) / sizeof(bars[0]);
	int x, y;

	if (open_fb(&fd, &mem, &v, &f) < 0)
		return 1;

	for (x = 0; x < (int)v.xres; x++) {
		int bar = x * nbars / v.xres;
		unsigned int pixel = pack_rgb(&v, bars[bar][0], bars[bar][1], bars[bar][2]);
		for (y = 0; y < (int)v.yres; y++)
			put_pixel(mem, &v, &f, x, y, pixel);
	}

	printf("Drew %d-bar color pattern on %ux%u @ %ubpp\n", nbars, v.xres, v.yres, v.bits_per_pixel);
	close_fb(fd, mem, &v, &f);
	return 0;
}

static int cmd_gradient(void)
{
	int fd;
	unsigned char *mem;
	struct fb_var_screeninfo v;
	struct fb_fix_screeninfo f;
	int x, y;

	if (open_fb(&fd, &mem, &v, &f) < 0)
		return 1;

	for (x = 0; x < (int)v.xres; x++) {
		unsigned char r = (unsigned char)(255 * x / (int)v.xres);
		unsigned char g = (unsigned char)(255 - r);
		unsigned int pixel = pack_rgb(&v, r, g, 0);
		for (y = 0; y < (int)v.yres; y++)
			put_pixel(mem, &v, &f, x, y, pixel);
	}

	printf("Drew red->green gradient on %ux%u @ %ubpp\n", v.xres, v.yres, v.bits_per_pixel);
	close_fb(fd, mem, &v, &f);
	return 0;
}

static int cmd_run(void)
{
	int fd;
	unsigned char *mem;
	struct fb_var_screeninfo v;
	struct fb_fix_screeninfo f;
	int x, y;
	unsigned int pixel;
	int bypp;

	/* 1. Output the info message first */
	cmd_info();
	printf("\nStarting automated LCD test sequence...\n");

	if (open_fb(&fd, &mem, &v, &f) < 0)
		return 1;

	bypp = v.bits_per_pixel / 8;

	/* --- Phase 1: Random Noise --- */
	printf("1. Displaying random noise (2 seconds)...\n");
	for (y = 0; y < (int)v.yres; y++) {
		for (x = 0; x < (int)v.xres; x++) {
			unsigned char r = rand() % 256;
			unsigned char g = rand() % 256;
			unsigned char b = rand() % 256;
			pixel = pack_rgb(&v, r, g, b);
			put_pixel(mem, &v, &f, x, y, pixel);
		}
	}
	lseek(fd, v.yoffset * f.line_length + v.xoffset * bypp, SEEK_SET);
	if (write(fd, mem, f.line_length * v.yres) < 0) {
		fprintf(stderr, "write failed\n");
	}
	sleep(2);

	/* --- Phase 2: Solid Colors --- */
	static const struct {
		const char *name;
		unsigned char r, g, b;
	} colors[] = {
		{"Red", 255, 0, 0},
		{"Green", 0, 255, 0},
		{"Blue", 0, 0, 255},
		{"White", 255, 255, 255},
		{"Black", 0, 0, 0}
	};
	int ncolors = sizeof(colors) / sizeof(colors[0]);
	int i;
	for (i = 0; i < ncolors; i++) {
		printf("2.%d Displaying solid %s (1 second)...\n", i + 1, colors[i].name);
		pixel = pack_rgb(&v, colors[i].r, colors[i].g, colors[i].b);
		for (y = 0; y < (int)v.yres; y++)
			for (x = 0; x < (int)v.xres; x++)
				put_pixel(mem, &v, &f, x, y, pixel);
		lseek(fd, v.yoffset * f.line_length + v.xoffset * bypp, SEEK_SET);
		if (write(fd, mem, f.line_length * v.yres) < 0) {
			fprintf(stderr, "write failed\n");
		}
		sleep(1);
	}

	/* --- Phase 3: Color Bars --- */
	printf("3. Displaying vertical color bars (2 seconds)...\n");
	static const unsigned char bars[][3] = {
		{255,255,255}, {255,255,0}, {0,255,255}, {0,255,0},
		{255,0,255}, {255,0,0}, {0,0,255}, {0,0,0},
	};
	int nbars = sizeof(bars) / sizeof(bars[0]);
	for (x = 0; x < (int)v.xres; x++) {
		int bar = x * nbars / v.xres;
		pixel = pack_rgb(&v, bars[bar][0], bars[bar][1], bars[bar][2]);
		for (y = 0; y < (int)v.yres; y++)
			put_pixel(mem, &v, &f, x, y, pixel);
	}
	lseek(fd, v.yoffset * f.line_length + v.xoffset * bypp, SEEK_SET);
	if (write(fd, mem, f.line_length * v.yres) < 0) {
		fprintf(stderr, "write failed\n");
	}
	sleep(2);

	/* --- Phase 4: Red-to-Green Gradient --- */
	printf("4. Displaying red-to-green gradient (2 seconds)...\n");
	for (x = 0; x < (int)v.xres; x++) {
		unsigned char r = (unsigned char)(255 * x / (int)v.xres);
		unsigned char g = (unsigned char)(255 - r);
		pixel = pack_rgb(&v, r, g, 0);
		for (y = 0; y < (int)v.yres; y++)
			put_pixel(mem, &v, &f, x, y, pixel);
	}
	lseek(fd, v.yoffset * f.line_length + v.xoffset * bypp, SEEK_SET);
	if (write(fd, mem, f.line_length * v.yres) < 0) {
		fprintf(stderr, "write failed\n");
	}
	sleep(2);

	/* --- Clear Screen to Black --- */
	pixel = pack_rgb(&v, 0, 0, 0);
	for (y = 0; y < (int)v.yres; y++)
		for (x = 0; x < (int)v.xres; x++)
			put_pixel(mem, &v, &f, x, y, pixel);
	lseek(fd, v.yoffset * f.line_length + v.xoffset * bypp, SEEK_SET);
	if (write(fd, mem, f.line_length * v.yres) < 0) {
		fprintf(stderr, "write failed\n");
	}

	free(mem);
	close(fd);
	printf("LCD test sequence complete.\n");
	return 0;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [run|test]                     - print info and cycle through all test patterns\n"
		"       %s info                           - print framebuffer and display info\n"
		"       %s fill <red|green|blue|...|r,g,b> - fill screen with solid color\n"
		"       %s bars                           - draw color-bars pattern\n"
		"       %s gradient                       - draw red->green gradient\n",
		argv0, argv0, argv0, argv0, argv0);
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		return cmd_run();
	}
	if (strcmp(argv[1], "run") == 0 || strcmp(argv[1], "test") == 0)
		return cmd_run();
	if (strcmp(argv[1], "info") == 0)
		return cmd_info();
	if (strcmp(argv[1], "fill") == 0 && argc >= 3)
		return cmd_fill(argv[2]);
	if (strcmp(argv[1], "bars") == 0)
		return cmd_bars();
	if (strcmp(argv[1], "gradient") == 0)
		return cmd_gradient();

	usage(argv[0]);
	return 1;
}
