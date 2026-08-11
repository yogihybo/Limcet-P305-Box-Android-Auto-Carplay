// Shared visual language for every custom_ui screen, styled to sit
// comfortably next to a real Android Auto projection: a dark surface,
// one accent color, rounded cards, and a consistent pill-shaped back
// affordance in the header -- rather than each screen (home/bluetooth/
// settings/android_auto/reverse_camera) picking its own ad hoc colors
// and leaving default LVGL-theme buttons (light, blue) sitting on a
// manually-darkened background, which is what every screen did before
// this file existed.
//
// theme::init() calls lv_theme_default_init() with this palette and
// dark=true, so every default widget (button, switch, slider,
// dropdown, tabview, textarea) picks up consistent colors automatically
// without per-screen styling -- screens only need the helpers below
// for the things the default theme doesn't cover (cards, the header
// affordance, section labels).
#pragma once

#include "lvgl.h"

namespace ui::theme {

// ---- palette -----------------------------------------------------------
// Same hex values every screen was already using ad hoc; centralized
// here so there's exactly one place to retune the look.

inline lv_color_t bg()             { return lv_color_hex(0x14141e); }
inline lv_color_t surface()        { return lv_color_hex(0x1e1e2c); }
inline lv_color_t surface_border() { return lv_color_hex(0x33334a); }
inline lv_color_t surface_pressed() { return lv_color_hex(0x2a2a40); }
inline lv_color_t accent()         { return lv_color_hex(0x66aaff); }
inline lv_color_t accent_dim()     { return lv_color_hex(0x3d6ba3); }
inline lv_color_t text_primary()   { return lv_color_white(); }
inline lv_color_t text_secondary() { return lv_color_hex(0x999999); }
inline lv_color_t success()        { return lv_color_hex(0x4caf50); }
inline lv_color_t danger()         { return lv_color_hex(0xe05252); }

// ---- setup ---------------------------------------------------------------

// Applies the dark/accent default theme to `disp` -- call once, right
// after hal::init_display() succeeds, before any screen is created.
void init(lv_display_t * disp);

// ---- header (title + optional back affordance) ---------------------------

// Adds a circular, semi-transparent back button (chevron icon, no
// text) at the top-left, AA-style, wired to `cb`. Returns the button
// in case a caller needs it (none currently do).
lv_obj_t * add_back_button(lv_obj_t * scr, lv_event_cb_t cb);

// Adds the screen title, top-center, in the shared header font/color.
lv_obj_t * add_title(lv_obj_t * scr, const char * text);

// Convenience: dark screen background + back button + title, the
// combination every non-home screen used to hand-roll identically.
// Returns the title label in case a caller wants to reposition it
// (none currently do, but home_screen's own title differs enough --
// left-aligned, with a subtitle -- that it still builds its own).
lv_obj_t * create_screen_with_header(lv_obj_t ** out_scr, const char * title, lv_event_cb_t back_cb);

// ---- reusable element styles -----------------------------------------

// Rounded, bordered surface -- same visual as home_screen's launcher
// tiles, generalized for any content card (status panels, list
// containers, grouped settings rows).
void style_card(lv_obj_t * obj);

// Accent-filled, rounded call-to-action button (e.g. "Connect
// (Wireless)", "Save", "Refresh") -- replaces the default theme's
// button look where a screen wants to draw attention to one primary
// action.
void style_primary_button(lv_obj_t * btn);

// Small accent-colored caps-style label used for section headings
// ("Display", "Audio", "Status", "Notes") -- every screen was already
// hand-coloring these 0x66aaff; this just names the pattern.
void style_section_label(lv_obj_t * label);

// De-emphasized supporting text (addresses, hints, disabled-field
// notes) -- the 0x999999 gray every screen already used ad hoc.
void style_secondary_text(lv_obj_t * label);

}  // namespace ui::theme
