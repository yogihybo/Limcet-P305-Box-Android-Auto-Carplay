/*
 * lcd-test — live LCD/framebuffer diagnostic tool, same purpose/style as
 * tools/i2c-scan and tools/ark1680-ts-test: a static ARM binary to run at
 * the live root shell, testing the raw kernel framebuffer (/dev/fb0)
 * directly -- no Qt/QWS server involved, so it works even though
 * MsnCoreApp/LCDTest -qws currently segfault (see
 * docs/ARK1680_TS_REVERSE_ENGINEERING.md "MsnCoreApp segfault").
 *
 * No arguments, no subcommands -- run it and it dumps /dev/fb0 and
 * /dev/ark_display's reported info to the console, then cycles through
 * solid-color fills, a color-bar pattern, and a gradient, pausing between
 * each and printing what it just drew so the whole sequence can be
 * watched on the panel while reading the console log alongside it.
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

#define STEP_DELAY_SEC	2

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
	/* Must account for the currently panned page (xoffset/yoffset) -- on a
	 * double/triple-buffered fb (yres_virtual > yres), the visible page
	 * is not necessarily at mmap offset 0. Writing without this offset
	 * silently lands on an off-screen back buffer: the write succeeds,
	 * the tool reports success, but nothing appears on the panel. This
	 * was a real bug here (2026-07-14) -- open_fb() now force-pans to
	 * (0,0) so v->xoffset/v->yoffset are 0 by the time we get here, but
	 * keep using them (not literal 0) in case a future caller skips that
	 * step or panning isn't supported and the fallback below is hit. */
	unsigned char *p = fbmem + (y + v->yoffset) * f->line_length + (x + v->xoffset) * bypp;

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
	/* Follow the same init step the stock LCDTest/Qt/DirectFB stack takes
	 * (docs/DISPLAY_SUBSYSTEM.md's 2026-07-16 milestone entry): DirectFB's
	 * fbdev system module applies its chosen mode via FBIOPUT_VSCREENINFO,
	 * which is what actually invokes the kernel driver's .fb_set_par hook
	 * (ark1668_lcdfb_set_par()) -- the function that programs *and enables*
	 * the OSD1 display layer (ARK1668_LCDC_OSD1_CTL, the EN bit). It runs
	 * once at kernel probe time too, so in principle OSD1 should already
	 * be enabled by the time this tool runs -- but this tool's own writes
	 * were never confirmed visible even after the panning fix below, so
	 * re-issuing FBIOPUT_VSCREENINFO here (with the info we just read back
	 * -- not changing the mode, just re-applying it) cheaply rules out any
	 * state where OSD1 ended up disabled/never-enabled by the time we get
	 * here, exactly mirroring what the one userspace path known to work
	 * (Qt/DirectFB) actually does that a bare open()+mmap() does not. */
	if (ioctl(fd, FBIOPUT_VSCREENINFO, v) < 0) {
		fprintf(stderr,
			"warning: FBIOPUT_VSCREENINFO failed: %s -- continuing anyway, "
			"but this is the step that re-triggers the kernel driver's "
			"OSD1 layer enable, so writes may still be invisible\n",
			strerror(errno));
	}
	/* Force the visible page to (0,0) so our writes are guaranteed to
	 * land where they're actually displayed, regardless of whatever page
	 * a compositor/QWS server last panned to. If panning isn't supported
	 * by this driver, fall through and rely on put_pixel()'s use of the
	 * offset actually reported by FBIOGET_VSCREENINFO instead. */
	if (v->xoffset != 0 || v->yoffset != 0) {
		struct fb_var_screeninfo pv = *v;
		pv.xoffset = 0;
		pv.yoffset = 0;
		if (ioctl(fd, FBIOPAN_DISPLAY, &pv) == 0) {
			v->xoffset = 0;
			v->yoffset = 0;
		} else {
			fprintf(stderr,
				"warning: fb reports xoffset=%u yoffset=%u (panned/"
				"double-buffered) and FBIOPAN_DISPLAY to (0,0) failed: %s "
				"-- writing at the current offset instead, but if something "
				"else pans the display afterwards this write may become "
				"invisible again\n",
				v->xoffset, v->yoffset, strerror(errno));
		}
	}
	mem = mmap(NULL, f->smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (mem == MAP_FAILED) {
		fprintf(stderr, "mmap /dev/fb0 failed: %s\n", strerror(errno));
		close(fd);
		return -1;
	}
	*out_fd = fd;
	*out_mem = mem;
	return 0;
}

static void draw_fill(unsigned char *mem, const struct fb_var_screeninfo *v,
		       const struct fb_fix_screeninfo *f,
		       unsigned char r, unsigned char g, unsigned char b, const char *name)
{
	unsigned int pixel = pack_rgb(v, r, g, b);
	int x, y;

	for (y = 0; y < (int)v->yres; y++)
		for (x = 0; x < (int)v->xres; x++)
			put_pixel(mem, v, f, x, y, pixel);

	printf("Filled %ux%u @ %ubpp with %s (%u,%u,%u)\n",
	       v->xres, v->yres, v->bits_per_pixel, name, r, g, b);
}

static void draw_bars(unsigned char *mem, const struct fb_var_screeninfo *v,
		       const struct fb_fix_screeninfo *f)
{
	static const unsigned char bars[][3] = {
		{255,255,255}, {255,255,0}, {0,255,255}, {0,255,0},
		{255,0,255}, {255,0,0}, {0,0,255}, {0,0,0},
	};
	int nbars = sizeof(bars) / sizeof(bars[0]);
	int x, y;

	for (x = 0; x < (int)v->xres; x++) {
		int bar = x * nbars / v->xres;
		unsigned int pixel = pack_rgb(v, bars[bar][0], bars[bar][1], bars[bar][2]);
		for (y = 0; y < (int)v->yres; y++)
			put_pixel(mem, v, f, x, y, pixel);
	}

	printf("Drew %d-bar color pattern on %ux%u @ %ubpp\n",
	       nbars, v->xres, v->yres, v->bits_per_pixel);
}

static void draw_gradient(unsigned char *mem, const struct fb_var_screeninfo *v,
			   const struct fb_fix_screeninfo *f)
{
	int x, y;

	for (x = 0; x < (int)v->xres; x++) {
		unsigned char r = (unsigned char)(255 * x / (int)v->xres);
		unsigned char g = (unsigned char)(255 - r);
		unsigned int pixel = pack_rgb(v, r, g, 0);
		for (y = 0; y < (int)v->yres; y++)
			put_pixel(mem, v, f, x, y, pixel);
	}

	printf("Drew red->green gradient on %ux%u @ %ubpp\n",
	       v->xres, v->yres, v->bits_per_pixel);
}

int main(void)
{
	int fd;
	unsigned char *mem;
	struct fb_var_screeninfo v;
	struct fb_fix_screeninfo f;
	static const struct { unsigned char r, g, b; const char *name; } colors[] = {
		{255, 0, 0, "red"}, {0, 255, 0, "green"}, {0, 0, 255, "blue"},
		{255, 255, 255, "white"}, {0, 0, 0, "black"},
	};
	int i;

	{
		int info_fd = open("/dev/fb0", O_RDWR);
		struct fb_var_screeninfo vi;
		struct fb_fix_screeninfo fi;

		if (info_fd < 0) {
			fprintf(stderr, "open /dev/fb0 failed: %s\n", strerror(errno));
			return 1;
		}
		if (ioctl(info_fd, FBIOGET_VSCREENINFO, &vi) < 0 ||
		    ioctl(info_fd, FBIOGET_FSCREENINFO, &fi) < 0) {
			fprintf(stderr, "FBIOGET_*SCREENINFO failed: %s\n", strerror(errno));
			close(info_fd);
			return 1;
		}
		print_vinfo(&vi);
		printf("\n");
		print_finfo(&fi);
		printf("\n");
		print_ark_display();
		printf("\n");
		close(info_fd);
	}

	if (open_fb(&fd, &mem, &v, &f) < 0)
		return 1;

	for (i = 0; i < (int)(sizeof(colors) / sizeof(colors[0])); i++) {
		draw_fill(mem, &v, &f, colors[i].r, colors[i].g, colors[i].b, colors[i].name);
		sleep(STEP_DELAY_SEC);
	}

	draw_bars(mem, &v, &f);
	sleep(STEP_DELAY_SEC);

	draw_gradient(mem, &v, &f);
	sleep(STEP_DELAY_SEC);

	printf("Done.\n");
	munmap(mem, f.smem_len);
	close(fd);
	return 0;
}
