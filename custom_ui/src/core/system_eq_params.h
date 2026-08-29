#pragma once

// Wire format shared with custom_ui/ladspa_eq/carpi_eq.c's own
// carpi_eq_params_t -- MUST be kept byte-for-byte identical (magic
// value, field order, field types) since the two sides are built by
// completely separate toolchains/build steps (this is compiled as part
// of custom_ui's own C++ build; carpi_eq.c is built standalone as a
// plain-C LADSPA .so) and never share a header directly, matching the
// same "no shared header across a process/plugin boundary" reasoning
// already used for the MCU wire-protocol framing in this project. See
// carpi_eq.c's own top-of-file comment for the full reasoning behind
// this whole mechanism (why a params *file* instead of ALSA controls).
//
// Real, honest note: NOT hardware-tested. Reused verbatim from already-
// working DSP code, but this specific plumbing (file-based live handoff
// to a LADSPA plugin's run() callback) hasn't been exercised on a real
// device yet.

#include <cstdint>

namespace core {

#define CARPI_EQ_PARAMS_PATH "/tmp/carpi_eq_params.bin"
constexpr uint32_t kCarpiEqMagic = 0x43457131u;  // "CEq1"

struct CarpiEqParams {
    uint32_t magic = kCarpiEqMagic;
    float bass_db = 0.0f;
    float mid_db = 0.0f;
    float treble_db = 0.0f;
    int32_t loudness_enabled = 0;
};

// Writes params to CARPI_EQ_PARAMS_PATH via write-to-temp + rename (atomic
// on the same tmpfs filesystem), so carpi_eq.c's run() callback never
// observes a torn/partial write. Returns false (logged by the caller) on
// any I/O failure -- never throws, safe to call from the LVGL main thread.
bool write_system_eq_params(const CarpiEqParams &params);

}  // namespace core
