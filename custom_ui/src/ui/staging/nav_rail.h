#pragma once

#include "lvgl.h"
#include <functional>

namespace staging_ui {

enum class NavDestination {
    Home,
    AndroidAuto,
    Bluetooth,
    Camera,
    Settings
};

// Callback invoked when a navigation rail icon is tapped
using NavCallback = std::function<void(NavDestination)>;

// Creates the persistent left 5-icon navigation rail dock on `parent`
lv_obj_t * create_nav_rail(lv_obj_t * parent, NavDestination active_dest, NavCallback cb);

} // namespace staging_ui
