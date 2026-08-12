#pragma once

#include <atomic>

// 2026-08-12: process-wide flag controlling whether VideoChannel should
// actually show the decoded-frame hardware layer (hal::show_video_layer(),
// /dev/fb1/VIDEO2, see video_layer.h) once frames are available, or keep
// it hidden even though decoding/pushing frames continues.
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

}  // namespace androidauto
