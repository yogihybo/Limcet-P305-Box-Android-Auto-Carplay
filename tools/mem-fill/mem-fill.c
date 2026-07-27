/*
 * mem-fill -- fills physical memory with a repeating 32-bit pattern via
 * mmap()'d /dev/mem. Write-side companion to tools/mem-dump -- same
 * mmap() rationale applies (plain write()/lseek() into /dev/mem only
 * succeeds for normal linear-mapped "System RAM", not reserved/no-map
 * DMA carve-out regions like an LCDC framebuffer scratch area; mmap()'s
 * mem_mmap() is far more permissive).
 *
 * Built for manually staging a test frame into an unused part of the
 * LCDC framebuffer DMA carve-out, then pointing a hardware layer's
 * *_ADDR register at it via devmem -- lets a video-layer/colorkey fix
 * be visually verified without a real decoder session feeding frames.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <stdint.h>

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <phys_addr> <length> <pattern32>\n", argv[0]);
        fprintf(stderr, "  phys_addr:  hex (0x...) or decimal physical address\n");
        fprintf(stderr, "  length:     bytes to fill (rounded down to a multiple of 4)\n");
        fprintf(stderr, "  pattern32:  hex (0x...) or decimal 32-bit value repeated across the range\n");
        fprintf(stderr, "\nExample -- fill 1.5MB at 0x0f800000 with opaque red (RGB888 0x00ff0000):\n");
        fprintf(stderr, "  %s 0x0f800000 0x177000 0x00ff0000\n", argv[0]);
        return 1;
    }

    unsigned long addr = strtoul(argv[1], NULL, 0);
    unsigned long length = strtoul(argv[2], NULL, 0) & ~0x3UL;
    uint32_t pattern = (uint32_t)strtoul(argv[3], NULL, 0);

    if (length == 0) {
        fprintf(stderr, "length must be >= 4\n");
        return 1;
    }

    long pagesize = sysconf(_SC_PAGESIZE);
    unsigned long page_base = addr & ~(unsigned long)(pagesize - 1);
    unsigned long offset_in_page = addr - page_base;
    unsigned long map_len = ((offset_in_page + length + pagesize - 1) / pagesize) * pagesize;

    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "open(/dev/mem): %s\n", strerror(errno));
        return 1;
    }

    void *map = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, page_base);
    if (map == MAP_FAILED) {
        fprintf(stderr, "mmap(0x%lx, len=0x%lx): %s\n", page_base, map_len, strerror(errno));
        close(fd);
        return 1;
    }

    uint32_t *words = (uint32_t *)((unsigned char *)map + offset_in_page);
    unsigned long n_words = length / 4;
    for (unsigned long i = 0; i < n_words; i++)
        words[i] = pattern;

    printf("Filled 0x%lx bytes at 0x%lx with pattern 0x%08x\n", length, addr, pattern);

    munmap(map, map_len);
    close(fd);
    return 0;
}
