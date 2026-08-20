#include "hal/audio.h"

#include <cstdio>
#include <cstdlib>
#include "core/log_timing.h"

namespace hal {

void init_audio_mixer() {
    std::printf("%s hal::audio::init_audio_mixer: unmuting DAC, BD37033 amplifier, and ALSA softvol\n",
                core::log_timestamp().c_str());

    // 1. Unmute ARK-SDDAC hardware DAC volume (0..127, 118 matches stock /etc/all.sh)
    std::system("amixer cset iface=MIXER,name='Left Playback Volume' 118 >/dev/null 2>&1 || true");
    std::system("amixer cset iface=MIXER,name='Right Playback Volume' 118 >/dev/null 2>&1 || true");

    // 2. Configure BD37033 Power Amplifier (PA) controls
    // PA Input Select: 1 = Main DAC (SDDAC / Bluetooth / Android Auto)
    std::system("amixer cset iface=MIXER,name='PA Input Select' 1 >/dev/null 2>&1 || true");
    std::system("amixer cset iface=MIXER,name='PA Mute' 0 >/dev/null 2>&1 || true");
    std::system("amixer cset iface=MIXER,name='PA Volume' 40 >/dev/null 2>&1 || true");
    std::system("amixer cset iface=MIXER,name='PA Fader-FL' 0 >/dev/null 2>&1 || true");
    std::system("amixer cset iface=MIXER,name='PA Fader-FR' 0 >/dev/null 2>&1 || true");
    std::system("amixer cset iface=MIXER,name='PA Fader-RL' 0 >/dev/null 2>&1 || true");
    std::system("amixer cset iface=MIXER,name='PA Fader-RR' 0 >/dev/null 2>&1 || true");

    // 3. Set ALSA software volumes to 100% (255)
    std::system("amixer sset 'softmaster' 100% >/dev/null 2>&1 || true");
    std::system("amixer sset 'softmaster1' 100% >/dev/null 2>&1 || true");
    std::system("amixer sset 'softmaster2' 100% >/dev/null 2>&1 || true");
    std::system("amixer sset 'softmaster3' 100% >/dev/null 2>&1 || true");
    std::system("amixer sset 'softmaster4' 100% >/dev/null 2>&1 || true");
    std::system("amixer sset 'softmaster5' 100% >/dev/null 2>&1 || true");
}

}  // namespace hal
