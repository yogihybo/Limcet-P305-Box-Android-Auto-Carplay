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

// 2026-09-06: real hardware bug found via code review -- the 5 rail
// buttons are created ONCE (on lv_layer_top(), shared across every
// screen) and used to be added to core::navigation::focus_group() only
// at that one creation moment. After the per-screen lv_group_t fix
// (see core::ScreenManager's own header comment), that meant only
// whichever screen happened to be active the very first time
// create_nav_rail() ever ran (the boot screen) had the rail buttons in
// its group -- every other screen's own, separate group never got
// them, so rotating the knob anywhere else just looped on that
// screen's single action button. create_nav_rail() now calls this on
// every invocation (moving the buttons is real work regardless of
// whether the rail itself needed recreating), and ScreenManager also
// calls it directly on pop() -- popping restores a screen WITHOUT
// re-running its factory()/create_nav_rail() at all, so nothing else
// would ever re-attach the (by then, likely moved-elsewhere) rail
// buttons to that restored screen's own group.
void attach_nav_rail_to_group(lv_group_t * group);

} // namespace staging_ui
