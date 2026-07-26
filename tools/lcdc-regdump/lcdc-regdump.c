/*
 * lcdc-regdump -- dumps every named ARK1668 LCDC register (by name, not
 * raw offset) so a stock-vs-our-build run can be diffed directly.
 *
 * Why: every static theory chased for the deterministic LCDTest black-
 * cell bug (rgb_order, rgb_ycbcr_bypass, colorkey/threshold, blend mode,
 * gamma) has been ruled out by source/decompile comparison without
 * finding the actual cause. The stock rootfs is known-good (correct
 * colors); ours isn't. Rather than keep guessing which register might
 * differ, dump every one of them by name on both systems while LCDTest
 * -qws shows the same test pattern, and diff the two text outputs --
 * whatever differs (that isn't a known-dynamic register) is the lead.
 *
 * Reads via mmap()'d /dev/mem at the real physical LCD_BASE (0xE0500000,
 * confirmed from U-Boot's own ark1668_hardware.h and consistent with
 * mem-dump usage earlier this investigation) -- same access method
 * mem-dump.c already uses successfully for this hardware, no special
 * handling needed for MMIO (unlike the DMA-framebuffer case that tool
 * was written for).
 *
 * The register table below is mechanically extracted from
 * linux/include/linux/soc/arkmicro/ark1668_lcdc_regs.h (242 entries,
 * covering offsets 0x00-0x3fc) -- not retyped by hand, to avoid
 * transcription drift from the real header. A few offsets have two
 * names (e.g. EN/DMABADDR1 both at 0x00) because the vendor header
 * itself aliases them depending on context; both are printed.
 *
 * Palette RAM (starting at Palette_BASE = 0x400) is deliberately NOT
 * dumped -- LCDTest uses RGBA888/RGB888 (non-palette) formats, and the
 * palette table is large (typically 256+ entries); add it back with
 * --palette <count> if a palette-format investigation ever needs it.
 *
 * Usage: lcdc-regdump [output-label]
 *   output-label: optional string echoed in the header line, so two
 *   runs (e.g. "stock" and "ours") are self-identifying when diffed.
 *
 * Some registers are known-dynamic (frame counters, IRQ status,
 * write-back progress) and will legitimately differ between any two
 * runs regardless of the bug -- these are flagged inline with a
 * "(dynamic)" suffix so a diff's noise can be filtered by eye or with
 * `diff ... | grep -v '(dynamic)'`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <time.h>

#define LCD_BASE 0xE0500000UL
#define LCD_REGION_LEN 0x400UL

struct lcdc_reg {
    const char *name;
    unsigned int offset;
};

/* Mechanically generated from ark1668_lcdc_regs.h -- see top-of-file note. */
static const struct lcdc_reg regs[] = {
#include "lcdc_regtable.inc"
};
#define NUM_REGS (int)(sizeof(regs) / sizeof(regs[0]))

/* Registers expected to legitimately vary run-to-run regardless of the
 * color bug -- frame/IRQ/scaler-progress state, not static config. */
static int is_dynamic(const char *name)
{
    static const char *dynamic_substrings[] = {
        "INT_STATUS", "TIMING_FRAME_START_CNT", "WB_DATA_PER_FRAME",
        "WRITE_BACK_ADDR", NULL
    };
    for (int i = 0; dynamic_substrings[i]; i++)
        if (strstr(name, dynamic_substrings[i]))
            return 1;
    return 0;
}

int main(int argc, char **argv)
{
    const char *label = (argc >= 2) ? argv[1] : "run";

    int fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "open(/dev/mem): %s\n", strerror(errno));
        return 1;
    }

    long pagesize = sysconf(_SC_PAGESIZE);
    unsigned long page_base = LCD_BASE & ~(unsigned long)(pagesize - 1);
    unsigned long offset_in_page = LCD_BASE - page_base;
    unsigned long map_len = ((offset_in_page + LCD_REGION_LEN + pagesize - 1) / pagesize) * pagesize;

    void *map = mmap(NULL, map_len, PROT_READ, MAP_SHARED, fd, page_base);
    if (map == MAP_FAILED) {
        fprintf(stderr, "mmap(0x%lx, len=0x%lx): %s\n", page_base, map_len, strerror(errno));
        close(fd);
        return 1;
    }

    volatile unsigned char *base = (volatile unsigned char *)map + offset_in_page;

    time_t now = time(NULL);
    printf("# lcdc-regdump: %s @ %s", label, ctime(&now));
    printf("# LCD_BASE=0x%08lx, %d registers\n", LCD_BASE, NUM_REGS);

    for (int i = 0; i < NUM_REGS; i++) {
        unsigned int val;
        memcpy((void *)&val, (const void *)(base + regs[i].offset), 4);
        printf("%-42s +0x%03x = 0x%08x%s\n", regs[i].name, regs[i].offset, val,
               is_dynamic(regs[i].name) ? "  (dynamic)" : "");
    }

    munmap(map, map_len);
    close(fd);
    return 0;
}
