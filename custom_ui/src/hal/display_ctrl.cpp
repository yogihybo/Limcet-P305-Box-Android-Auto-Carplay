#include "hal/display_ctrl.h"
#include "core/log_timing.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace hal {

namespace {

// Local copy, same rationale as hal/camera.cpp's header comment --
// minimal ioctl-ABI-only excerpt rather than vendoring the whole
// vendor header. Confirmed against arkpro_custom/vendor/AVService/
// display.h (see display_ctrl.h's top comment).
#define ARKDISP_IOCTL_BASE 0xa0
#define ARKDISP_GET_VDE_CFG _IOWR(ARKDISP_IOCTL_BASE, 1, unsigned long)
#define ARKDISP_SET_VDE_CFG _IOW(ARKDISP_IOCTL_BASE, 2, unsigned long)

struct ark_disp_vde_cfg_arg {
    unsigned int layer_id;
    unsigned int hue;
    unsigned int saturation;
    unsigned int brightness;
    unsigned int contrast;
};

}  // namespace

bool init_display_ctrl(DisplayCtrlHandle & out, const char * path) {
    out.fd = open(path, O_RDWR);
    if (out.fd < 0) {
        std::fprintf(stderr, "%s hal::display_ctrl::init_display_ctrl: warning: %s unavailable (%s)\n", core::log_timestamp().c_str(), path,
                     std::strerror(errno));
        return false;
    }
    return true;
}

bool get_vde_config(DisplayCtrlHandle & h, DisplayLayer layer, VdeConfig & out) {
    if (h.fd < 0) return false;
    ark_disp_vde_cfg_arg arg{};
    arg.layer_id = static_cast<unsigned int>(layer);
    if (ioctl(h.fd, ARKDISP_GET_VDE_CFG, &arg) < 0) {
        std::fprintf(stderr, "%s hal::display_ctrl::get_vde_config: ARKDISP_GET_VDE_CFG failed (%s)\n", core::log_timestamp().c_str(),
                     std::strerror(errno));
        return false;
    }
    out.hue = arg.hue;
    out.saturation = arg.saturation;
    out.brightness = arg.brightness;
    out.contrast = arg.contrast;
    return true;
}

bool set_vde_config(DisplayCtrlHandle & h, DisplayLayer layer, const VdeConfig & cfg) {
    if (h.fd < 0) return false;
    ark_disp_vde_cfg_arg arg{};
    arg.layer_id = static_cast<unsigned int>(layer);
    arg.hue = cfg.hue;
    arg.saturation = cfg.saturation;
    arg.brightness = cfg.brightness;
    arg.contrast = cfg.contrast;
    if (ioctl(h.fd, ARKDISP_SET_VDE_CFG, &arg) < 0) {
        std::fprintf(stderr, "%s hal::display_ctrl::set_vde_config: ARKDISP_SET_VDE_CFG failed (%s)\n", core::log_timestamp().c_str(),
                     std::strerror(errno));
        return false;
    }
    return true;
}

void close_display_ctrl(DisplayCtrlHandle & h) {
    if (h.fd >= 0) {
        close(h.fd);
        h.fd = -1;
    }
}

namespace {

// 2026-09-05: real hardware bug found via code review -- set/get below
// used to write/read the raw 0-100 percent value straight to/from the
// kernel's own "brightness" sysfs node, but the Linux backlight class
// convention (confirmed applicable here: drivers/video/backlight/
// pwm_bl.c IS built for this kernel per CONFIG_BACKLIGHT_PWM=y, and
// docs/logs/directfb_strace.txt shows a real userspace process walking
// /sys/class/backlight/backlight/ using the standard sysfs class
// enumeration -- a real, active backlight class device on this
// hardware, not a vendor-custom path) scales "brightness" against a
// SEPARATE, driver-reported "max_brightness" sibling file, not a fixed
// 0-100 range. If that max is ever anything other than 100 (e.g. 255,
// common for an 8-bit PWM duty-cycle table), writing a raw percent
// under-drives the backlight and the getter returns a value sliders
// misread as "over 100%".
//
// Rather than hardcode an assumed max (this device's own real value
// couldn't be confirmed from available logs/DT -- the DTS's own
// lcdcon-backlight/backlight-value properties turned out to belong to
// the display controller node, not a standard pwm-backlight binding,
// so guessing wrong here risked making things WORSE, not better), this
// reads the real max_brightness from the same sysfs directory at
// runtime and scales against THAT. Self-correcting regardless of what
// the real value is: if it's genuinely 100, this is a no-op-equivalent
// to the old behavior; if it's 255 (or anything else), it's now
// actually correct.
int read_max_brightness(const char * brightness_path) {
    // brightness_path is ".../brightness"; max_brightness is always
    // the sibling file in the same directory, per the Linux backlight
    // class's own sysfs layout.
    std::string path(brightness_path);
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return 100;
    std::string max_path = path.substr(0, slash + 1) + "max_brightness";

    FILE * f = fopen(max_path.c_str(), "r");
    if (!f) return 100;  // no sibling file -- assume this node is already 0-100
    int max_val = 100;
    if (fscanf(f, "%d", &max_val) != 1 || max_val <= 0) {
        max_val = 100;
    }
    fclose(f);
    return max_val;
}

}  // namespace

bool set_backlight_brightness(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    const char * paths[] = {
        "/sys/class/backlight/backlight/brightness",
        "/sys/class/backlight/pwm-backlight/brightness",
        "/sys/devices/platform/pwm-backlight/backlight/pwm-backlight/brightness"
    };
    for (const char * path : paths) {
        FILE * f = fopen(path, "w");
        if (f) {
            int max_val = read_max_brightness(path);
            int raw = (percent * max_val) / 100;
            fprintf(f, "%d\n", raw);
            fclose(f);
            return true;
        }
    }
    return false;
}

int get_backlight_brightness() {
    const char * paths[] = {
        "/sys/class/backlight/backlight/brightness",
        "/sys/class/backlight/pwm-backlight/brightness",
        "/sys/devices/platform/pwm-backlight/backlight/pwm-backlight/brightness"
    };
    for (const char * path : paths) {
        FILE * f = fopen(path, "r");
        if (f) {
            int val = 100;
            if (fscanf(f, "%d", &val) == 1) {
                fclose(f);
                int max_val = read_max_brightness(path);
                if (max_val <= 0) max_val = 100;
                int percent = (val * 100) / max_val;
                if (percent < 0) percent = 0;
                if (percent > 100) percent = 100;
                return percent;
            }
            fclose(f);
        }
    }
    return 100;
}

}  // namespace hal
