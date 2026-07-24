/*
 * mem-dump -- hex-dumps physical memory via mmap()'d /dev/mem.
 *
 * Why not `dd if=/dev/mem | hexdump`? dd's plain read()/lseek() path
 * into /dev/mem goes through the kernel's mem_rw() -> xlate_dev_mem_ptr(),
 * which only succeeds for physical addresses that are part of the
 * kernel's normal linear-mapped "System RAM" -- it fails with EFAULT
 * ("Bad address") for reserved/no-map DMA carve-out regions (e.g. LCDC
 * framebuffer memory reserved via a DT reserved-memory node), which is
 * exactly the kind of memory this tool needs to read. `devmem`'s
 * mmap()-based access works for these regions instead (mem_mmap() only
 * checks valid_mmap_phys_addr_range(), which is far more permissive --
 * that's how devmem successfully reads LCDC MMIO registers). This tool
 * applies the same mmap() approach to dump an arbitrary-length range
 * instead of a single register value.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <phys_addr> [length]\n", argv[0]);
        fprintf(stderr, "  phys_addr: hex (0x...) or decimal physical address\n");
        fprintf(stderr, "  length:    bytes to dump, default 64\n");
        return 1;
    }

    unsigned long addr = strtoul(argv[1], NULL, 0);
    unsigned long length = (argc >= 3) ? strtoul(argv[2], NULL, 0) : 64;

    long pagesize = sysconf(_SC_PAGESIZE);
    unsigned long page_base = addr & ~(unsigned long)(pagesize - 1);
    unsigned long offset_in_page = addr - page_base;
    unsigned long map_len = ((offset_in_page + length + pagesize - 1) / pagesize) * pagesize;

    int fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "open(/dev/mem): %s\n", strerror(errno));
        return 1;
    }

    void *map = mmap(NULL, map_len, PROT_READ, MAP_SHARED, fd, page_base);
    if (map == MAP_FAILED) {
        fprintf(stderr, "mmap(0x%lx, len=0x%lx): %s\n", page_base, map_len, strerror(errno));
        close(fd);
        return 1;
    }

    const unsigned char *bytes = (const unsigned char *)map + offset_in_page;

    for (unsigned long i = 0; i < length; i += 16) {
        printf("%08lx  ", addr + i);
        for (unsigned long j = 0; j < 16; j++) {
            if (i + j < length)
                printf("%02x ", bytes[i + j]);
            else
                printf("   ");
            if (j == 7)
                printf(" ");
        }
        printf(" |");
        for (unsigned long j = 0; j < 16 && i + j < length; j++) {
            unsigned char c = bytes[i + j];
            putchar((c >= 0x20 && c < 0x7f) ? c : '.');
        }
        printf("|\n");
    }

    munmap(map, map_len);
    close(fd);
    return 0;
}
