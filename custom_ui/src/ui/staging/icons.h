#pragma once

#include "lvgl.h"

namespace staging_ui::icons {

extern const lv_image_dsc_t icon_nav_home;
extern const lv_image_dsc_t icon_nav_navigation;
extern const lv_image_dsc_t icon_nav_bluetooth;
extern const lv_image_dsc_t icon_nav_camera;
extern const lv_image_dsc_t icon_nav_settings;
extern const lv_image_dsc_t icon_brightness;
extern const lv_image_dsc_t icon_contrast;
extern const lv_image_dsc_t icon_saturation;
extern const lv_image_dsc_t icon_phone;
extern const lv_image_dsc_t icon_smartphone;
extern const lv_image_dsc_t icon_volume;
extern const lv_image_dsc_t icon_bell;
extern const lv_image_dsc_t icon_prev;
extern const lv_image_dsc_t icon_play;
extern const lv_image_dsc_t icon_pause;
extern const lv_image_dsc_t icon_next;
extern const lv_image_dsc_t icon_plus;
extern const lv_image_dsc_t icon_minus;

// Helper to create an icon widget styled and recolored
lv_obj_t * create_icon(lv_obj_t * parent, const lv_image_dsc_t * dsc, lv_color_t color);

} // namespace staging_ui::icons

// Backward compatibility alias
namespace ui::icons {
    using namespace staging_ui::icons;
}
