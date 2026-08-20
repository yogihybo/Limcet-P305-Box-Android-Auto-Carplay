#include "hal/audio.h"

#include <cstdio>
#include <cstdlib>
#include "core/log_timing.h"

namespace hal {

void init_audio_mixer() {
    std::printf("%s hal::audio::init_audio_mixer: unmuting ARK-SDDAC hardware DAC and ALSA softmaster\n",
                core::log_timestamp().c_str());

    // 1. Unmute ARK-SDDAC hardware DAC volume (0..127, 118 matches stock /etc/all.sh)
    std::system("amixer cset iface=MIXER,name='Left Playback Volume' 118 >/dev/null 2>&1 || true");
    std::system("amixer cset iface=MIXER,name='Right Playback Volume' 118 >/dev/null 2>&1 || true");

    // 2. Set ALSA software volume controls (softmaster, softmaster1..5) to 100%
    std::system("amixer sset 'softmaster' 100% >/dev/null 2>&1 || true");
    std::system("amixer sset 'softmaster1' 100% >/dev/null 2>&1 || true");
    std::system("amixer sset 'softmaster2' 100% >/dev/null 2>&1 || true");
    std::system("amixer sset 'softmaster3' 100% >/dev/null 2>&1 || true");
    std::system("amixer sset 'softmaster4' 100% >/dev/null 2>&1 || true");
    std::system("amixer sset 'softmaster5' 100% >/dev/null 2>&1 || true");
}

}  // namespace hal
