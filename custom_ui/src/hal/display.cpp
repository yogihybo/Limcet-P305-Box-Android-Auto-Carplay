#include "hal/display.h"

#include <cstdio>
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace hal {

namespace {

// ARK_IO(43)/ARK_IO(44) -- this device's real ARKFB_SHOW_WINDOW_REAL/
// ARKFB_HIDE_WINDOW_REAL, per linux-arkmicro's ark_lcdc_common.h. Not
// pulled from a shared header (that kernel tree isn't part of this
// repo's build) -- hardcoded with provenance, same convention already
// used in hal/camera.h and hal/display_ctrl.h for this device's other
// vendor ioctls.
//
// Confirmed via decompile of stock's real libarkcmn.so
// (docs/DEVICE_TEST_CHECKLIST_2026-07-18.md section 29):
// arkapi_show_fb(fd, show) is exactly `ioctl(fd, 0x4f2c, 0)` to hide,
// `ioctl(fd, 0x4f2b, 0)` to show -- the same mechanism already
// hardware-confirmed fixing the Android Auto video layer's "renders
// but never appears" bug. This is the layer-enable path real vendor
// apps actually use, more direct than relying on
// lv_linux_fbdev_set_force_refresh()'s indirect
// FBIOPUT_VSCREENINFO -> fb_set_par() route (kept below too, since
// it's harmless and may matter for geometry/colorkey reapplication,
// but this explicit call is the one with hardware precedent).
constexpr unsigned long kArkfbShowWindowReal = 0x4f2b;

}  // namespace

lv_display_t * init_display(const char * fb_path) {
    // Pre-flight diagnostic, entirely separate from lv_linux_fbdev's own
    // internal open/mmap below -- runs BEFORE handing off to that
    // (opaque, third-party) code path so we get visibility even if
    // lv_linux_fbdev_set_file() itself never returns. Specifically
    // testing a real hypothesis: if FBIOGET_VSCREENINFO reports a
    // bogus/mismatched struct on this kernel, lv_linux_fbdev would
    // compute an enormous draw_buf_size and hang inside lv_malloc() --
    // a pure userspace hang (no dmesg output, process shows as
    // "running" not blocked), matching what's been observed on real
    // hardware. Printed here so this shows up even if that theory is
    // right and everything after this point stays silent.
    {
        int probe_fd = open(fb_path, O_RDWR);
        if (probe_fd >= 0) {
            std::printf("hal::init_display: pre-flight open(%s) ok, probing ioctls...\n", fb_path);
            if (ioctl(probe_fd, FBIOBLANK, FB_BLANK_UNBLANK) != 0) {
                perror("hal::init_display: pre-flight ioctl(FBIOBLANK)");
            }
            struct fb_var_screeninfo probe_var {};
            struct fb_fix_screeninfo probe_fix {};
            bool got_var = ioctl(probe_fd, FBIOGET_VSCREENINFO, &probe_var) == 0;
            bool got_fix = ioctl(probe_fd, FBIOGET_FSCREENINFO, &probe_fix) == 0;
            if (got_var) {
                std::printf("hal::init_display: pre-flight FBIOGET_VSCREENINFO: "
                            "xres=%u yres=%u xres_virtual=%u yres_virtual=%u bits_per_pixel=%u\n",
                            probe_var.xres, probe_var.yres, probe_var.xres_virtual,
                            probe_var.yres_virtual, probe_var.bits_per_pixel);
                unsigned long long est_buf_size =
                    static_cast<unsigned long long>(probe_var.xres) *
                    (probe_var.bits_per_pixel / 8) * probe_var.yres;
                std::printf("hal::init_display: pre-flight estimated single draw buffer size: "
                            "%llu bytes (%.1f MB)\n",
                            est_buf_size, static_cast<double>(est_buf_size) / (1024.0 * 1024.0));
            } else {
                perror("hal::init_display: pre-flight ioctl(FBIOGET_VSCREENINFO)");
            }
            if (got_fix) {
                std::printf("hal::init_display: pre-flight FBIOGET_FSCREENINFO: "
                            "line_length=%u smem_start=0x%lx smem_len=%u type=%u visual=%u\n",
                            probe_fix.line_length, probe_fix.smem_start, probe_fix.smem_len,
                            probe_fix.type, probe_fix.visual);
            } else {
                perror("hal::init_display: pre-flight ioctl(FBIOGET_FSCREENINFO)");
            }
            close(probe_fd);
        } else {
            perror("hal::init_display: pre-flight open() failed");
        }
    }

    std::printf("hal::init_display: calling lv_linux_fbdev_create()...\n");
    lv_display_t * disp = lv_linux_fbdev_create();
    if (!disp) {
        std::fprintf(stderr, "hal::init_display: lv_linux_fbdev_create() failed\n");
        return nullptr;
    }
    std::printf("hal::init_display: calling lv_linux_fbdev_set_file(%s)...\n", fb_path);
    if (lv_linux_fbdev_set_file(disp, fb_path) != LV_RESULT_OK) {
        std::fprintf(stderr, "hal::init_display: failed to open %s\n", fb_path);
        return nullptr;
    }
    std::printf("hal::init_display: %s opened and mapped by lv_linux_fbdev\n", fb_path);

    // See kArkfbShowWindowReal's comment: without this, this device's
    // kernel driver never turns on the OSD1 hardware layer, so writes
    // to the mmap'd framebuffer succeed but nothing reaches the panel
    // -- confirmed on real hardware: process alive and rendering,
    // /dev/fb0 open, screen stayed blank until this was added.
    int show_fd = open(fb_path, O_RDWR);
    if (show_fd >= 0) {
        if (ioctl(show_fd, kArkfbShowWindowReal, 0) != 0) {
            perror("hal::init_display: ioctl(ARKFB_SHOW_WINDOW_REAL)");
        } else {
            std::printf("hal::init_display: ioctl(ARKFB_SHOW_WINDOW_REAL) on %s: ok\n", fb_path);
        }

        // Diagnostic only -- read back what the kernel actually reports
        // for this layer's mode/geometry/buffer, independent of
        // whatever lv_linux_fbdev negotiated internally, so a hardware
        // test can confirm what's really active.
        struct fb_var_screeninfo var {};
        struct fb_fix_screeninfo fix {};
        if (ioctl(show_fd, FBIOGET_VSCREENINFO, &var) == 0 &&
            ioctl(show_fd, FBIOGET_FSCREENINFO, &fix) == 0) {
            std::printf("hal::init_display: %s reports %ux%u, %ubpp, line_length=%u, "
                        "smem_start=0x%lx, smem_len=%u\n",
                        fb_path, var.xres, var.yres, var.bits_per_pixel,
                        fix.line_length, fix.smem_start, fix.smem_len);
        } else {
            perror("hal::init_display: FBIOGET_VSCREENINFO/FBIOGET_FSCREENINFO");
        }

        close(show_fd);
    } else {
        std::fprintf(stderr, "hal::init_display: couldn't reopen %s for SHOW_WINDOW\n", fb_path);
    }

    // Belt-and-suspenders: also force every flush to go through
    // FBIOPUT_VSCREENINFO (fb_set_par()'s own OSD1-unconditional-
    // enable path, per linux-arkmicro's ark1668_lcdfb.c) in case
    // something later disables the layer again.
    lv_linux_fbdev_set_force_refresh(disp, true);
    std::printf("hal::init_display: done, force_refresh enabled\n");

    return disp;
}

}  // namespace hal
