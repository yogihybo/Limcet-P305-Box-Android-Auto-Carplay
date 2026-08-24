#include "aap_video.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <dlfcn.h>

#define MEMALLOC_IOCXGETBUFFER  0xc0046b01
#define MEMALLOC_IOCXFREEBUFFER 0x40046b02
#define ARK_IO_INIT_FB_DISPLAY  0x403c4f27
#define ARK_IO_SET_FB_ADDR      0x40104f2a
#define ARK_DISPLAY_IOC_SHOW    0x8004a009
#define ARK_DISPLAY_IOC_HIDE    0x8004a00a

#define STREAM_BUF_SIZE (1024 * 1024)

typedef struct {
    uint32_t busAddress;
    uint32_t size;
} MemallocParams;

typedef struct {
    uint32_t pStream;
    uint32_t streamBusAddress;
    uint32_t dataLen;
    uint32_t picId;
} H264DecInput;

typedef struct {
    uint32_t pStrmCurrPos;
    uint32_t strmCurrBusAddress;
    uint32_t dataLeft;
    uint8_t  pad[64];
} H264DecOutput;

typedef struct {
    uint8_t *pOutputPicture;
    uint32_t outputPictureBusAddress;
    uint32_t picWidth;
    uint32_t picHeight;
    uint32_t picFormat;
    uint32_t nbrOfErrMBs;
    uint32_t picId;
    uint32_t isIdr;
    uint32_t interlaced;
    uint32_t field;
    uint32_t topFieldFirst;
} H264DecPicture;

struct ark_disp_update_window {
    uint32_t win_x;
    uint32_t win_y;
    uint32_t win_width;
    uint32_t win_height;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t rgb_order;
    uint32_t yuyv_order;
    uint32_t out_x;
    uint32_t out_y;
    uint32_t out_width;
    uint32_t out_height;
    uint32_t interlace_out;
    uint32_t show_tv;
};

struct ark_disp_addr {
    uint32_t y;
    uint32_t cb_cr;
    uint32_t reserved0;
    uint32_t reserved1;
};

struct aap_video_sink {
    uint32_t width;
    uint32_t height;
    bool is_visible;
    bool is_configured;

    void *hantro_lib;
    void *hantro_dec;
    int (*h264_init)(void **dec_inst, uint32_t no_output_reordering, uint32_t use_display_smoothing);
    int (*h264_decode)(void *dec_inst, const H264DecInput *input, H264DecOutput *output);
    int (*h264_next_picture)(void *dec_inst, H264DecPicture *picture, uint32_t end_of_stream);
    void (*h264_release)(void *dec_inst);

    int memalloc_fd;
    uint32_t stream_bus_addr;
    uint8_t *stream_vir_addr;

    int fb4_fd;
    int ark_disp_fd;
    uint32_t pic_id;
};

aap_video_sink_t *aap_video_sink_create(uint32_t width, uint32_t height) {
    aap_video_sink_t *sink = (aap_video_sink_t *)calloc(1, sizeof(aap_video_sink_t));
    if (!sink) return NULL;

    sink->width = width ? width : 800;
    sink->height = height ? height : 480;
    sink->memalloc_fd = -1;
    sink->fb4_fd = -1;
    sink->ark_disp_fd = -1;
    sink->is_visible = true;

    return sink;
}

void aap_video_sink_destroy(aap_video_sink_t *sink) {
    if (!sink) return;
    aap_video_sink_close(sink);
    free(sink);
}

bool aap_video_sink_open(aap_video_sink_t *sink) {
    if (!sink) return false;

    sink->hantro_lib = dlopen("/usr/lib/libmfc.so", RTLD_NOW);
    if (!sink->hantro_lib) {
        fprintf(stderr, "aap_video: dlopen(/usr/lib/libmfc.so) failed: %s\n", dlerror());
        return false;
    }

    sink->h264_init = dlsym(sink->hantro_lib, "H264DecInit");
    sink->h264_decode = dlsym(sink->hantro_lib, "H264DecDecode");
    sink->h264_next_picture = dlsym(sink->hantro_lib, "H264DecNextPicture");
    sink->h264_release = dlsym(sink->hantro_lib, "H264DecRelease");

    if (!sink->h264_init || !sink->h264_decode || !sink->h264_next_picture || !sink->h264_release) {
        fprintf(stderr, "aap_video: dlsym failed for Hantro functions\n");
        return false;
    }

    int ret = sink->h264_init(&sink->hantro_dec, 0, 0);
    if (ret != 0) {
        fprintf(stderr, "aap_video: H264DecInit failed with %d\n", ret);
        return false;
    }

    sink->memalloc_fd = open("/tmp/dev/memalloc", O_RDWR);
    if (sink->memalloc_fd < 0) {
        sink->memalloc_fd = open("/dev/memalloc", O_RDWR);
    }
    if (sink->memalloc_fd < 0) {
        fprintf(stderr, "aap_video: open memalloc failed\n");
        return false;
    }

    MemallocParams params = {0, STREAM_BUF_SIZE};
    if (ioctl(sink->memalloc_fd, MEMALLOC_IOCXGETBUFFER, &params) != 0) {
        fprintf(stderr, "aap_video: ioctl MEMALLOC_IOCXGETBUFFER failed\n");
        return false;
    }

    sink->stream_bus_addr = params.busAddress;
    sink->stream_vir_addr = (uint8_t *)mmap(NULL, STREAM_BUF_SIZE, PROT_READ | PROT_WRITE,
                                            MAP_SHARED, sink->memalloc_fd, (off_t)params.busAddress);
    if (sink->stream_vir_addr == MAP_FAILED) {
        fprintf(stderr, "aap_video: mmap stream buffer failed\n");
        return false;
    }

    sink->fb4_fd = open("/dev/fb4", O_RDWR);
    sink->ark_disp_fd = open("/dev/ark_display", O_RDWR);

    printf("aap_video: Hantro H.264 hardware decoder initialized (800x480)\n");
    return true;
}

void aap_video_sink_close(aap_video_sink_t *sink) {
    if (!sink) return;

    if (sink->stream_vir_addr && sink->stream_vir_addr != MAP_FAILED) {
        munmap(sink->stream_vir_addr, STREAM_BUF_SIZE);
        sink->stream_vir_addr = NULL;
    }
    if (sink->memalloc_fd >= 0) {
        if (sink->stream_bus_addr) {
            MemallocParams params = {sink->stream_bus_addr, STREAM_BUF_SIZE};
            ioctl(sink->memalloc_fd, MEMALLOC_IOCXFREEBUFFER, &params);
        }
        close(sink->memalloc_fd);
        sink->memalloc_fd = -1;
    }

    if (sink->hantro_dec && sink->h264_release) {
        sink->h264_release(sink->hantro_dec);
        sink->hantro_dec = NULL;
    }
    if (sink->hantro_lib) {
        dlclose(sink->hantro_lib);
        sink->hantro_lib = NULL;
    }
    if (sink->fb4_fd >= 0) {
        close(sink->fb4_fd);
        sink->fb4_fd = -1;
    }
    if (sink->ark_disp_fd >= 0) {
        close(sink->ark_disp_fd);
        sink->ark_disp_fd = -1;
    }
}

static void configure_video_layer(aap_video_sink_t *sink, uint32_t width, uint32_t height) {
    if (sink->fb4_fd < 0 || sink->is_configured) return;

    struct ark_disp_update_window win;
    memset(&win, 0, sizeof(win));
    win.win_x = 0;
    win.win_y = 0;
    win.win_width = width;
    win.win_height = height;
    win.width = (width + 15) & ~15;
    win.height = (height + 15) & ~15;
    win.format = 0x11; /* YUV420 semi-planar */
    win.out_x = 0;
    win.out_y = 0;
    win.out_width = 800;
    win.out_height = 480;

    ioctl(sink->fb4_fd, ARK_IO_INIT_FB_DISPLAY, &win);
    sink->is_configured = true;
}

bool aap_video_sink_decode(aap_video_sink_t *sink, const uint8_t *nalu_data, size_t nalu_len) {
    if (!sink || !sink->hantro_dec || !nalu_data || nalu_len == 0) return false;
    if (nalu_len > STREAM_BUF_SIZE) nalu_len = STREAM_BUF_SIZE;

    memcpy(sink->stream_vir_addr, nalu_data, nalu_len);

    H264DecInput in;
    H264DecOutput out;
    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));

    in.pStream = (uint32_t)sink->stream_vir_addr;
    in.streamBusAddress = sink->stream_bus_addr;
    in.dataLen = (uint32_t)nalu_len;
    in.picId = ++sink->pic_id;

    int ret = sink->h264_decode(sink->hantro_dec, &in, &out);
    if (ret < 0) {
        return false;
    }

    H264DecPicture pic;
    memset(&pic, 0, sizeof(pic));
    bool rendered = false;

    while (sink->h264_next_picture(sink->hantro_dec, &pic, 0) == 2 /* kH264DecPicRdy */) {
        if (!sink->is_configured) {
            configure_video_layer(sink, pic.picWidth, pic.picHeight);
        }

        if (sink->fb4_fd >= 0 && sink->is_visible) {
            struct ark_disp_addr addr;
            uint32_t aligned_w = (pic.picWidth + 15) & ~15;
            uint32_t aligned_h = (pic.picHeight + 15) & ~15;
            addr.y = pic.outputPictureBusAddress;
            addr.cb_cr = pic.outputPictureBusAddress + (aligned_w * aligned_h);
            addr.reserved0 = 0;
            addr.reserved1 = 0;
            ioctl(sink->fb4_fd, ARK_IO_SET_FB_ADDR, &addr);
            rendered = true;
        }
    }

    return rendered;
}

void aap_video_sink_set_visible(aap_video_sink_t *sink, bool visible) {
    if (!sink) return;
    sink->is_visible = visible;
    if (sink->ark_disp_fd >= 0) {
        uint32_t layer_idx = 4; /* Video layer 4 */
        ioctl(sink->ark_disp_fd, visible ? ARK_DISPLAY_IOC_SHOW : ARK_DISPLAY_IOC_HIDE, &layer_idx);
    }
}
