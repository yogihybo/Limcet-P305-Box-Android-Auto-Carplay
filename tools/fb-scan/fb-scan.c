/*
 * fb-scan -- locate near-black pixel regions in the live /dev/fb0
 * framebuffer, report their exact (x,y,w,h) plus raw pixel bytes, and
 * for each one predict what color it should actually be by fitting a
 * linear ramp across the other solid-color cells in the same grid row.
 *
 * Why: LCDTest -qws's color-test-pattern grid renders two cells (in the
 * red intensity-ramp row) as solid black on our build, deterministically,
 * while the same binary is correct on stock hardware. Every register-
 * level theory chased so far (rgb_order, rgb_ycbcr_bypass, colorkey/
 * threshold, blend mode) has been static analysis against source/
 * decompiles -- none confirmed against what's actually IN the
 * framebuffer at the moment of the bug. This tool reads the real,
 * currently-displayed pixel data directly (mmap()'d /dev/fb0, not
 * /dev/mem -- fbdev's own mmap works fine for its own memory, unlike
 * the DMA carve-out case mem-dump.c had to work around).
 *
 * The expected-color prediction deliberately does NOT hardcode the
 * ramp's pixel geometry from the LCDTest disassembly (the QRect layout
 * is computed at runtime from the actual widget size via several
 * chained float ops -- fragile to reproduce exactly and easy to get
 * subtly wrong). Instead it fits a line through the *other* cells
 * actually rendered in the same row on THIS run, which is self-
 * consistent by construction and works regardless of window size. The
 * one piece of static ground truth used is a sanity note only: LCDTest's
 * ramp rows are confirmed (paintEvent disassembly, ~0x13398-0x133b4) to
 * step in exactly floor(k*255/10) for k=0,2,4,6,8, with k=10 forced to
 * 255 -- i.e. six discrete stops {0,51,102,153,204,255}. If a predicted
 * value lands far from that set, treat the prediction itself with
 * suspicion rather than as ground truth.
 *
 * Usage: fb-scan [threshold]
 *   threshold: max per-channel R/G/B value (0-255) still counted as
 *              "black", default 4 (near-zero, allows for minor GPU/
 *              blend rounding noise). Same threshold also used as the
 *              per-channel tolerance for "same solid color" grouping.
 *
 * Run with LCDTest -qws already on screen showing the bug:
 *   QWS_DISPLAY=directfb:... LCDTest -qws &
 *   ./fb-scan
 *
 * Output, one line per detected solid-color rectangle >= 25px^2:
 *   COLOR x=<x> y=<y> w=<w> h=<h> px=0x<raw pixel> r=<r> g=<g> b=<b>
 *   BLACK x=<x> y=<y> w=<w> h=<h> px=0x<raw pixel>
 *     [EXPECTED r=<r> g=<g> b=<b> (fit from N cells in same row, dominant=R/G/B)]
 *     [no row fit available: <reason>]
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

struct rect {
	unsigned x, y, w, h;
	unsigned int px;
	unsigned char r, g, b;
	int is_black;
};

#define MAX_RECTS 4096

static struct rect rects[MAX_RECTS];
static int nrects = 0;

int main(int argc, char **argv)
{
	unsigned char threshold = (argc >= 2) ? (unsigned char)strtoul(argv[1], NULL, 0) : 4;

	int fd = open("/dev/fb0", O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "open /dev/fb0: %s\n", strerror(errno));
		return 1;
	}

	struct fb_var_screeninfo v;
	struct fb_fix_screeninfo f;
	if (ioctl(fd, FBIOGET_VSCREENINFO, &v) < 0 || ioctl(fd, FBIOGET_FSCREENINFO, &f) < 0) {
		fprintf(stderr, "FBIOGET_*SCREENINFO: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	if (v.bits_per_pixel != 32) {
		fprintf(stderr, "bits_per_pixel=%u, this tool only handles 32bpp\n",
			v.bits_per_pixel);
		close(fd);
		return 1;
	}

	fprintf(stderr,
		"fb0: %ux%u bpp=%u line_length=%u smem_start=0x%lx smem_len=%u "
		"red(off=%u,len=%u) green(off=%u,len=%u) blue(off=%u,len=%u) threshold=%u\n",
		v.xres, v.yres, v.bits_per_pixel, f.line_length,
		(unsigned long)f.smem_start, f.smem_len,
		v.red.offset, v.red.length, v.green.offset, v.green.length,
		v.blue.offset, v.blue.length, threshold);

	size_t map_len = (size_t)f.line_length * v.yres;
	unsigned char *mem = mmap(NULL, map_len, PROT_READ, MAP_SHARED, fd, 0);
	if (mem == MAP_FAILED) {
		fprintf(stderr, "mmap(/dev/fb0): %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	unsigned char *visited = calloc(v.xres, v.yres);
	if (!visited) {
		fprintf(stderr, "calloc failed\n");
		return 1;
	}

	for (unsigned y = 0; y < v.yres; y++) {
		for (unsigned x = 0; x < v.xres; x++) {
			if (visited[y * v.xres + x])
				continue;

			unsigned char *p = mem + (size_t)y * f.line_length + (size_t)x * 4;
			unsigned int px;
			memcpy(&px, p, 4);
			unsigned char r = (px >> v.red.offset) & 0xff;
			unsigned char g = (px >> v.green.offset) & 0xff;
			unsigned char b = (px >> v.blue.offset) & 0xff;

#define CLOSE(a, b) (((a) > (b) ? (a) - (b) : (b) - (a)) <= threshold)

			/* grow the maximal rectangle of pixels matching this anchor's
			 * color within tolerance: width along the row, then height
			 * while the whole [x, x+w) span keeps matching */
			unsigned w = 1;
			while (x + w < v.xres) {
				unsigned char *pw = mem + (size_t)y * f.line_length + (size_t)(x + w) * 4;
				unsigned int pxw;
				memcpy(&pxw, pw, 4);
				unsigned char rw = (pxw >> v.red.offset) & 0xff;
				unsigned char gw = (pxw >> v.green.offset) & 0xff;
				unsigned char bw = (pxw >> v.blue.offset) & 0xff;
				if (!CLOSE(rw, r) || !CLOSE(gw, g) || !CLOSE(bw, b))
					break;
				w++;
			}

			unsigned h = 1;
			while (y + h < v.yres) {
				int row_ok = 1;
				for (unsigned xi = x; xi < x + w; xi++) {
					unsigned char *ph = mem + (size_t)(y + h) * f.line_length + (size_t)xi * 4;
					unsigned int pxh;
					memcpy(&pxh, ph, 4);
					unsigned char rh = (pxh >> v.red.offset) & 0xff;
					unsigned char gh = (pxh >> v.green.offset) & 0xff;
					unsigned char bh = (pxh >> v.blue.offset) & 0xff;
					if (!CLOSE(rh, r) || !CLOSE(gh, g) || !CLOSE(bh, b)) {
						row_ok = 0;
						break;
					}
				}
				if (!row_ok)
					break;
				h++;
			}

			for (unsigned yi = y; yi < y + h; yi++)
				for (unsigned xi = x; xi < x + w; xi++)
					visited[yi * v.xres + xi] = 1;

			/* ignore tiny/noise blobs (e.g. anti-aliased text descenders) */
			if (w * h >= 25 && nrects < MAX_RECTS) {
				struct rect *rc = &rects[nrects++];
				rc->x = x; rc->y = y; rc->w = w; rc->h = h;
				rc->px = px; rc->r = r; rc->g = g; rc->b = b;
				rc->is_black = (r <= threshold && g <= threshold && b <= threshold);
			}
		}
	}

	munmap(mem, map_len);
	close(fd);
	free(visited);

	for (int i = 0; i < nrects; i++) {
		struct rect *rc = &rects[i];
		if (!rc->is_black) {
			printf("COLOR x=%u y=%u w=%u h=%u px=0x%08x r=%u g=%u b=%u\n",
			       rc->x, rc->y, rc->w, rc->h, rc->px, rc->r, rc->g, rc->b);
			continue;
		}

		printf("BLACK x=%u y=%u w=%u h=%u px=0x%08x\n", rc->x, rc->y, rc->w, rc->h, rc->px);

		/* gather COLOR cells in the same row: y-ranges overlapping this
		 * cell's by at least half its height, similar height (+/-50%) */
		unsigned cy0 = rc->y, cy1 = rc->y + rc->h;
		double sum_x = 0, sum_x2 = 0, sum_r = 0, sum_g = 0, sum_b = 0;
		double sum_xr = 0, sum_xg = 0, sum_xb = 0;
		int n = 0;

		for (int j = 0; j < nrects; j++) {
			struct rect *o = &rects[j];
			if (o->is_black)
				continue;
			unsigned oy0 = o->y, oy1 = o->y + o->h;
			unsigned ov_lo = cy0 > oy0 ? cy0 : oy0;
			unsigned ov_hi = cy1 < oy1 ? cy1 : oy1;
			if (ov_hi <= ov_lo)
				continue;
			unsigned overlap = ov_hi - ov_lo;
			if (overlap * 2 < rc->h)
				continue;
			if (o->h < rc->h / 2 || o->h > rc->h * 2)
				continue;

			double cx = o->x + o->w / 2.0;
			sum_x += cx; sum_x2 += cx * cx;
			sum_r += o->r; sum_g += o->g; sum_b += o->b;
			sum_xr += cx * o->r; sum_xg += cx * o->g; sum_xb += cx * o->b;
			n++;
		}

		if (n < 2) {
			printf("  no row fit available: only %d color cell(s) found in this row\n", n);
			continue;
		}

		double target_x = rc->x + rc->w / 2.0;
		double denom = n * sum_x2 - sum_x * sum_x;
		double pr, pg, pb;

		if (denom == 0) {
			/* all neighbor cells share one x (shouldn't happen) -- fall back to mean */
			pr = sum_r / n; pg = sum_g / n; pb = sum_b / n;
		} else {
			double slope_r = (n * sum_xr - sum_x * sum_r) / denom;
			double icpt_r = (sum_r - slope_r * sum_x) / n;
			double slope_g = (n * sum_xg - sum_x * sum_g) / denom;
			double icpt_g = (sum_g - slope_g * sum_x) / n;
			double slope_b = (n * sum_xb - sum_x * sum_b) / denom;
			double icpt_b = (sum_b - slope_b * sum_x) / n;
			pr = slope_r * target_x + icpt_r;
			pg = slope_g * target_x + icpt_g;
			pb = slope_b * target_x + icpt_b;
		}

		if (pr < 0) pr = 0;
		if (pr > 255) pr = 255;
		if (pg < 0) pg = 0;
		if (pg > 255) pg = 255;
		if (pb < 0) pb = 0;
		if (pb > 255) pb = 255;

		const char *dominant = "?";
		double range_r = 0, range_g = 0, range_b = 0;
		{
			double minr = 999, maxr = -1, ming = 999, maxg = -1, minb = 999, maxb = -1;
			for (int j = 0; j < nrects; j++) {
				struct rect *o = &rects[j];
				if (o->is_black) continue;
				unsigned oy0 = o->y, oy1 = o->y + o->h;
				unsigned ov_lo = cy0 > oy0 ? cy0 : oy0;
				unsigned ov_hi = cy1 < oy1 ? cy1 : oy1;
				if (ov_hi <= ov_lo || (ov_hi - ov_lo) * 2 < rc->h) continue;
				if (o->r < minr) minr = o->r;
				if (o->r > maxr) maxr = o->r;
				if (o->g < ming) ming = o->g;
				if (o->g > maxg) maxg = o->g;
				if (o->b < minb) minb = o->b;
				if (o->b > maxb) maxb = o->b;
			}
			range_r = maxr - minr; range_g = maxg - ming; range_b = maxb - minb;
			if (range_r >= range_g && range_r >= range_b) dominant = "R";
			else if (range_g >= range_b) dominant = "G";
			else dominant = "B";
		}

		printf("  EXPECTED r=%.0f g=%.0f b=%.0f (linear fit from %d cell(s) in same row, "
		       "dominant=%s)\n", pr, pg, pb, n, dominant);
	}

	return 0;
}
