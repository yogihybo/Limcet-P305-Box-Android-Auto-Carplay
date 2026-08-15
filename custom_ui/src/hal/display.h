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

// Hardware-level show/hide of an ARKFB layer (ARKFB_SHOW_WINDOW_REAL /
// ARKFB_HIDE_WINDOW_REAL, see init_display()'s kArkfbShowWindowReal
// comment for provenance -- same ioctls, same device-confirmed
// mechanism already used for the AA video layer on /dev/fb1). Each
// call does its own independent open()/ioctl()/close() (matching
// init_display()'s own show-ioctl pattern) rather than holding a
// persistent fd, so this never interferes with LVGL's own fbdev
// handle. Intended for hal::hide_display() to fully disable the OSD2
// (LVGL/UI) hardware layer while Android Auto's video is showing full-
// screen -- AA's own in-app UI has its own exit affordance, so this
// screen's chrome (header/back button) doesn't need to stay visible
// underneath. Returns false (logs the reason) on failure; non-fatal.
bool hide_display(const char * fb_path = "/dev/fb0");
bool show_display(const char * fb_path = "/dev/fb0");

}  // namespace hal
