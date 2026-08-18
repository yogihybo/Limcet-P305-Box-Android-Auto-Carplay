#pragma once

#include "lvgl.h"
#include <cstdint>

namespace staging_ui::theme {

// ---- Google Material 3 Automotive Tonal Palette --------------------------
inline lv_color_t bg()                    { return lv_color_hex(0x111318); }
inline lv_color_t surface()               { return lv_color_hex(0x1c2024); }
inline lv_color_t surface_card()          { return lv_color_hex(0x1a1d22); }
inline lv_color_t surface_container_high(){ return lv_color_hex(0x282c34); }
inline lv_color_t surface_border()        { return lv_color_hex(0x2f3540); }
inline lv_color_t surface_pressed()       { return lv_color_hex(0x353b47); }

inline lv_color_t accent_primary()        { return lv_color_hex(0x8ab4f8); } // Google Blue
inline lv_color_t accent_secondary()      { return lv_color_hex(0x78d9ec); } // Cyan / Teal
inline lv_color_t accent_glow()           { return lv_color_hex(0x385c96); }

inline lv_color_t text_primary()          { return lv_color_hex(0xe2e2e9); }
inline lv_color_t text_secondary()        { return lv_color_hex(0x90909a); }
inline lv_color_t text_on_accent()        { return lv_color_hex(0x111318); }

inline lv_color_t track_bg()              { return lv_color_hex(0x282c35); }
inline lv_color_t track_fill()            { return lv_color_hex(0x8ab4f8); }

inline lv_color_t success()               { return lv_color_hex(0x81c995); } // M3 Soft Green
inline lv_color_t danger()                { return lv_color_hex(0xf28b82); } // M3 Soft Red

// ---- Sizing & Geometry ---------------------------------------------------
constexpr int32_t kRailWidth = 72;
constexpr int32_t kMinTouchTarget = 64;
constexpr int32_t kCardRadius = 20;
constexpr int32_t kPillRadius = LV_RADIUS_CIRCLE;
constexpr int32_t kStepperBtnSize = 48;

// ---- Global Theme Setup --------------------------------------------------
void init(lv_display_t * disp);

// ---- Element Styling Helpers ---------------------------------------------
void style_card(lv_obj_t * obj);
void style_nav_rail(lv_obj_t * obj);
void style_nav_button(lv_obj_t * btn, bool active);
void style_filter_chip(lv_obj_t * chip, bool active);
void style_stepper_button(lv_obj_t * btn);
void style_level_bar(lv_obj_t * bar);
void style_focusable(lv_obj_t * obj);

} // namespace staging_ui::theme
