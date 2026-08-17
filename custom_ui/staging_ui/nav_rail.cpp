#include "staging_ui/nav_rail.h"
#include "staging_ui/theme.h"
#include "staging_ui/fonts.h"
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
    }
}

void destroy_rail_ctx(lv_event_t * e) {
    delete static_cast<RailCtx *>(lv_event_get_user_data(e));
}

lv_obj_t * add_rail_button(lv_obj_t * rail, const char * symbol, NavDestination dest, bool active) {
    lv_obj_t * btn = lv_button_create(rail);
    theme::style_nav_button(btn, active);
    lv_obj_add_event_cb(btn, nav_btn_cb, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<uintptr_t>(dest)));

    lv_obj_t * icon = lv_label_create(btn);
    lv_label_set_text(icon, symbol);
    lv_obj_set_style_text_font(icon, &lv_font_roboto_24, 0);
    lv_obj_set_style_text_color(icon, active ? theme::text_on_accent() : theme::text_primary(), 0);
    lv_obj_center(icon);

    if (core::navigation::focus_group()) {
        lv_group_add_obj(core::navigation::focus_group(), btn);
    }

    return btn;
}

} // namespace

lv_obj_t * create_nav_rail(lv_obj_t * parent, NavDestination active_dest, NavCallback cb) {
    lv_obj_t * rail = lv_obj_create(parent);
    theme::style_nav_rail(rail);
    lv_obj_align(rail, LV_ALIGN_LEFT_MID, 0, 0);

    auto * ctx = new RailCtx{std::move(cb)};
    lv_obj_set_user_data(rail, ctx);
    lv_obj_add_event_cb(rail, destroy_rail_ctx, LV_EVENT_DELETE, ctx);

    // Exact 5 Icons matching the mockup from top to bottom:
    // 1. Home
    add_rail_button(rail, LV_SYMBOL_HOME, NavDestination::Home, active_dest == NavDestination::Home);

    // 2. Android Auto (Navigation arrow / GPS symbol)
    add_rail_button(rail, LV_SYMBOL_GPS, NavDestination::AndroidAuto, active_dest == NavDestination::AndroidAuto);

    // 3. Bluetooth
    add_rail_button(rail, LV_SYMBOL_BLUETOOTH, NavDestination::Bluetooth, active_dest == NavDestination::Bluetooth);

    // 4. Camera (Eye / Camera symbol)
    add_rail_button(rail, LV_SYMBOL_EYE_OPEN, NavDestination::Camera, active_dest == NavDestination::Camera);

    // 5. Settings (Gear symbol)
    add_rail_button(rail, LV_SYMBOL_SETTINGS, NavDestination::Settings, active_dest == NavDestination::Settings);

    return rail;
}

} // namespace staging_ui
