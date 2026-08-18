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

struct RailCtx {
    NavCallback callback;
};

void nav_btn_cb(lv_event_t * e) {
    auto dest = static_cast<NavDestination>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
    auto * rail = lv_obj_get_parent(static_cast<lv_obj_t *>(lv_event_get_target(e)));
    auto * ctx = static_cast<RailCtx *>(lv_obj_get_user_data(rail));
    if (ctx && ctx->callback) {
        ctx->callback(dest);
    } else {
        navigate_to(dest);
    }
}

void destroy_rail_ctx(lv_event_t * e) {
    delete static_cast<RailCtx *>(lv_event_get_user_data(e));
}

lv_obj_t * add_rail_button(lv_obj_t * rail, const lv_image_dsc_t * icon_dsc, NavDestination dest, bool active) {
    lv_obj_t * btn = lv_button_create(rail);
    theme::style_nav_button(btn, active);
    lv_obj_add_event_cb(btn, nav_btn_cb, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<uintptr_t>(dest)));

    lv_color_t icon_color = active ? theme::text_on_accent() : theme::text_primary();
    lv_obj_t * icon = ui::icons::create_icon(btn, icon_dsc, icon_color);
    lv_obj_center(icon);

    if (core::navigation::focus_group()) {
        lv_group_add_obj(core::navigation::focus_group(), btn);
    }

    return btn;
}

} // namespace

void navigate_to(NavDestination dest) {
    switch (dest) {
        case NavDestination::Home:
            if (core::navigation::depth() > 1) {
                core::navigation::pop();
            }
            break;
        case NavDestination::AndroidAuto:
            if (core::navigation::depth() > 1) {
                core::navigation::replace(ui::create_android_auto_screen);
            } else {
                core::navigation::push(ui::create_android_auto_screen);
            }
            break;
        case NavDestination::Bluetooth:
            if (core::navigation::depth() > 1) {
                core::navigation::replace(ui::create_bluetooth_screen);
            } else {
                core::navigation::push(ui::create_bluetooth_screen);
            }
            break;
        case NavDestination::Camera:
            if (core::navigation::depth() > 1) {
                core::navigation::replace(ui::create_reverse_camera_screen);
            } else {
                core::navigation::push(ui::create_reverse_camera_screen);
            }
            break;
        case NavDestination::Settings:
            if (core::navigation::depth() > 1) {
                core::navigation::replace(create_settings_screen);
            } else {
                core::navigation::push(create_settings_screen);
            }
            break;
    }
}

lv_obj_t * create_nav_rail(lv_obj_t * parent, NavDestination active_dest, NavCallback cb) {
    lv_obj_t * rail = lv_obj_create(parent);
    theme::style_nav_rail(rail);
    lv_obj_align(rail, LV_ALIGN_LEFT_MID, 0, 0);

    auto * ctx = new RailCtx{std::move(cb)};
    lv_obj_set_user_data(rail, ctx);
    lv_obj_add_event_cb(rail, destroy_rail_ctx, LV_EVENT_DELETE, ctx);

    // Exact 5 Icons matching the mockup from top to bottom:
    // 1. Home (Material house icon)
    add_rail_button(rail, &ui::icons::icon_nav_home, NavDestination::Home, active_dest == NavDestination::Home);

    // 2. Android Auto (Material Navigation chevron/arrowhead)
    add_rail_button(rail, &ui::icons::icon_nav_navigation, NavDestination::AndroidAuto, active_dest == NavDestination::AndroidAuto);

    // 3. Bluetooth (Material Bluetooth Rune)
    add_rail_button(rail, &ui::icons::icon_nav_bluetooth, NavDestination::Bluetooth, active_dest == NavDestination::Bluetooth);

    // 4. Camera (Material Camera icon)
    add_rail_button(rail, &ui::icons::icon_nav_camera, NavDestination::Camera, active_dest == NavDestination::Camera);

    // 5. Settings (Material 6-tooth Gear)
    add_rail_button(rail, &ui::icons::icon_nav_settings, NavDestination::Settings, active_dest == NavDestination::Settings);

    return rail;
}

} // namespace staging_ui
