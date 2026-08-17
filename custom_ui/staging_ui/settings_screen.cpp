#include "staging_ui/settings_screen.h"
#include "staging_ui/theme.h"
#include "staging_ui/fonts.h"
#include "staging_ui/nav_rail.h"
#include "core/config_store.h"
#include "core/navigation.h"
#include "hal/display_ctrl.h"
#include "ui/android_auto_screen.h"
#include "ui/bluetooth_screen.h"
#include "ui/reverse_camera_screen.h"

namespace staging_ui {

namespace {

hal::DisplayCtrlHandle & display_handle() {
    static hal::DisplayCtrlHandle handle;
    static bool tried = false;
    if (!tried) {
        hal::init_display_ctrl(handle);
        tried = true;
    }
    return handle;
}

enum class SettingsTab {
    Display,
    Audio,
    System
};

enum class VdeField { None, Brightness, Contrast, Saturation, Hue };

struct StepperCtx {
    std::string section;
    std::string key;
    VdeField vde_field;
    int min;
    int max;
    int step;
    int value;
    lv_obj_t * value_label;
    lv_obj_t * level_bar;
};

struct StepperBtnCtx {
    StepperCtx * shared;
    int dir;
};

void destroy_stepper_ctx(lv_event_t * e) {
    delete static_cast<StepperCtx *>(lv_event_get_user_data(e));
}

void destroy_btn_ctx(lv_event_t * e) {
    delete static_cast<StepperBtnCtx *>(lv_event_get_user_data(e));
}

void apply_vde(VdeField field, int value) {
    if (field == VdeField::None) return;
    hal::DisplayCtrlHandle & h = display_handle();
    hal::VdeConfig cfg;
    hal::get_vde_config(h, hal::DisplayLayer::Osd1, cfg);
    switch (field) {
        case VdeField::Brightness: cfg.brightness = static_cast<unsigned int>(value); break;
        case VdeField::Contrast:   cfg.contrast = static_cast<unsigned int>(value); break;
        case VdeField::Saturation: cfg.saturation = static_cast<unsigned int>(value); break;
        case VdeField::Hue:        cfg.hue = static_cast<unsigned int>(value); break;
        default: break;
    }
    hal::set_vde_config(h, hal::DisplayLayer::Osd1, cfg);
}

void stepper_click_cb(lv_event_t * e) {
    auto * btn_ctx = static_cast<StepperBtnCtx *>(lv_event_get_user_data(e));
    StepperCtx * ctx = btn_ctx->shared;

    int new_val = ctx->value + btn_ctx->dir * ctx->step;
    if (new_val < ctx->min) new_val = ctx->min;
    if (new_val > ctx->max) new_val = ctx->max;
    if (new_val == ctx->value) return;

    ctx->value = new_val;
    if (ctx->value_label) {
        lv_label_set_text_fmt(ctx->value_label, "%d", ctx->value);
    }
    if (ctx->level_bar) {
        lv_bar_set_value(ctx->level_bar, ctx->value, LV_ANIM_ON);
    }

    core::default_store().set_int(ctx->key, ctx->value, ctx->section);
    apply_vde(ctx->vde_field, ctx->value);
    core::default_store().save();
}

lv_obj_t * create_stepper_row(lv_obj_t * parent, const char * symbol, const char * label_text,
                             int min, int max, int step, const std::string & key,
                             const std::string & section, VdeField vde_field) {
    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 72);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(row, 8, 0);

    // Left Icon + Label
    lv_obj_t * left_box = lv_obj_create(row);
    lv_obj_remove_style_all(left_box);
    lv_obj_set_size(left_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(left_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(left_box, 14, 0);

    if (symbol && symbol[0] != '\0') {
        lv_obj_t * icon = lv_label_create(left_box);
        lv_label_set_text(icon, symbol);
        lv_obj_set_style_text_font(icon, &lv_font_roboto_20, 0);
        lv_obj_set_style_text_color(icon, theme::text_secondary(), 0);
    }

    lv_obj_t * label = lv_label_create(left_box);
    lv_label_set_text(label, label_text);
    lv_obj_set_style_text_font(label, &lv_font_roboto_20, 0);
    lv_obj_set_style_text_color(label, theme::text_primary(), 0);

    // Initial Value
    int initial = core::default_store().get_int(key, (min + max) / 2, section);
    if (initial < min) initial = min;
    if (initial > max) initial = max;
    initial = min + ((initial - min) / step) * step;  // snap to the step grid

    auto * ctx = new StepperCtx{section, key, vde_field, min, max, step, initial, nullptr, nullptr};
    lv_obj_add_event_cb(row, destroy_stepper_ctx, LV_EVENT_DELETE, ctx);

    // Right Controls (Percentage + Level Bar + Minus + Plus)
    lv_obj_t * right_box = lv_obj_create(row);
    lv_obj_remove_style_all(right_box);
    lv_obj_set_size(right_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(right_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_box, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right_box, 16, 0);

    lv_obj_t * val_lbl = lv_label_create(right_box);
    lv_label_set_text_fmt(val_lbl, "%d", initial);
    lv_obj_set_style_text_font(val_lbl, &lv_font_roboto_20, 0);
    lv_obj_set_style_text_color(val_lbl, theme::text_primary(), 0);
    lv_obj_set_width(val_lbl, 56);
    lv_obj_set_style_text_align(val_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    ctx->value_label = val_lbl;

    // Horizontal Progress Level Bar
    lv_obj_t * bar = lv_bar_create(right_box);
    theme::style_level_bar(bar);
    lv_bar_set_range(bar, min, max);
    lv_bar_set_value(bar, initial, LV_ANIM_OFF);
    ctx->level_bar = bar;

    // Minus Button
    lv_obj_t * minus_btn = lv_button_create(right_box);
    theme::style_stepper_button(minus_btn);
    auto * minus_ctx = new StepperBtnCtx{ctx, -1};
    lv_obj_add_event_cb(minus_btn, destroy_btn_ctx, LV_EVENT_DELETE, minus_ctx);
    lv_obj_add_event_cb(minus_btn, stepper_click_cb, LV_EVENT_CLICKED, minus_ctx);
    lv_obj_t * minus_lbl = lv_label_create(minus_btn);
    lv_label_set_text(minus_lbl, LV_SYMBOL_MINUS);
    lv_obj_set_style_text_font(minus_lbl, &lv_font_roboto_20, 0);
    lv_obj_set_style_text_color(minus_lbl, theme::text_on_accent(), 0);
    lv_obj_center(minus_lbl);

    // Plus Button
    lv_obj_t * plus_btn = lv_button_create(right_box);
    theme::style_stepper_button(plus_btn);
    auto * plus_ctx = new StepperBtnCtx{ctx, 1};
    lv_obj_add_event_cb(plus_btn, destroy_btn_ctx, LV_EVENT_DELETE, plus_ctx);
    lv_obj_add_event_cb(plus_btn, stepper_click_cb, LV_EVENT_CLICKED, plus_ctx);
    lv_obj_t * plus_lbl = lv_label_create(plus_btn);
    lv_label_set_text(plus_lbl, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_font(plus_lbl, &lv_font_roboto_20, 0);
    lv_obj_set_style_text_color(plus_lbl, theme::text_on_accent(), 0);
    lv_obj_center(plus_lbl);

    if (core::navigation::focus_group()) {
        lv_group_add_obj(core::navigation::focus_group(), minus_btn);
        lv_group_add_obj(core::navigation::focus_group(), plus_btn);
    }

    // Push the seeded/persisted value to hardware once at build time --
    // matches src/ui/settings_screen.cpp's own add_stepper_row(), which
    // does this specifically so the display stays in sync with
    // whatever was last saved every time this screen is (re)built, not
    // just from the next +/- tap.
    if (vde_field != VdeField::None) {
        apply_vde(vde_field, initial);
    }

    return row;
}

struct ScreenState {
    SettingsTab current_tab = SettingsTab::Display;
    lv_obj_t * card_container = nullptr;
    lv_obj_t * chip_display = nullptr;
    lv_obj_t * chip_audio = nullptr;
    lv_obj_t * chip_system = nullptr;
};

void render_tab_content(ScreenState * state) {
    if (!state->card_container) return;
    lv_obj_clean(state->card_container);

    // Update Filter Chips Active Appearance
    theme::style_filter_chip(state->chip_display, state->current_tab == SettingsTab::Display);
    theme::style_filter_chip(state->chip_audio, state->current_tab == SettingsTab::Audio);
    theme::style_filter_chip(state->chip_system, state->current_tab == SettingsTab::System);

    lv_obj_t * lbl_d = lv_obj_get_child(state->chip_display, 0);
    if (lbl_d) lv_obj_set_style_text_color(lbl_d, state->current_tab == SettingsTab::Display ? theme::text_on_accent() : theme::text_primary(), 0);

    lv_obj_t * lbl_a = lv_obj_get_child(state->chip_audio, 0);
    if (lbl_a) lv_obj_set_style_text_color(lbl_a, state->current_tab == SettingsTab::Audio ? theme::text_on_accent() : theme::text_primary(), 0);

    lv_obj_t * lbl_s = lv_obj_get_child(state->chip_system, 0);
    if (lbl_s) lv_obj_set_style_text_color(lbl_s, state->current_tab == SettingsTab::System ? theme::text_on_accent() : theme::text_primary(), 0);

    if (state->current_tab == SettingsTab::Display) {
        // 0-255 (not 0-100) and section "General" (not "Display") --
        // matches the real hal::VdeConfig register range and the
        // config_store section src/ui/settings_screen.cpp's own
        // Brightness/Contrast/Saturation rows already use. The old
        // 0-100 range capped real hardware brightness/contrast at
        // ~39% of its actual maximum, and "Display" was a section no
        // other code in this app reads from.
        create_stepper_row(state->card_container, LV_SYMBOL_EYE_OPEN, "Brightness", 0, 255, 5,
                           "Brightness", "General", VdeField::Brightness);
        create_stepper_row(state->card_container, LV_SYMBOL_IMAGE, "Contrast", 0, 255, 5,
                           "Contrast", "General", VdeField::Contrast);
        create_stepper_row(state->card_container, LV_SYMBOL_SETTINGS, "Saturation", 0, 255, 5,
                           "Saturation", "General", VdeField::Saturation);
    } else if (state->current_tab == SettingsTab::Audio) {
        // Audio Tab: Media & Guidance Volume
        create_stepper_row(state->card_container, LV_SYMBOL_AUDIO, "Media Volume", 0, 100, 5,
                           "MediaVolume", "Audio", VdeField::None);
        create_stepper_row(state->card_container, LV_SYMBOL_BELL, "Guidance Volume", 0, 100, 5,
                           "GuidanceVolume", "Audio", VdeField::None);
        create_stepper_row(state->card_container, LV_SYMBOL_VOLUME_MAX, "System Volume", 0, 100, 5,
                           "SystemVolume", "Audio", VdeField::None);
    } else if (state->current_tab == SettingsTab::System) {
        // System Tab: Auto-Start Switch + Reset
        lv_obj_t * row = lv_obj_create(state->card_container);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, 72);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(row, 8, 0);

        lv_obj_t * label = lv_label_create(row);
        lv_label_set_text(label, "Auto-Start CarLink");
        lv_obj_set_style_text_font(label, &lv_font_roboto_20, 0);
        lv_obj_set_style_text_color(label, theme::text_primary(), 0);

        lv_obj_t * sw = lv_switch_create(row);
        bool auto_start = core::default_store().get_bool("AutoStartCarLink", true, "General");
        if (auto_start) lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_add_event_cb(sw, [](lv_event_t * e) {
            lv_obj_t * target = static_cast<lv_obj_t *>(lv_event_get_target(e));
            bool val = lv_obj_has_state(target, LV_STATE_CHECKED);
            core::default_store().set_bool("AutoStartCarLink", val, "General");
            core::default_store().save();
        }, LV_EVENT_VALUE_CHANGED, nullptr);

        if (core::navigation::focus_group()) {
            lv_group_add_obj(core::navigation::focus_group(), sw);
        }
    }
}

void chip_clicked_cb(lv_event_t * e) {
    auto tab = static_cast<SettingsTab>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
    auto * state = static_cast<ScreenState *>(lv_obj_get_user_data(static_cast<lv_obj_t *>(lv_event_get_target(e))));
    if (state && state->current_tab != tab) {
        state->current_tab = tab;
        render_tab_content(state);
    }
}

void destroy_screen_state(lv_event_t * e) {
    delete static_cast<ScreenState *>(lv_event_get_user_data(e));
}

} // namespace

lv_obj_t * create_settings_screen() {
    lv_obj_t * scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, theme::bg(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    auto * state = new ScreenState();
    lv_obj_set_user_data(scr, state);
    lv_obj_add_event_cb(scr, destroy_screen_state, LV_EVENT_DELETE, state);

    // 1. Persistent 5-Icon Navigation Rail on the Left (Settings is active)
    create_nav_rail(scr, NavDestination::Settings, [](NavDestination dest) {
        switch (dest) {
            case NavDestination::Home:
                core::navigation::pop();
                break;
            case NavDestination::AndroidAuto:
                core::navigation::push(ui::create_android_auto_screen);
                break;
            case NavDestination::Bluetooth:
                core::navigation::push(ui::create_bluetooth_screen);
                break;
            case NavDestination::Camera:
                core::navigation::push(ui::create_reverse_camera_screen);
                break;
            case NavDestination::Settings:
                break;
        }
    });

    // 2. Main Content Area to the right of the rail
    lv_obj_t * content = lv_obj_create(scr);
    lv_obj_remove_style_all(content);
    lv_obj_set_pos(content, theme::kRailWidth + 24, 16);
    lv_obj_set_size(content, 800 - (theme::kRailWidth + 48), 448);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content, 18, 0);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    // 3. Top Header Bar (Title & Filter Chips)
    lv_obj_t * header_row = lv_obj_create(content);
    lv_obj_remove_style_all(header_row);
    lv_obj_set_width(header_row, LV_PCT(100));
    lv_obj_set_height(header_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(header_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(header_row, 16, 0);

    // Segmented Filter Chips
    auto make_chip = [scr, state, header_row](const char * text, SettingsTab tab) -> lv_obj_t * {
        lv_obj_t * chip = lv_button_create(header_row);
        lv_obj_set_user_data(chip, state);
        lv_obj_add_event_cb(chip, chip_clicked_cb, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<uintptr_t>(tab)));

        lv_obj_t * lbl = lv_label_create(chip);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_font(lbl, &lv_font_roboto_20, 0);
        lv_obj_center(lbl);

        if (core::navigation::focus_group()) {
            lv_group_add_obj(core::navigation::focus_group(), chip);
        }
        return chip;
    };

    state->chip_display = make_chip("Display", SettingsTab::Display);
    state->chip_audio   = make_chip("Audio", SettingsTab::Audio);
    state->chip_system  = make_chip("System", SettingsTab::System);

    // 4. Central Rounded Card
    lv_obj_t * card = lv_obj_create(content);
    theme::style_card(card);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(card, 10, 0);
    state->card_container = card;

    // Render Initial Display Tab Content
    render_tab_content(state);

    return scr;
}

} // namespace staging_ui
