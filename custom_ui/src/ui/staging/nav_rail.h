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

// Creates the persistent left 5-icon navigation rail dock on `parent`.
// If cb is null/empty, standard top-level navigation (navigate_to) is used.
lv_obj_t * create_nav_rail(lv_obj_t * parent, NavDestination active_dest, NavCallback cb = nullptr);

// Standard top-level navigation routing between the 5 destinations
void navigate_to(NavDestination dest);

} // namespace staging_ui
