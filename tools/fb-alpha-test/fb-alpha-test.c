/*
 * fb-alpha-test -- empirically determine how the LCDC's OSD1 layer
 * actually treats the alpha byte of each 32bpp pixel, and whether the
 * kernel's reported fb_var_screeninfo channel layout (red/green/blue/
 * transp offset+length) matches what the hardware really scans out.
 *
 * Why this exists: MsnCoreApp shows a red tint on UI elements (icons,
 * anti-aliased/alpha-blended widgets) but NOT on the flat opaque
 * background -- see docs/DISPLAY_SUBSYSTEM.md. A live register test
 * (clearing bit 17 of ARK1668_LCDC_OSD1_CTL, the suspected per-pixel-
 * alpha-enable bit) turned the *entire* screen green instead of only
 * affecting blended elements, ruling out a simple register-level fix --
 * any hardware-register change is necessarily global to the whole OSD1
 * layer, but the real symptom is selective. That points at a software/
 * pixel-data-level mismatch (Qt's alpha-blending math assuming a channel
 * layout the hardware doesn't actually implement), not a register bug.
 * This tool paints known (R,G,B,A) values directly into /dev/fb0 --
 * both via the kernel's own reported field offsets, AND as raw literal
 * bytes ignoring those offsets -- so what actually appears on the panel
 * can be compared directly against what was written, with no inference.
 *
 * Reuses the open_fb()/put_pixel() approach already proven working in
 * tools/lcd-test/lcd-test.c (FBIOPUT_VSCREENINFO re-apply to guarantee
 * OSD1 is enabled, FBIOPAN_DISPLAY to (0,0) so writes land on the
 * visible page). MsnCoreApp/any other QWS server must be stopped first
 * (killall MsnCoreApp) or it will simply repaint over this within a
 * frame or two, same caveat as lcd-test.
 *
 * No arguments. Draws 6 labeled bands top-to-bottom and prints exactly
 * what each one should look like, so the on-screen result can be
 * compared by eye against the printed expectation.
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

static unsigned int pack_channel(unsigned char val8, const struct fb_bitfield *f)
{
	unsigned int v = f->length >= 8 ? val8 : (val8 >> (8 - f->length));
	return v << f->offset;
}

/* Packs using the kernel's own reported red/green/blue/transp field
 * offsets -- what Qt itself would do if it trusted FBIOGET_VSCREENINFO. */
static unsigned int pack_argb_reported(const struct fb_var_screeninfo *v,
					unsigned char r, unsigned char g,
					unsigned char b, unsigned char a)
{
	unsigned int p = pack_channel(r, &v->red) | pack_channel(g, &v->green) |
			  pack_channel(b, &v->blue);
	if (v->transp.length)
		p |= pack_channel(a, &v->transp);
	return p;
}

static void put_pixel(unsigned char *fbmem, const struct fb_var_screeninfo *v,
		       const struct fb_fix_screeninfo *f, int x, int y, unsigned int pixel)
{
	int bypp = v->bits_per_pixel / 8;
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
	if (v->bits_per_pixel != 32) {
		fprintf(stderr,
			"bits_per_pixel=%u, not 32 -- this tool only handles 32bpp "
			"(the RGBA888 format the LCDC OSD1 layer is configured for)\n",
			v->bits_per_pixel);
		close(fd);
		return -1;
	}
	if (ioctl(fd, FBIOPUT_VSCREENINFO, v) < 0)
		fprintf(stderr, "warning: FBIOPUT_VSCREENINFO failed: %s\n", strerror(errno));

	if (v->xoffset != 0 || v->yoffset != 0) {
		struct fb_var_screeninfo pv = *v;
		pv.xoffset = 0;
		pv.yoffset = 0;
		if (ioctl(fd, FBIOPAN_DISPLAY, &pv) == 0) {
			v->xoffset = 0;
			v->yoffset = 0;
		} else {
			fprintf(stderr, "warning: FBIOPAN_DISPLAY to (0,0) failed: %s\n",
				strerror(errno));
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

static void fill_band(unsigned char *mem, const struct fb_var_screeninfo *v,
		       const struct fb_fix_screeninfo *f, int band, int nbands,
		       unsigned int pixel, const char *label)
{
	int y0 = (int)v->yres * band / nbands;
	int y1 = (int)v->yres * (band + 1) / nbands;
	int x, y;

	for (y = y0; y < y1; y++)
		for (x = 0; x < (int)v->xres; x++)
			put_pixel(mem, v, f, x, y, pixel);

	printf("Band %d (y=%d..%d): pixel=0x%08x -- %s\n", band, y0, y1, pixel, label);
}

int main(void)
{
	int fd;
	unsigned char *mem;
	struct fb_var_screeninfo v;
	struct fb_fix_screeninfo f;

	if (open_fb(&fd, &mem, &v, &f) < 0)
		return 1;

	printf("Kernel-reported field layout: red off=%u len=%u, green off=%u len=%u, "
	       "blue off=%u len=%u, transp off=%u len=%u\n",
	       v.red.offset, v.red.length, v.green.offset, v.green.length,
	       v.blue.offset, v.blue.length, v.transp.offset, v.transp.length);
	if (!v.transp.length)
		printf("NOTE: transp.length=0 -- kernel doesn't report an alpha field at all; "
		       "the raw-byte bands below still test bit 24-31 directly regardless.\n");

	const int NBANDS = 6;

	/* Band 0: opaque mid-gray reference background, alpha=0xff via
	 * reported fields. Sanity check -- should look neutral gray. */
	fill_band(mem, &v, &f, 0, NBANDS,
		  pack_argb_reported(&v, 128, 128, 128, 255),
		  "opaque mid-gray (R128 G128 B128 A255, via reported fields) -- reference");

	/* Band 1: opaque solid red, alpha=0xff. Sanity check for RGB order. */
	fill_band(mem, &v, &f, 1, NBANDS,
		  pack_argb_reported(&v, 255, 0, 0, 255),
		  "opaque red (R255 G0 B0 A255, via reported fields) -- should look pure red");

	/* Band 2: half-alpha red, via reported fields -- the actual bug case:
	 * MsnCoreApp's alpha-blended elements are exactly this kind of pixel. */
	fill_band(mem, &v, &f, 2, NBANDS,
		  pack_argb_reported(&v, 255, 0, 0, 128),
		  "half-alpha red (R255 G0 B0 A128, via reported fields) -- THE bug case");

	/* Band 3: half-alpha green, control comparison against band 2. */
	fill_band(mem, &v, &f, 3, NBANDS,
		  pack_argb_reported(&v, 0, 255, 0, 128),
		  "half-alpha green (R0 G255 B0 A128, via reported fields) -- control");

	/* Band 4: raw literal bytes 0xFF,0x00,0x00,0x80 regardless of what
	 * the kernel claims red/green/blue/transp offsets are -- tests
	 * hardware truth directly. On a little-endian ARM this literal
	 * 0x800000ff means byte0=0xff,byte1=0x00,byte2=0x00,byte3=0x80. */
	fill_band(mem, &v, &f, 4, NBANDS, 0x800000ffu,
		  "RAW bytes [ff,00,00,80] (byte0..byte3), ignoring reported fields entirely");

	/* Band 5: raw literal bytes 0x80,0x00,0x00,0xFF -- the reverse byte
	 * order of band 4, to see which raw arrangement actually produces a
	 * half-intensity/blended-looking red vs which produces something else. */
	fill_band(mem, &v, &f, 5, NBANDS, 0xff000080u,
		  "RAW bytes [80,00,00,ff] (byte0..byte3), ignoring reported fields entirely");

	munmap(mem, f.smem_len);
	close(fd);

	printf("\nDone. Compare each band's actual on-screen color against its label above.\n"
	       "Band 0 should look neutral gray and Band 1 should look pure red if the basic\n"
	       "RGB order is right. Bands 2 vs 3 show whether red specifically misbehaves\n"
	       "under alpha blending vs other colors. Bands 4/5 show the true raw byte-order\n"
	       "behavior regardless of what the kernel claims.\n");
	return 0;
}
