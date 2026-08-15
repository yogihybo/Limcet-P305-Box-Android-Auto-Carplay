#include "hal/display_ctrl.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
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
        std::fprintf(stderr, "hal::display_ctrl::init_display_ctrl: warning: %s unavailable (%s)\n", path,
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
        std::fprintf(stderr, "hal::display_ctrl::get_vde_config: ARKDISP_GET_VDE_CFG failed (%s)\n",
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
        std::fprintf(stderr, "hal::display_ctrl::set_vde_config: ARKDISP_SET_VDE_CFG failed (%s)\n",
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

}  // namespace hal
