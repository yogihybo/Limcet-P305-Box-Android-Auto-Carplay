#pragma once

namespace hal {

// Initializes the ARK1680 DAC, BD37033 Power Amplifier (PA), and ALSA mixer
// controls so audio output is unmuted and routed to the car speakers.
void init_audio_mixer();

// The three Android Auto media-sink audio streams each route through their
// OWN softvol ALSA PCM device, each with its own independent ALSA mixer
// control -- confirmed real via /etc/asound.conf and androidauto/session.cpp's
// own AudioChannel construction (not just one shared DAC gain):
//   Media    -> plug:softvol2 -> mixer control "softmaster2"
//   Guidance -> plug:softvol1 -> mixer control "softmaster1"
//   System   -> plug:softvol4 -> mixer control "softmaster4"
// (softmaster/softmaster3/softmaster5 exist too, but nothing in this app
// currently routes through them -- see asound.conf, not exposed here.)
enum class AudioStream { Media, Guidance, System };

// Sets one stream's independent ALSA softvol level, 0-100 (%). Real,
// audible effect -- distinct from init_audio_mixer()'s one-time startup
// unmute, and from the VDE display HAL's brightness/contrast/etc (a
// completely separate hardware path, see hal/display_ctrl.h).
bool set_stream_volume(AudioStream stream, int percent);

}  // namespace hal
