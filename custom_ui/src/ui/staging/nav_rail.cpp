#include "ui/staging/nav_rail.h"
#include "ui/staging/theme.h"
#include "ui/staging/fonts.h"
#include "ui/staging/home_dashboard.h"
#include "ui/staging/settings_screen.h"
#include "ui/android_auto_screen.h"
#include "ui/bluetooth_screen.h"
#include "ui/reverse_camera_screen.h"
#include "ui/staging/icons.h"
#include "core/navigation.h"

namespace staging_ui {

namespace {

struct PersistentRail {
    lv_obj_t * rail = nullptr;
    lv_obj_t * buttons[5] = {nullptr};
    lv_obj_t * icons[5] = {nullptr};
    const lv_image_dsc_t * icon_dscs[5] = {
        &ui::icons::icon_nav_home,
        &ui::icons::icon_nav_navigation,
        &ui::icons::icon_nav_bluetooth,
        &ui::icons::icon_nav_camera,
        &ui::icons::icon_nav_settings
    };
    NavDestination current_dest = NavDestination::Home;
    NavCallback custom_callback = nullptr;
};

PersistentRail & rail_instance() {
    static PersistentRail instance;
    return instance;
}

void nav_btn_cb(lv_event_t * e) {
    auto dest = static_cast<NavDestination>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
    auto & inst = rail_instance();
    if (inst.custom_callback) {
        inst.custom_callback(dest);
    } else {
        navigate_to(dest);
    }
}

void update_active_button(NavDestination dest) {
    auto & inst = rail_instance();
    inst.current_dest = dest;
    for (int i = 0; i < 5; ++i) {
        if (!inst.buttons[i]) continue;
        bool active = (i == static_cast<int>(dest));
        theme::style_nav_button(inst.buttons[i], active);
        if (inst.icons[i]) {
            lv_color_t c = active ? theme::text_on_accent() : theme::text_primary();
            lv_obj_set_style_image_recolor(inst.icons[i], c, 0);
        }
    }
}

lv_obj_t * ensure_persistent_rail() {
    auto & inst = rail_instance();
    if (inst.rail && lv_obj_is_valid(inst.rail)) {
        return inst.rail;
    }

    // Place the persistent navigation rail on lv_layer_top() so it remains
    // 100% stationary and never slides during screen load animations.
    lv_obj_t * top_layer = lv_layer_top();
    inst.rail = lv_obj_create(top_layer);
    theme::style_nav_rail(inst.rail);
    lv_obj_align(inst.rail, LV_ALIGN_LEFT_MID, 0, 0);

    for (int i = 0; i < 5; ++i) {
        auto dest = static_cast<NavDestination>(i);
        lv_obj_t * btn = lv_button_create(inst.rail);
        lv_obj_remove_style_all(btn);
        inst.buttons[i] = btn;
        theme::style_nav_button(btn, false);
        lv_obj_add_event_cb(btn, nav_btn_cb, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<uintptr_t>(dest)));

        lv_obj_t * icon = ui::icons::create_icon(btn, inst.icon_dscs[i], theme::text_primary());
        inst.icons[i] = icon;
        lv_obj_center(icon);

        if (core::navigation::focus_group()) {
            lv_group_add_obj(core::navigation::focus_group(), btn);
        }
    }

    return inst.rail;
}

} // namespace

void navigate_to(NavDestination dest) {
    update_active_button(dest);

    switch (dest) {
        case NavDestination::Home:
            if (core::navigation::depth() > 1) {
                core::navigation::pop();
            } else {
                core::navigation::replace(create_home_dashboard);
            }
            break;
        case NavDestination::AndroidAuto:
            core::navigation::replace(ui::create_android_auto_screen);
            break;
        case NavDestination::Bluetooth:
            core::navigation::replace(ui::create_bluetooth_screen);
            break;
        case NavDestination::Camera:
            core::navigation::replace(ui::create_reverse_camera_screen);
            break;
        case NavDestination::Settings:
            core::navigation::replace(create_settings_screen);
            break;
    }
}

lv_obj_t * create_nav_rail(lv_obj_t * /*parent*/, NavDestination active_dest, NavCallback cb) {
    lv_obj_t * rail = ensure_persistent_rail();
    auto & inst = rail_instance();
    inst.custom_callback = std::move(cb);
    update_active_button(active_dest);
    return rail;
}

} // namespace staging_ui
