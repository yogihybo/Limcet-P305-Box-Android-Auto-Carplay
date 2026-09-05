/* carpi_eq -- system-wide 3-band EQ + dynamic loudness, hosted via ALSA's
 * built-in "type ladspa" PCM plugin (real, confirmed built directly into
 * this target's libasound.so -- see docs/ note below).
 *
 * Why this exists: the original EQ/Loudness implementation lived entirely
 * inside micro_aap's own per-sample audio sink (aap_audio_sink_set_eq()),
 * so it only ever affected Android Auto's own media stream -- silently
 * doing nothing for radio, Bluetooth, CarPlay, or any other audio source,
 * and doing nothing at all unless an AA session happened to be connected
 * (custom_ui's own settings toggle talks to the AA sidecar over a socket
 * that only exists while AA is running). Real user report: "the loudness
 * toggle doesn't seem to work" -- confirmed true, not a false alarm.
 *
 * Real fix: this plugin is inserted into the ONE place all audio sources
 * on this device actually converge before reaching hardware -- ALSA's
 * shared `dmix` node (see firmware_overlay_dyn/etc/asound.conf, where
 * every softvolN control routes to the same dmix). Wiring it as dmix's
 * own downstream slave (dmix -> this plugin -> hw:0,0) means exactly ONE
 * instance processes the fully-mixed output of every source at once --
 * not once per source, which matters on this device's single-core
 * Cortex-A5.
 *
 * Real, load-bearing limitation of ALSA's own "type ladspa" PCM plugin,
 * confirmed by reading alsa-lib's pcm_ladspa.c directly: it does NOT
 * expose LADSPA control-input ports as live ALSA control/mixer elements
 * at all -- control values are read once, at PCM-open time, from static
 * asound.conf config, and never change again without the PCM being
 * closed and reopened (which would mean coordinating every audio-
 * producing process on the device to reopen its stream -- not viable).
 * So this plugin deliberately has ZERO LADSPA control ports; instead its
 * run() callback polls a small, atomically-written parameter file
 * (see PARAMS_PATH below) once per audio period (~10-20ms), which is
 * cheap and gives real, near-instant live adjustability without needing
 * ALSA's own (non-functional-for-this-purpose) control mechanism.
 *
 * DSP itself (3 shelving/peaking biquads + a 2-band loudness contour) is
 * the SAME already-verified math from
 * custom_ui/micro_aap/src/aap_audio.c's aap_audio_sink_set_eq() --
 * reused directly, not re-derived, since that code was already correct
 * and tuned. That AA-specific path is being retired in the same change
 * that introduces this plugin (see custom_ui/src/hal/audio.cpp) to avoid
 * double-applying EQ to AA's own audio once this system-wide instance is
 * in place -- AA's output reaches dmix like everything else now.
 *
 * Real, honest scope note: NOT hardware-tested. The DSP math is reused
 * verbatim from already-working code, and the plugin structure matches
 * the documented LADSPA API precisely, but the actual asound.conf wiring
 * and the live parameter-file hand-off have not been exercised on a real
 * device -- this needs a real deploy+listen test.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "ladspa.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Must match custom_ui/src/core/system_eq_params.h exactly -- kept as a
 * separate, deliberately tiny plain-C header (not shared directly with
 * this plugin's build) so this plugin has zero dependency on the rest of
 * custom_ui's C++ codebase; the two are kept in sync by hand, matching
 * the same "no shared header across a process/plugin boundary that has
 * to survive independent redeploys" reasoning already used elsewhere in
 * this project for the MCU wire-protocol framing. */
#define CARPI_EQ_PARAMS_PATH "/tmp/carpi_eq_params.bin"
#define CARPI_EQ_MAGIC 0x43457131u /* "CEq1" */

typedef struct {
    uint32_t magic;
    float bass_db;
    float mid_db;
    float treble_db;
    int32_t loudness_enabled;
} carpi_eq_params_t;

typedef struct {
    float b0, b1, b2, a1, a2;
    float x1, x2, y1, y2;
    int active;
} biquad_t;

typedef struct {
    unsigned long sample_rate;
    const LADSPA_Data *in[2];
    LADSPA_Data *out[2];

    biquad_t bass[2];
    biquad_t mid[2];
    biquad_t treble[2];
    biquad_t loud_bass[2];
    biquad_t loud_treble[2];

    float last_bass_db;
    float last_mid_db;
    float last_treble_db;
    int last_loudness;
    int have_params;

    int poll_counter;
} carpi_eq_t;

static void biquad_reset_state(biquad_t *f) {
    f->x1 = f->x2 = f->y1 = f->y2 = 0.0f;
}

static void init_low_shelf(biquad_t *f, float f0, float gain_db, float fs) {
    if (fabsf(gain_db) < 0.1f) {
        f->active = 0;
        biquad_reset_state(f);
        return;
    }
    float A = powf(10.0f, gain_db / 40.0f);
    float w0 = 2.0f * (float)M_PI * f0 / fs;
    float cos_w = cosf(w0);
    float sin_w = sinf(w0);
    float alpha = sin_w / 2.0f * 1.41421356f;

    float a0 = (A + 1.0f) + (A - 1.0f) * cos_w + 2.0f * sqrtf(A) * alpha;
    f->b0 = (A * ((A + 1.0f) - (A - 1.0f) * cos_w + 2.0f * sqrtf(A) * alpha)) / a0;
    f->b1 = (2.0f * A * ((A - 1.0f) - (A + 1.0f) * cos_w)) / a0;
    f->b2 = (A * ((A + 1.0f) - (A - 1.0f) * cos_w - 2.0f * sqrtf(A) * alpha)) / a0;
    f->a1 = (-2.0f * ((A - 1.0f) + (A + 1.0f) * cos_w)) / a0;
    f->a2 = ((A + 1.0f) + (A - 1.0f) * cos_w - 2.0f * sqrtf(A) * alpha) / a0;
    f->active = 1;
}

static void init_peaking(biquad_t *f, float f0, float gain_db, float Q, float fs) {
    if (fabsf(gain_db) < 0.1f) {
        f->active = 0;
        biquad_reset_state(f);
        return;
    }
    float A = powf(10.0f, gain_db / 40.0f);
    float w0 = 2.0f * (float)M_PI * f0 / fs;
    float cos_w = cosf(w0);
    float sin_w = sinf(w0);
    float alpha = sin_w / (2.0f * Q);

    float a0 = 1.0f + alpha / A;
    f->b0 = (1.0f + alpha * A) / a0;
    f->b1 = (-2.0f * cos_w) / a0;
    f->b2 = (1.0f - alpha * A) / a0;
    f->a1 = (-2.0f * cos_w) / a0;
    f->a2 = (1.0f - alpha / A) / a0;
    f->active = 1;
}

static void init_high_shelf(biquad_t *f, float f0, float gain_db, float fs) {
    if (fabsf(gain_db) < 0.1f) {
        f->active = 0;
        biquad_reset_state(f);
        return;
    }
    float A = powf(10.0f, gain_db / 40.0f);
    float w0 = 2.0f * (float)M_PI * f0 / fs;
    float cos_w = cosf(w0);
    float sin_w = sinf(w0);
    float alpha = sin_w / 2.0f * 1.41421356f;

    float a0 = (A + 1.0f) - (A - 1.0f) * cos_w + 2.0f * sqrtf(A) * alpha;
    f->b0 = (A * ((A + 1.0f) + (A - 1.0f) * cos_w + 2.0f * sqrtf(A) * alpha)) / a0;
    f->b1 = (-2.0f * A * ((A - 1.0f) + (A + 1.0f) * cos_w)) / a0;
    f->b2 = (A * ((A + 1.0f) + (A - 1.0f) * cos_w - 2.0f * sqrtf(A) * alpha)) / a0;
    f->a1 = (2.0f * ((A - 1.0f) - (A + 1.0f) * cos_w)) / a0;
    f->a2 = ((A + 1.0f) - (A - 1.0f) * cos_w - 2.0f * sqrtf(A) * alpha) / a0;
    f->active = 1;
}

static inline float process(biquad_t *f, float in) {
    if (!f->active) return in;
    float out = f->b0 * in + f->b1 * f->x1 + f->b2 * f->x2 - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1; f->x1 = in;
    f->y2 = f->y1; f->y1 = out;
    return out;
}

/* Reads the live params file. Deliberately tolerant: a missing file, a
 * short/torn read, or a bad magic number all just mean "no change" --
 * the plugin keeps whatever coefficients it already had rather than
 * ever going silent or crashing on a transient write race. */
static int read_params(carpi_eq_params_t *out) {
    int fd = open(CARPI_EQ_PARAMS_PATH, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = read(fd, out, sizeof(*out));
    close(fd);
    if (n != (ssize_t)sizeof(*out)) return 0;
    if (out->magic != CARPI_EQ_MAGIC) return 0;
    return 1;
}

/* 2026-09-05: real hardware concern raised via code review -- this used
 * to call read_params() (open+read+close of a tmpfs file) on EVERY
 * run() call, i.e. every single audio period (~10-20ms, 50-100x/sec
 * whenever any audio is active). That's a real syscall/VFS-lookup cost
 * paid unconditionally inside an ALSA real-time audio callback, on a
 * single-core Cortex-A5 -- plausible contributor to scheduling
 * pressure/XRUNs under load, though not confirmed as an actual cause
 * of any observed glitch on this hardware (this whole plugin remains
 * self-documented above as not yet hardware-tested).
 *
 * Throttled via the poll_counter field (already declared in the
 * struct, previously unused) instead of a wall-clock check --
 * clock_gettime() is its own syscall-ish cost to pay every period just
 * to decide whether to skip a check; a plain call counter is free and
 * exact enough given run() is invoked once per period regardless of
 * sample_count. kPollEveryNCalls=5 -> checks every ~50-100ms (10-20x/
 * sec) instead of every ~10-20ms (50-100x/sec) -- a 5x reduction in
 * open/read/close calls, still comfortably faster than a human can
 * perceive as a delay on a settings-screen slider. */
#define CARPI_EQ_POLL_EVERY_N_CALLS 5

static void maybe_update_coeffs(carpi_eq_t *p) {
    if (p->poll_counter > 0) {
        p->poll_counter--;
        return;
    }
    p->poll_counter = CARPI_EQ_POLL_EVERY_N_CALLS - 1;

    carpi_eq_params_t params;
    if (!read_params(&params)) return;

    if (p->have_params &&
        params.bass_db == p->last_bass_db &&
        params.mid_db == p->last_mid_db &&
        params.treble_db == p->last_treble_db &&
        params.loudness_enabled == p->last_loudness) {
        return; /* unchanged -- skip recomputing coefficients */
    }

    /* 2026-09-05: real hardware bug found via code review -- instantiate()
     * stores whatever sample_rate the host passes with no validation.
     * If that's ever 0 (a host misconfiguration, or a future asound.conf
     * edit that drops the rate), w0 = 2*pi*f0/fs below divides by zero,
     * producing inf/NaN biquad coefficients -- silence or digital noise
     * through the ENTIRE system-wide audio mixer (every source routes
     * through this one dmix-attached instance), not just this stream.
     * Falls back to 44100 (this plugin's own documented/expected rate,
     * per asound.conf) rather than silently computing garbage. */
    float fs = (float)p->sample_rate;
    if (fs <= 8000.0f) {
        fs = 44100.0f;
    }
    for (int ch = 0; ch < 2; ++ch) {
        init_low_shelf(&p->bass[ch], 100.0f, params.bass_db, fs);
        init_peaking(&p->mid[ch], 1000.0f, params.mid_db, 1.0f, fs);
        init_high_shelf(&p->treble[ch], 8000.0f, params.treble_db, fs);
        if (params.loudness_enabled) {
            init_low_shelf(&p->loud_bass[ch], 80.0f, 3.0f, fs);
            init_high_shelf(&p->loud_treble[ch], 10000.0f, 2.5f, fs);
        } else {
            p->loud_bass[ch].active = 0;
            p->loud_treble[ch].active = 0;
        }
    }

    p->last_bass_db = params.bass_db;
    p->last_mid_db = params.mid_db;
    p->last_treble_db = params.treble_db;
    p->last_loudness = params.loudness_enabled;
    p->have_params = 1;
}

static LADSPA_Handle instantiate(const LADSPA_Descriptor *d, unsigned long sample_rate) {
    (void)d;
    carpi_eq_t *p = (carpi_eq_t *)calloc(1, sizeof(carpi_eq_t));
    if (!p) return NULL;
    p->sample_rate = sample_rate;
    return p;
}

static void connect_port(LADSPA_Handle instance, unsigned long port, LADSPA_Data *data) {
    carpi_eq_t *p = (carpi_eq_t *)instance;
    switch (port) {
        case 0: p->in[0] = data; break;
        case 1: p->in[1] = data; break;
        case 2: p->out[0] = data; break;
        case 3: p->out[1] = data; break;
        default: break;
    }
}

static void activate(LADSPA_Handle instance) {
    carpi_eq_t *p = (carpi_eq_t *)instance;
    memset(p->bass, 0, sizeof(p->bass));
    memset(p->mid, 0, sizeof(p->mid));
    memset(p->treble, 0, sizeof(p->treble));
    memset(p->loud_bass, 0, sizeof(p->loud_bass));
    memset(p->loud_treble, 0, sizeof(p->loud_treble));
    p->have_params = 0;
    // Force an immediate params check on the first run() call after
    // (re)activation, rather than skipping the first
    // CARPI_EQ_POLL_EVERY_N_CALLS-1 calls -- matches have_params=0's
    // own "start from a known, fresh state" intent above.
    p->poll_counter = 0;
}

static void run(LADSPA_Handle instance, unsigned long sample_count) {
    carpi_eq_t *p = (carpi_eq_t *)instance;
    maybe_update_coeffs(p);

    for (int ch = 0; ch < 2; ++ch) {
        const LADSPA_Data *in = p->in[ch];
        LADSPA_Data *out = p->out[ch];
        if (!in || !out) continue;
        for (unsigned long i = 0; i < sample_count; ++i) {
            float s = in[i];
            s = process(&p->bass[ch], s);
            s = process(&p->mid[ch], s);
            s = process(&p->treble[ch], s);
            s = process(&p->loud_bass[ch], s);
            s = process(&p->loud_treble[ch], s);
            out[i] = s;
        }
    }
}

static void cleanup(LADSPA_Handle instance) {
    free(instance);
}

static const char *port_names[4] = { "In L", "In R", "Out L", "Out R" };

static LADSPA_PortDescriptor port_descriptors[4] = {
    LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO,
    LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO,
    LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO,
    LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO,
};

static LADSPA_PortRangeHint port_hints[4] = {
    { 0, 0.0f, 0.0f },
    { 0, 0.0f, 0.0f },
    { 0, 0.0f, 0.0f },
    { 0, 0.0f, 0.0f },
};

static LADSPA_Descriptor descriptor = {
    .UniqueID = 0x4341524,  /* "CAR" -- arbitrary, not registered with any central body (none of this project's other in-house tools are either); real collision risk is negligible since this plugin is only ever loaded from this project's own private plugin path. */
    .Label = "carpi_eq",
    /* NOT LADSPA_PROPERTY_HARD_RT_CAPABLE -- run() does a real open()/
     * read()/close() on a tmpfs file every call, which is a genuine
     * (if in-practice-cheap-and-tmpfs-backed) syscall path; claiming
     * hard-RT-capable would be a real, incorrect promise per the LADSPA
     * spec's own definition of that property. */
    .Properties = LADSPA_PROPERTY_REALTIME,
    .Name = "Carpi System EQ + Loudness",
    .Maker = "prado-firmware-reconstruction",
    .Copyright = "None",
    .PortCount = 4,
    .PortDescriptors = port_descriptors,
    .PortNames = port_names,
    .PortRangeHints = port_hints,
    .ImplementationData = NULL,
    .instantiate = instantiate,
    .connect_port = connect_port,
    .activate = activate,
    .run = run,
    .run_adding = NULL,
    .set_run_adding_gain = NULL,
    .deactivate = NULL,
    .cleanup = cleanup,
};

const LADSPA_Descriptor *ladspa_descriptor(unsigned long index) {
    if (index == 0) return &descriptor;
    return NULL;
}
