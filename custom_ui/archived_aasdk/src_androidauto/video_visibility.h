#pragma once

#include <atomic>

// 2026-08-12: process-wide flag controlling whether VideoChannel should
// actually show the decoded-frame hardware layer (hal::show_video_layer(),
// /dev/fb4/VIDEO_LAYER2, see video_layer.h) once frames are available, or
// keep it hidden even though decoding/pushing frames continues.
//
// Why this exists: with wireless_session_manager.cpp's connection now
// able to auto-start the moment a phone is detected as Android-Auto-
// capable over Bluetooth (see main.cpp's AaAutoStartWatcher), the
// session -- and therefore video decode -- can be running well before
// the user has ever opened ui/android_auto_screen.cpp. video_channel.cpp
// used to call hal::show_video_layer() unconditionally the instant the
// first real frame decoded, with no awareness of which custom_ui screen
// was active -- meaning AA video could appear over the Home screen (or
// wherever the user happened to be) the moment a background auto-start
// connection finished, before the user ever selected the AA icon. Per
// explicit request: selecting the AA icon is what should reveal the
// video feed; auto-start should only control whether the CONNECTION
// (and decode) begins in the background, not whether it's visible.
//
// Set by sidecars/androidauto/main.cpp's "SHOW"/"HIDE" socket commands,
// sent by ui::android_auto_screen.cpp when it becomes the active screen
// / when the user navigates away (see hal::AndroidAutoClient::setVisible()).
// Read by VideoChannel::pushDecodedFrame() on every frame, reconciling
// the hardware layer's actual shown/hidden state against this flag --
// decode itself is NOT gated by this, only the hardware layer's
// visibility, so video is ready to display instantly the moment the
// user does select the AA icon rather than needing to wait for decode
// to catch up.
namespace androidauto {

inline std::atomic<bool> & video_visible() {
    static std::atomic<bool> flag{false};
    return flag;
}

// 2026-08-19: separate from video_visible() above on purpose -- that
// flag is the UI process's own "is the AA screen selected" intent
// (SHOW/HIDE commands); this one is the PHONE's own intent, real
// hardware-confirmed (VideoFocusRequestNotification.mode(), decompiled
// against aap_protobuf's real VideoFocusMode enum -- VIDEO_FOCUS_NATIVE
// = 2 is exactly what Android Auto's own in-app exit/back control
// sends: "give focus back to your native UI," NOT a session teardown
// -- see docs research, the phone stays connected in the background).
// video_channel.cpp's onVideoFocusRequest() sets this; custom_ui
// polls it (via a new "FOCUS" sidecar command) to know when to switch
// its own fb0/LVGL layer back into view even though the session is
// still fully "Connected" and everything else (audio, sensors) keeps
// running untouched.
inline std::atomic<bool> & video_focus_native() {
    static std::atomic<bool> flag{false};
    return flag;
}

}  // namespace androidauto
