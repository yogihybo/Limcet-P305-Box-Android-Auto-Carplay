#include "hal/audio.h"

#include <cstdio>
#include <cstdlib>
#include "core/log_timing.h"
#include "core/config_store.h"
#include "hal/androidauto_client.h"

namespace hal {

namespace {

const char * mixer_name_for(AudioStream stream) {
    switch (stream) {
        case AudioStream::Media:    return "softmaster2";
        case AudioStream::Guidance: return "softmaster1";
        case AudioStream::System:   return "softmaster4";
    }
    return "softmaster2";
}

}  // namespace

void init_audio_mixer() {
    std::printf("%s [HAL:AUDIO] Unmuting ARK-SDDAC hardware DAC and ALSA softmaster\n",
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

bool set_stream_volume(AudioStream stream, int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    char cmd[128];
    std::snprintf(cmd, sizeof(cmd), "amixer sset '%s' %d%% >/dev/null 2>&1",
                  mixer_name_for(stream), percent);
    return std::system(cmd) == 0;
}

void apply_reversing_volume_cut(bool in_reverse) {
    int mediaVol = core::default_store().get_int("MediaVolume", 80, "Audio");
    int sysVol = core::default_store().get_int("SystemVolume", 80, "Audio");
    if (in_reverse) {
        int cut = core::default_store().get_int("ReversingVolumeCut", 70, "General");
        if (cut < 0) cut = 0;
        if (cut > 100) cut = 100;
        int attMedia = (mediaVol * (100 - cut)) / 100;
        int attSys = (sysVol * (100 - cut)) / 100;
        std::printf("%s [HAL:AUDIO] Reverse gear engaged -- applying %d%% volume cut (Media: %d%% -> %d%%)\n",
                    core::log_timestamp().c_str(), cut, mediaVol, attMedia);
        set_stream_volume(AudioStream::Media, attMedia);
        set_stream_volume(AudioStream::System, attSys);
    } else {
        std::printf("%s [HAL:AUDIO] Reverse gear disengaged -- restoring volume (Media: %d%%)\n",
                    core::log_timestamp().c_str(), mediaVol);
        set_stream_volume(AudioStream::Media, mediaVol);
        set_stream_volume(AudioStream::System, sysVol);
    }
}

void set_audio_eq(int bass_db, int mid_db, int treble_db, bool loudness) {
    std::printf("%s [HAL:AUDIO] Applying 3-Band Parametric EQ: Bass=%d dB, Mid=%d dB, Treble=%d dB, Loudness=%d\n",
                core::log_timestamp().c_str(), bass_db, mid_db, treble_db, loudness ? 1 : 0);
    AndroidAutoClient client;
    client.sendEq(bass_db, mid_db, treble_db, loudness);
}

void sync_audio_eq() {
    int bass = core::default_store().get_int("Bass", 0, "Audio");
    int mid = core::default_store().get_int("Mid", 0, "Audio");
    int treble = core::default_store().get_int("Treble", 0, "Audio");
    bool loudness = core::default_store().get_bool("Loudness", false, "Audio");
    set_audio_eq(bass, mid, treble, loudness);
}

}  // namespace hal
