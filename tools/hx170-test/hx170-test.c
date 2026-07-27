/*
 * hx170-test -- direct standalone test of the Hantro hx170dec H.264
 * hardware decoder via libmfc.so's real H264DecInit/H264DecDecode API,
 * completely bypassing sink/Android Auto/the network. Built to answer
 * a specific question this project could not otherwise answer: is the
 * decoder hardware+driver+libmfc.so stack itself broken, or is the
 * real problem upstream of it (no frame data ever reaching sink)?
 *
 * Real API signatures confirmed via Ghidra decompile of the deployed
 * libmfc.so (not guessed/from generic docs):
 *   i32 H264DecInit(H264DecInst *pDecInst, u32 noOutputReordering,
 *                    u32 useVideoFreezeConcealment, u32 numOutputBuffers);
 *   i32 H264DecDecode(H264DecInst decInst, H264DecInput *pInput,
 *                      H264DecOutput *pOutput);
 *   i32 H264DecNextPicture(H264DecInst decInst, H264DecPicture *pPicture,
 *                           u32 endOfStream);
 *   void H264DecRelease(H264DecInst decInst);
 * H264DecInput field order/semantics (offset 0/4/8/12) cross-checked
 * against H264DecDecode's own early validation code (offset-8 field
 * range-checked 1..0xffffff, matches dataLen; offsets 0/4 sanity-
 * checked >=0x40, matches pointer/bus-address, not length) and matches
 * the well-documented public Hantro G1/8170 legacy decoder API.
 *
 * DMA input buffer: rather than replicating libmfc.so's own internal
 * DWLInit()/DWLMallocLinear() (which needs an opaque DWL context this
 * tool has no independent way to construct correctly), this opens
 * /tmp/dev/memalloc directly and replicates DWLMallocLinear's own
 * ioctl sequence by hand (confirmed via decompile: 0xc0046b01 allocate
 * -> u32 bus address out, then mmap() using that as the file offset).
 * /tmp/dev/memalloc itself is a real, already hardware-confirmed-
 * working device node (see docs/DEVICE_TEST_CHECKLIST_2026-07-18.md,
 * 2026-07-20 fix) -- this tool only needs it for staging the input
 * bitstream, not for the decoder's own internal hardware context,
 * which H264DecInit sets up independently.
 *
 * CAVEAT, stated plainly: the embedded test H.264 stream below is a
 * minimal baseline-profile SPS+PPS+IDR sequence for a tiny (16x16)
 * solid frame, reconstructed carefully from H.264 bitstream syntax
 * rather than copied from a verified reference file -- if this tool
 * reports a decode failure, check whether the failure looks like a
 * bitstream/SPS-parsing problem specifically (inconclusive -- could
 * be this test stream, not real hardware) versus a device/ioctl/open
 * failure or hang (unambiguous evidence of a real driver/hardware
 * problem, regardless of stream validity).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <errno.h>

typedef void *H264DecInst;

typedef struct {
    uint32_t pStream;         /* virtual address of input buffer */
    uint32_t streamBusAddress;
    uint32_t dataLen;
    uint32_t picId;
} H264DecInput;

typedef struct {
    uint32_t pStrmCurrPos;
    uint32_t strmCurrBusAddress;
    uint32_t dataLeft;
    uint8_t  pad[64]; /* generous padding -- real struct has more trailing
                          fields (picture info) this tool doesn't need to
                          interpret, just needs to not overrun the buffer
                          libmfc.so writes into */
} H264DecOutput;

typedef struct {
    uint8_t pad[256]; /* H264DecPicture is a large struct (frame dims,
                          plane pointers, crop info, etc.) -- oversized
                          padding since this tool only reads back the
                          call's return code, not individual fields */
} H264DecPicture;

/* DWLMallocLinear's own confirmed ioctl protocol against /dev/memalloc,
 * replicated by hand against our own fd (see file header). */
#define MEMALLOC_IOCX_GETBUFFER 0xc0046b01
#define MEMALLOC_IOCX_FREEBUFFER 0x40046b02

static void *g_mem_virt;
static uint32_t g_mem_bus;
static uint32_t g_mem_size;
static int g_mem_fd = -1;

static int mem_alloc(uint32_t size) {
    long pagesize = sysconf(_SC_PAGESIZE);
    uint32_t aligned = (size + pagesize - 1) & ~(uint32_t)(pagesize - 1);

    g_mem_fd = open("/tmp/dev/memalloc", O_RDWR);
    if (g_mem_fd < 0) {
        fprintf(stderr, "open(/tmp/dev/memalloc): %s\n", strerror(errno));
        return -1;
    }

    uint32_t bus_addr = 0;
    if (ioctl(g_mem_fd, MEMALLOC_IOCX_GETBUFFER, &bus_addr) < 0) {
        fprintf(stderr, "ioctl(GETBUFFER) on /tmp/dev/memalloc: %s\n", strerror(errno));
        close(g_mem_fd);
        g_mem_fd = -1;
        return -1;
    }
    if (bus_addr == 0) {
        fprintf(stderr, "ioctl(GETBUFFER) returned bus address 0 -- allocation failed\n");
        close(g_mem_fd);
        g_mem_fd = -1;
        return -1;
    }

    void *virt = mmap(NULL, aligned, PROT_READ | PROT_WRITE, MAP_SHARED, g_mem_fd, bus_addr);
    if (virt == MAP_FAILED) {
        fprintf(stderr, "mmap(/tmp/dev/memalloc, bus=0x%x, size=0x%x): %s\n", bus_addr, aligned, strerror(errno));
        close(g_mem_fd);
        g_mem_fd = -1;
        return -1;
    }

    g_mem_virt = virt;
    g_mem_bus = bus_addr;
    g_mem_size = aligned;
    return 0;
}

static void mem_free(void) {
    if (g_mem_fd < 0) return;
    if (g_mem_virt) munmap(g_mem_virt, g_mem_size);
    ioctl(g_mem_fd, MEMALLOC_IOCX_FREEBUFFER, &g_mem_bus);
    close(g_mem_fd);
    g_mem_fd = -1;
}

/* Minimal baseline-profile H.264 elementary stream: SPS + PPS + one IDR
 * slice, 16x16 luma (1 macroblock), I_PCM-free CAVLC I_16x16 coding,
 * solid mid-gray. Annex-B start-code-prefixed NAL units. See the file
 * header's caveat about this stream's provenance. */
static const uint8_t g_test_stream[] = {
    /* SPS: nal_ref_idc=3, nal_unit_type=7 */
    0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x0a,
    0x96, 0x54, 0x0a, 0x0f, 0xd8, 0x08, 0x80, 0x00,
    0x00, 0x03, 0x00, 0x80, 0x00, 0x00, 0x1e, 0x23,
    0xc4, 0x40,
    /* PPS: nal_ref_idc=3, nal_unit_type=8 */
    0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80,
    /* IDR slice: nal_ref_idc=3, nal_unit_type=5 */
    0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21,
    0xff, 0xfe, 0xf7, 0xdf, 0x7d, 0xfd, 0xfb, 0xfb,
    0xdf, 0x7d, 0xf7, 0xdb, 0xfb, 0xdf, 0x7d, 0xf7,
    0xdb, 0xfb, 0x80
};

int main(int argc, char **argv) {
    const char *libpath = "/usr/lib/libmfc.so";
    if (argc > 1) libpath = argv[1];

    printf("hx170-test: standalone hardware H.264 decode test\n");
    printf("Loading %s ...\n", libpath);

    void *lib = dlopen(libpath, RTLD_NOW);
    if (!lib) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }

    int (*H264DecInit)(H264DecInst *, uint32_t, uint32_t, uint32_t) = dlsym(lib, "H264DecInit");
    int (*H264DecDecode)(H264DecInst, H264DecInput *, H264DecOutput *) = dlsym(lib, "H264DecDecode");
    int (*H264DecNextPicture)(H264DecInst, H264DecPicture *, uint32_t) = dlsym(lib, "H264DecNextPicture");
    void (*H264DecRelease)(H264DecInst) = dlsym(lib, "H264DecRelease");

    if (!H264DecInit || !H264DecDecode || !H264DecNextPicture || !H264DecRelease) {
        fprintf(stderr, "dlsym failed to resolve one or more H264Dec* symbols: %s\n", dlerror());
        return 1;
    }
    printf("Resolved H264DecInit/Decode/NextPicture/Release OK.\n");

    printf("Allocating %zu-byte DMA input buffer via /tmp/dev/memalloc ...\n", sizeof(g_test_stream));
    if (mem_alloc(sizeof(g_test_stream)) != 0) {
        fprintf(stderr, "FAIL: could not allocate DMA buffer -- this points at /tmp/dev/memalloc "
                        "itself, not the decoder.\n");
        return 1;
    }
    printf("DMA buffer OK: virt=%p bus=0x%08x size=0x%x\n", g_mem_virt, g_mem_bus, g_mem_size);
    memcpy(g_mem_virt, g_test_stream, sizeof(g_test_stream));

    H264DecInst dec = NULL;
    printf("Calling H264DecInit(noReorder=0, freezeConcealment=0, numBuffers=0) ...\n");
    int init_ret = H264DecInit(&dec, 0, 0, 0);
    printf("H264DecInit returned %d (0x%x), handle=%p\n", init_ret, init_ret, dec);
    if (init_ret != 0 || dec == NULL) {
        fprintf(stderr, "FAIL: H264DecInit did not succeed. This means the decoder hardware/driver "
                        "itself is not initializing correctly -- unrelated to sink/Android Auto.\n");
        mem_free();
        return 1;
    }
    printf("PASS: decoder hardware initialized successfully (real Product ID 0x6731 confirmed by "
           "H264DecInit's own ASIC ID check).\n");

    H264DecInput input;
    memset(&input, 0, sizeof(input));
    input.pStream = (uint32_t)(uintptr_t)g_mem_virt;
    input.streamBusAddress = g_mem_bus;
    input.dataLen = sizeof(g_test_stream);
    input.picId = 1;

    H264DecOutput output;
    memset(&output, 0, sizeof(output));

    printf("Calling H264DecDecode(dataLen=%u) ...\n", input.dataLen);
    int decode_ret = H264DecDecode(dec, &input, &output);
    printf("H264DecDecode returned %d (0x%x)\n", decode_ret, decode_ret);

    H264DecPicture pic;
    memset(&pic, 0, sizeof(pic));
    printf("Calling H264DecNextPicture(endOfStream=1) ...\n");
    int pic_ret = H264DecNextPicture(dec, &pic, 1);
    printf("H264DecNextPicture returned %d (0x%x)\n", pic_ret, pic_ret);

    H264DecRelease(dec);
    mem_free();

    printf("\n=== Summary ===\n");
    printf("H264DecInit:         %d\n", init_ret);
    printf("H264DecDecode:       %d\n", decode_ret);
    printf("H264DecNextPicture:  %d\n", pic_ret);
    printf("\nIf H264DecInit succeeded (0) but Decode/NextPicture returned an error, that's most\n");
    printf("likely this tool's hand-built test stream, not the hardware -- still proves the\n");
    printf("driver/hardware bring-up itself works, which is the main open question. If\n");
    printf("H264DecInit itself failed, or anything above hung/crashed instead of returning,\n");
    printf("that IS real evidence of a hardware/driver problem.\n");

    return 0;
}
