// /dev/ark_display HAL -- per-layer hue/saturation/brightness/contrast,
// backing the Settings screen's Display panel (Phase 3, Basic tier).
// See docs/ARCHITECTURE.md's "Display" section: misc device,
// ARKDISP_GET_VDE_CFG/ARKDISP_SET_VDE_CFG ioctls, layer_id 0-4.
//
// Ioctl numbers and struct ark_disp_vde_cfg_arg layout confirmed
// against the real vendor header, `arkpro_custom/vendor/AVService/
// display.h` (ARKDISP_IOCTL_BASE=0xa0, GET=cmd 1, SET=cmd 2 --
// matches the independently-confirmed encoding in
// `display handoff_vm_prompt.md`/`.agents/skills/embedded-engineer/
// SKILL.md`: ARKDISP_GET_VDE_CFG=0xc004a001, ARKDISP_SET_VDE_CFG=
// 0x4004a002, both `unsigned long`-sized regardless of the actual
// struct payload -- matching the userspace ioctl encoding convention
// note in that same handoff doc). Not yet hardware-tested by this
// project against a real /dev/ark_display.
#pragma once

namespace hal {

enum class DisplayLayer {
    Osd1 = 0,   // primary UI layer -- this app's own fb0/GUI layer
    Osd2 = 1,
    Osd3 = 2,
    Video = 3,
    Video2 = 4,  // reversing-camera pipeline, DISPLAY_LAYER=4 per
                 // docs/ARCHITECTURE.md -- do not fight
                 // src/hal/camera.h for control of this layer
};

struct VdeConfig {
    unsigned int hue = 0;
    unsigned int saturation = 0;
    unsigned int brightness = 0;
    unsigned int contrast = 0;
};

// Handle for /dev/ark_display. fd == -1 means unavailable -- callers
// must check before using the rest of this API (non-fatal pattern,
// same as hal::CameraHandle).
struct DisplayCtrlHandle {
    int fd = -1;
};

bool init_display_ctrl(DisplayCtrlHandle & out, const char * path = "/dev/ark_display");

// Returns false (and leaves *out untouched) on ioctl failure or an
// unavailable handle.
bool get_vde_config(DisplayCtrlHandle & h, DisplayLayer layer, VdeConfig & out);

// Returns false on ioctl failure or an unavailable handle.
bool set_vde_config(DisplayCtrlHandle & h, DisplayLayer layer, const VdeConfig & cfg);

void close_display_ctrl(DisplayCtrlHandle & h);

// Controls physical PWM backlight intensity (0..100%) via sysfs without altering VDE color matrix
bool set_backlight_brightness(int percent);
int get_backlight_brightness();

}  // namespace hal
