#include "hal/audio.h"

#include <cstdio>
#include <cstdlib>
#include "core/log_timing.h"
#include "core/config_store.h"
#include "core/system_eq_params.h"

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
    // 2026-08-29: REWRITTEN -- this used to call AndroidAutoClient::sendEq(),
    // which only ever reached AA's own in-process media-stream EQ
    // (micro_aap's aap_audio_sink_set_eq()) over a socket that only exists
    // while an AA session is actively connected. Real, confirmed bug: the
    // toggle silently did nothing whenever AA wasn't running (the common
    // case), and even when it was, it never touched radio/Bluetooth/
    // CarPlay/anything else. Real fix: write the live params to the file
    // custom_ui/ladspa_eq/carpi_eq.c's run() callback polls every audio
    // period -- that plugin sits on dmix's own downstream slave (see
    // firmware_overlay_dyn/etc/asound.conf), the one place every audio
    // source on this device actually converges before reaching hardware,
    // so this now genuinely applies system-wide, regardless of whether AA
    // is connected. The AA-specific sendEq() path is deliberately no
    // longer called at all -- keeping both would double-apply the EQ to
    // AA's own audio once it also passes through carpi_eq_out at the ALSA
    // level. Real, honest scope note: NOT hardware-tested end to end.
    core::CarpiEqParams params;
    params.bass_db = static_cast<float>(bass_db);
    params.mid_db = static_cast<float>(mid_db);
    params.treble_db = static_cast<float>(treble_db);
    params.loudness_enabled = loudness ? 1 : 0;
    bool ok = core::write_system_eq_params(params);
    std::printf("%s [HAL:AUDIO] Applying system-wide 3-Band EQ: Bass=%d dB, Mid=%d dB, Treble=%d dB, "
                "Loudness=%d (params file write %s)\n",
                core::log_timestamp().c_str(), bass_db, mid_db, treble_db, loudness ? 1 : 0,
                ok ? "OK" : "FAILED");
}

void sync_audio_eq() {
    int bass = core::default_store().get_int("Bass", 0, "Audio");
    int mid = core::default_store().get_int("Mid", 0, "Audio");
    int treble = core::default_store().get_int("Treble", 0, "Audio");
    bool loudness = core::default_store().get_bool("Loudness", false, "Audio");
    set_audio_eq(bass, mid, treble, loudness);
}

}  // namespace hal
