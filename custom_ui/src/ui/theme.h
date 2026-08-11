// Shared visual language for every custom_ui screen, styled to sit
// comfortably next to a real Android Auto projection: a dark surface,
// one accent color, rounded cards, and a consistent pill-shaped back
// affordance in the header -- rather than each screen (home/bluetooth/
// settings/android_auto/reverse_camera) picking its own ad hoc colors
// and leaving default LVGL-theme buttons (light, blue) sitting on a
// manually-darkened background, which is what every screen did before
// this file existed.
//
// Two things this file optimizes for, not just color matching:
//
// 1. Reads as "Android Auto app grid", not "generic dark app": real AA/
//    AAOS launcher tiles are one big glyph on a solid colored circular
//    badge with a short label underneath, no visible card border, and
//    a LOT of tap-target area relative to the visible icon. See
//    style_icon_badge() / kIconBadgeDiameter below -- home_screen.cpp's
//    tiles use these now instead of a bordered rectangular card with a
//    small icon in the corner.
//
// 2. Driving-safety sizing: Google's own Android Auto design guidelines
//    call for large touch targets and glanceable, low-precision
//    controls -- nothing that needs sustained visual attention or fine
//    motor control while the vehicle is moving. Concretely here: no
//    drag-sliders anywhere in this app any more (settings_screen.cpp's
//    Brightness/Contrast/Saturation/Volume rows are now big +/- stepper
//    buttons via style_step_button(), not lv_slider_create()); every
//    interactive element sized at or above kMinTouchTarget; bigger
//    fonts throughout (LV_FONT_MONTSERRAT_24 for button/value text,
//    _48 for launcher icon glyphs).
//
// theme::init() calls lv_theme_default_init() with this palette and
// dark=true, so every default widget (button, switch, dropdown,
// tabview, textarea) picks up consistent colors automatically without
// per-screen styling -- screens only need the helpers below for the
// things the default theme doesn't cover (cards, icon badges, the
// header affordance, steppers, section labels).
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

// ---- driving-safety sizing -----------------------------------------------
// Google's Android Auto design guidelines specify a 76dp minimum touch
// target; this device's panel isn't dp-scaled (no known DPI figure to
// convert against), so kMinTouchTarget is a direct pixel floor picked
// to look/feel comparable at this screen's real 800x480 -- every
// interactive element created via the helpers below meets or exceeds
// it. Not a substitute for real hardware/usability testing with the
// actual touch digitizer, just the concrete number every style here is
// built around.
constexpr int32_t kMinTouchTarget = 64;
constexpr int32_t kIconBadgeDiameter = 96;

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

// Rounded, bordered surface -- used for content panels/list containers
// (android_auto_screen.cpp's status card, bluetooth_screen.cpp's
// device list panel) -- NOT used for home-screen tiles any more, see
// style_icon_badge().
void style_card(lv_obj_t * obj);

// The circular, accent-filled icon badge behind a launcher tile's
// glyph -- the specific thing that makes home_screen.cpp's tiles read
// as "AA app grid" instead of "bordered card with an icon in it".
// `obj` should be a plain lv_obj_t (not a button -- the tile itself is
// the button; this is just the colored circle drawn behind its icon).
void style_icon_badge(lv_obj_t * obj);

// Accent-filled, rounded call-to-action button (e.g. "Connect
// (Wireless)", "Save", "Refresh") -- replaces the default theme's
// button look where a screen wants to draw attention to one primary
// action. Sized to at least kMinTouchTarget in height.
void style_primary_button(lv_obj_t * btn);

// One half (either the "-" or the "+") of a stepper control -- a big,
// square, accent-outlined button, kMinTouchTarget square. See
// settings_screen.cpp's add_stepper_row(), which replaced every
// drag-slider in this app with one of these pairs: dragging a thin
// slider precisely while driving is exactly the kind of sustained-
// attention/fine-motor-control interaction Android Auto's own design
// guidelines steer away from; a big flat +/- tap is a single glance
// and a single tap.
void style_step_button(lv_obj_t * btn);

// Small accent-colored caps-style label used for section headings
// ("Display", "Audio", "Status", "Notes") -- every screen was already
// hand-coloring these 0x66aaff; this just names the pattern.
void style_section_label(lv_obj_t * label);

// De-emphasized supporting text (addresses, hints, disabled-field
// notes) -- the 0x999999 gray every screen already used ad hoc.
void style_secondary_text(lv_obj_t * label);

// One row button inside an lv_list (bluetooth_screen.cpp's paired-
// device list) -- taller (kMinTouchTarget, LVGL's own default list
// button is well under that) and left-aligned with breathing room,
// replacing the stock lv_list button look (thin row, tight padding,
// small text) that reads as generic-LVGL rather than matching the
// rest of this app's big-target, AA-flavored controls.
void style_list_button(lv_obj_t * btn);

// Adds ONLY the knob/encoder focus-ring outline (LV_STATE_FOCUSED) --
// every other style_*() helper above already includes this, so most
// callers never need it directly. Exists for objects that are
// knob-navigable but don't go through any of those helpers, e.g.
// home_screen.cpp's launcher tiles (hand-styled, not a card/button/
// list-row in the shared-style sense). Without an explicit focus
// indicator, core::navigation::focus_group() still moves the logical
// selection via the knob correctly, but nothing on screen shows which
// item is currently selected -- a real, reported gap this closes.
void style_focusable(lv_obj_t * obj);

}  // namespace ui::theme
