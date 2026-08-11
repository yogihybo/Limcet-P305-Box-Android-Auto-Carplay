#include "ui/home_screen.h"

#include "core/navigation.h"
#include "ui/android_auto_screen.h"
#include "ui/bluetooth_screen.h"
#include "ui/reverse_camera_screen.h"
#include "ui/settings_screen.h"
#include "ui/status_bar.h"
#include "ui/theme.h"

namespace ui {

namespace {

void android_auto_btn_cb(lv_event_t *) {
    core::navigation::push(create_android_auto_screen);
}

void bluetooth_btn_cb(lv_event_t *) {
    core::navigation::push(create_bluetooth_screen);
}

void settings_btn_cb(lv_event_t *) {
    core::navigation::push(create_settings_screen);
}

// Manual preview -- see reverse_camera_screen.cpp's top comment: the
// automatic gear-triggered path (core::ReverseGearWatcher) isn't wired
// into main.cpp yet, so this tile is currently the only way to reach
// this screen at all. Genuinely useful on its own (checking the camera
// works without needing to put the vehicle in reverse), so it stays
// even once the automatic path is wired up.
void reverse_camera_btn_cb(lv_event_t *) {
    core::navigation::push(create_reverse_camera_screen);
}

// One launcher tile: a big solid-accent circular icon badge (see
// ui/theme.h's top comment -- this is the specific thing that makes
// the grid read as "Android Auto app icons" rather than "bordered
// settings-app cards") with a label underneath. No visible border on
// the tile itself -- LVGL applies a soft rounded highlight behind the
// whole tile on press (set below) instead, so the badge alone doesn't
// have to double as the only interactive-looking element.
lv_obj_t * create_tile(lv_obj_t * parent, const char * symbol, const char * label_text,
                        lv_event_cb_t cb) {
    lv_obj_t * tile = lv_button_create(parent);
    lv_obj_remove_style_all(tile);
    lv_obj_set_size(tile, 170, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(tile, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tile, 0, 0);
    lv_obj_set_style_shadow_width(tile, 0, 0);
    lv_obj_set_style_radius(tile, 20, 0);
    lv_obj_set_style_bg_color(tile, theme::surface_pressed(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(tile, LV_OPA_60, LV_STATE_PRESSED);
    // Knob focus ring -- see theme::style_focusable()'s comment. Without
    // this, selecting a tile via the knob moved the logical focus but
    // showed nothing on screen.
    theme::style_focusable(tile);
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(tile, 12, 0);
    lv_obj_set_style_pad_top(tile, 10, 0);
    lv_obj_set_style_pad_bottom(tile, 10, 0);
    lv_obj_add_event_cb(tile, cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t * badge = lv_obj_create(tile);
    lv_obj_remove_style_all(badge);
    lv_obj_set_size(badge, theme::kIconBadgeDiameter, theme::kIconBadgeDiameter);
    theme::style_icon_badge(badge);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t * icon = lv_label_create(badge);
    lv_label_set_text(icon, symbol);
    // Twice the size of the old flat-card icon -- a big, glanceable
    // glyph is the whole point of the badge treatment.
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, 0);
    lv_obj_center(icon);

    lv_obj_t * label = lv_label_create(tile);
    lv_label_set_text(label, label_text);
    lv_obj_set_style_text_color(label, theme::text_primary(), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);

    // Knob navigation -- see hal/knob.h. LVGL auto-removes this from
    // the group when the tile itself is deleted (screen popped/
    // recreated), so no manual cleanup needed here.
    lv_group_add_obj(core::navigation::focus_group(), tile);

    return tile;
}

}  // namespace

lv_obj_t * create_home_screen() {
    lv_obj_t * scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, theme::bg(), 0);

    // Persistent status bar (see ui/status_bar.h) -- same component
    // every other screen gets via theme::create_screen_with_header(),
    // added directly here since home_screen builds its own header
    // (left-aligned title + subtitle, no back button) rather than
    // using that helper.
    status_bar::create(scr);

    // Header -- simple app title, matches the top-mid title convention
    // used by every other screen (settings_screen.cpp,
    // bluetooth_screen.cpp), just left-aligned with a subtitle instead
    // of centered, and without a back button since this is the root of
    // the stack. Offset down by the status bar's height, same reasoning
    // as theme.cpp's create_screen_with_header().
    lv_obj_t * title = lv_label_create(scr);
    lv_label_set_text(title, "Prado");
    lv_obj_set_style_text_color(title, theme::text_primary(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 20, 16 + status_bar::kHeight);

    lv_obj_t * subtitle = lv_label_create(scr);
    lv_label_set_text(subtitle, "Home");
    theme::style_secondary_text(subtitle);
    lv_obj_align_to(subtitle, title, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 4);

    // Tile grid -- centered row, wraps if it ever grows past what fits
    // on one line at 800x480. New destinations get added here as they
    // land (Phase 6+: media, phone, etc. once those exist).
    lv_obj_t * grid = lv_obj_create(scr);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, LV_PCT(94), LV_SIZE_CONTENT);
    lv_obj_align(grid, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(grid, 0, 0);
    // Vertical padding isn't for the tiles themselves -- it's room for
    // theme::style_focusable()'s knob focus ring, which draws OUTSIDE
    // each tile's own box (outline_width 3 + outline_pad 3 = ~6px).
    // LVGL clips a child's drawing to its PARENT's bounds, and this
    // grid is LV_SIZE_CONTENT with zero margin around its one row of
    // tiles, so the ring was getting clipped top/bottom against the
    // grid's own edge -- confirmed on real hardware.
    lv_obj_set_style_pad_ver(grid, 8, 0);
    lv_obj_set_style_pad_row(grid, 20, 0);
    lv_obj_set_style_pad_column(grid, 16, 0);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    create_tile(grid, LV_SYMBOL_USB, "Android Auto", android_auto_btn_cb);
    create_tile(grid, LV_SYMBOL_BLUETOOTH, "Bluetooth", bluetooth_btn_cb);
    create_tile(grid, LV_SYMBOL_EYE_OPEN, "Reverse Camera", reverse_camera_btn_cb);
    create_tile(grid, LV_SYMBOL_SETTINGS, "Settings", settings_btn_cb);

    return scr;
}

}  // namespace ui
