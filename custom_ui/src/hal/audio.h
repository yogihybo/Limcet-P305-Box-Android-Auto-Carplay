#pragma once

namespace hal {

// Initializes the ARK1680 DAC, BD37033 Power Amplifier (PA), and ALSA mixer
// controls so audio output is unmuted and routed to the car speakers.
void init_audio_mixer();

}  // namespace hal
