// Reversing-camera HAL. Wraps two real kernel devices this project's
// own linux-arkmicro reconstruction exposes -- see
// docs/ARCHITECTURE.md's "Reversing camera" section for the full
// research trail this is built on:
//
//  - /dev/dvr (ARK_DVR_* ioctls, driver:
//    linux-arkmicro/linux/drivers/soc/arkmicro/itu656/ark1668_itu656.c)
//    -- controls the ITU656/RN6752 camera decode pipeline itself
//    (start/stop, channel select, brightness/contrast/hue/mirror).
//    IMPORTANT: ARK_DVR_GETFRAME is a compiled-in no-op in this
//    driver (empty switch case) -- there is genuinely no
//    software frame-readback path. The decoded video is composited
//    directly onto its own hardware display layer (DISPLAY_LAYER=4,
//    separate from our LVGL/fb0 GUI layer) by the LCDC itself, not
//    delivered to userspace as pixel data. This HAL therefore cannot
//    and does not attempt to pull frames into an LVGL canvas -- it
//    only starts/stops the pipeline and adjusts image parameters.
//    The actual picture appears (or doesn't) independent of anything
//    this process draws, as long as our own GUI layer isn't opaquely
//    covering it -- see reverse_camera_screen.cpp for how that's
//    handled today (unconfirmed on real hardware).
//
//  - /dev/carback (device node name from the driver's own
//    device_create() call -- NOT /dev/ark_carback, that's only the
//    platform driver's name/pr_fmt prefix, same class of naming trap
//    already hit once for hx170dec, see memory
//    project_hx170dec_device_naming) -- CARBACK_IOCTL_* app-ready
//    coordination protocol + blocking read()/poll()/fasync for
//    reverse-gear state changes.
//
// Both are optional at runtime by design (same non-fatal pattern as
// hal::init_touch): a build/device without the reversing-camera
// hardware wired should still boot the rest of the UI.
#pragma once

namespace hal {

// Handle for both device fds. -1 in either field means that device
// wasn't available -- callers must check before using the
// corresponding half of the API.
struct CameraHandle {
    int dvr_fd = -1;
    int carback_fd = -1;
};

// Opens /dev/dvr and /dev/carback if present. Returns false only if
// BOTH fail to open -- either one alone is enough to be useful (e.g.
// carback-only lets a listener react to gear state even if the video
// pipeline itself can't be controlled from here).
bool init_camera(CameraHandle & out, const char * dvr_path = "/dev/dvr",
                  const char * carback_path = "/dev/carback");

// ARK_DVR_START / ARK_DVR_STOP. No-op if dvr_fd is closed.
void start_camera_stream(CameraHandle & h);
void stop_camera_stream(CameraHandle & h);

enum class ReverseGearState {
    Unknown,     // carback device unavailable, or no read yet
    Engaged,     // reverse gear active -- CARBACK_IOCTL_GET_STATUS == 1
    Disengaged,
};

// CARBACK_IOCTL_GET_STATUS -- current state without blocking.
ReverseGearState get_reverse_gear_state(CameraHandle & h);

// CARBACK_IOCTL_SET_APP_READY -- tells the kernel driver this process
// wants to participate in the enter/exit handshake (carback_int_work()
// in ark-carback.c waits up to 500ms for app_enter_done/app_exit_done
// before forcibly hiding/showing our GUI layer itself). Call once
// after init_camera() succeeds, before the first blocking read.
void set_app_ready(CameraHandle & h);

// CARBACK_IOCTL_APP_ENTER_DONE / APP_EXIT_DONE -- ack the transition
// once the UI has actually finished reacting (e.g. pushed/popped the
// reverse camera screen). Only meaningful after set_app_ready(); the
// kernel proceeds on a fixed 500ms timeout regardless, so a missed ack
// degrades to an abrupt layer switch rather than a hang.
void ack_enter_done(CameraHandle & h);
void ack_exit_done(CameraHandle & h);

// Blocking read of the next carback state-change byte (mirrors the
// driver's own read() contract: wait_event_interruptible on
// carback_changed, exactly 1 byte). Meant to be run on its own thread
// -- see main.cpp's reverse-gear listener. Returns Unknown on error
// (fd closed, read failure) so callers can distinguish "no camera
// hardware" from "gear disengaged".
ReverseGearState wait_reverse_gear_change(CameraHandle & h);

void close_camera(CameraHandle & h);

// Real reversing-camera video FORMAT selector -- disassembled from the
// real stock vendor app (usr/lib/libSetting.so's
// FactoryWindow::on_btnCameraType_clicked(), the "OEM Factory Camera"
// UI setting) 2026-08-29. NOT a binary OEM/Aftermarket relay toggle
// (this project's own CMD 0xA0 id=0x11 / CMD 0x84 sends for this were
// built on a wrong premise -- see docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md).
// Real mechanism, zero MCU/UART/CAN involvement, matches stock exactly:
// persists a U-Boot env var (read by
// linux-arkmicro/u-boot/.../ark1668_display_cfg.c's
// ark_carback_camera_check() at next boot) AND writes a kernel sysfs
// attribute the rn6752 video-decoder driver exposes, for an immediate
// runtime effect.
enum class CameraFormat {
    Auto        = 0,  // CARBACK_CAMERA_MODE_DYNAMIC
    CvbsPal     = 1,  // CARBACK_CAMERA_MODE_CVBS_PAL
    CvbsNtsc    = 2,  // CARBACK_CAMERA_MODE_CVBS_NTST
    Ahd720p25   = 3,  // CARBACK_CAMERA_MODE_720P25
    Ahd720p30   = 4,  // CARBACK_CAMERA_MODE_720P30
    Ahd1080p25  = 5,  // CARBACK_CAMERA_MODE_1080P25
    Ahd1080p30  = 6,  // CARBACK_CAMERA_MODE_1080P30
};

// Runs, in order, exactly what stock's real button handler runs:
//   fw_setenv carback_camera_mode <N>
//   echo "camera_mode <N>" > /sys/devices/platform/i2c-gpio.1/i2c-1/1-002c/dvr
// `format` is a fixed enum value (never raw user/network input), so the
// same std::system()-based approach this project already uses elsewhere
// (hal/bluetooth.cpp, hal/androidauto_client.cpp) carries no injection
// risk here.
void set_camera_format(CameraFormat format);

}  // namespace hal
