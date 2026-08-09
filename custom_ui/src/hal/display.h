// Framebuffer display HAL. Wraps LVGL's lv_linux_fbdev driver against
// this hardware's real /dev/fb0 -- see docs/ARCHITECTURE.md's
// "Display" section for why fbdev directly, not DirectFB/GPU.
#pragma once

#include "lvgl.h"

namespace hal {

// Opens /dev/fb0 and creates the LVGL display for it. Returns nullptr
// on failure (device busy/missing -- e.g. MsnCoreApp/DirectFB still
// holding the framebuffer).
lv_display_t * init_display(const char * fb_path = "/dev/fb0");

}  // namespace hal
