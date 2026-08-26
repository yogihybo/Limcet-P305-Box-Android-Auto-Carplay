#include "ui/staging/settings_screen.h"
#include "ui/staging/theme.h"
#include "ui/staging/fonts.h"
#include "ui/staging/nav_rail.h"
#include "ui/staging/icons.h"
#include "core/config_store.h"
#include "core/navigation.h"
#include "hal/audio.h"
#include "hal/display_ctrl.h"
#include "core/log_timing.h"
#include <functional>

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
    // Extra hardware apply beyond the VDE display path -- currently only
    // used for the Audio section's per-stream ALSA volume (hal::audio.h's
    // set_stream_volume(), a real independent mixer per stream, see that
    // header's own comment). nullptr for rows with no such hardware
    // effect (e.g. VdeField-only Display rows).
    std::function<void(int)> extra_apply;
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
    if (ctx->extra_apply) {
        ctx->extra_apply(ctx->value);
    }
    core::default_store().save();
}

lv_obj_t * create_section_header(lv_obj_t * parent, const char * title) {
    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(row, 8, 0);
    lv_obj_set_style_pad_bottom(row, 4, 0);
    lv_obj_set_style_pad_hor(row, 16, 0);

    lv_obj_t * lbl = lv_label_create(row);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_font(lbl, &lv_font_roboto_14, 0);
    lv_obj_set_style_text_color(lbl, theme::accent_primary(), 0);
    return row;
}

lv_obj_t * create_stepper_row(lv_obj_t * parent, const lv_image_dsc_t * icon_dsc, const char * label_text,
                             int min, int max, int step, const std::string & key,
                             const std::string & section, VdeField vde_field,
                             std::function<void(int)> extra_apply = nullptr) {
    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 64);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(row, 16, 0);

    // Left Icon + Label
    lv_obj_t * left_box = lv_obj_create(row);
    lv_obj_remove_style_all(left_box);
    lv_obj_set_size(left_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(left_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(left_box, 14, 0);

    if (icon_dsc) {
        lv_obj_t * icon = ui::icons::create_icon(left_box, icon_dsc, theme::text_secondary());
        (void)icon;
    }

    lv_obj_t * label = lv_label_create(left_box);
    lv_label_set_text(label, label_text);
    lv_obj_set_style_text_font(label, &lv_font_roboto_20, 0);
    lv_obj_set_style_text_color(label, theme::text_primary(), 0);

    // Initial Value
    int initial = core::default_store().get_int(key, (min + max) / 2, section);
    if (initial < min) initial = min;
    if (initial > max) initial = max;
    initial = min + ((initial - min) / step) * step;

    auto * ctx = new StepperCtx{section, key, vde_field, min, max, step, initial, nullptr, nullptr, extra_apply};
    lv_obj_add_event_cb(row, destroy_stepper_ctx, LV_EVENT_DELETE, ctx);

    // Right Controls (Percentage + Level Bar + Minus + Plus)
    lv_obj_t * right_box = lv_obj_create(row);
    lv_obj_remove_style_all(right_box);
    lv_obj_set_size(right_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(right_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_box, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right_box, 16, 0);
    lv_obj_set_style_pad_all(right_box, 6, 0);
    lv_obj_set_style_clip_corner(right_box, false, 0);

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
    lv_obj_t * minus_icon = ui::icons::create_icon(minus_btn, &ui::icons::icon_minus, theme::text_primary());
    lv_obj_set_style_image_recolor(minus_icon, theme::text_on_accent(), LV_STATE_FOCUSED);
    lv_obj_center(minus_icon);

    // Plus Button
    lv_obj_t * plus_btn = lv_button_create(right_box);
    theme::style_stepper_button(plus_btn);
    auto * plus_ctx = new StepperBtnCtx{ctx, 1};
    lv_obj_add_event_cb(plus_btn, destroy_btn_ctx, LV_EVENT_DELETE, plus_ctx);
    lv_obj_add_event_cb(plus_btn, stepper_click_cb, LV_EVENT_CLICKED, plus_ctx);
    lv_obj_t * plus_icon = ui::icons::create_icon(plus_btn, &ui::icons::icon_plus, theme::text_primary());
    lv_obj_set_style_image_recolor(plus_icon, theme::text_on_accent(), LV_STATE_FOCUSED);
    lv_obj_center(plus_icon);

    if (core::navigation::focus_group()) {
        lv_group_add_obj(core::navigation::focus_group(), minus_btn);
        lv_group_add_obj(core::navigation::focus_group(), plus_btn);
    }

    apply_vde(vde_field, initial);
    if (extra_apply) {
        extra_apply(initial);
    }

    return row;
}

struct ToggleCtx {
    std::string key;
    std::string section;
    std::function<void(bool)> onChange;
};

void destroy_toggle_ctx(lv_event_t * e) {
    delete static_cast<ToggleCtx *>(lv_event_get_user_data(e));
}

lv_obj_t * create_toggle_row(lv_obj_t * parent, const lv_image_dsc_t * icon_dsc, const char * label_text,
                            const std::string & key, const std::string & section, bool def_val,
                            std::function<void(bool)> on_change = nullptr) {
    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 64);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(row, 16, 0);

    lv_obj_t * left_box = lv_obj_create(row);
    lv_obj_remove_style_all(left_box);
    lv_obj_set_size(left_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(left_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(left_box, 14, 0);

    if (icon_dsc) {
        lv_obj_t * icon = ui::icons::create_icon(left_box, icon_dsc, theme::text_secondary());
        (void)icon;
    }

    lv_obj_t * label = lv_label_create(left_box);
    lv_label_set_text(label, label_text);
    lv_obj_set_style_text_font(label, &lv_font_roboto_20, 0);
    lv_obj_set_style_text_color(label, theme::text_primary(), 0);

    lv_obj_t * sw = lv_switch_create(row);
    bool state = core::default_store().get_bool(key, def_val, section);
    if (state) lv_obj_add_state(sw, LV_STATE_CHECKED);

    auto * ctx = new ToggleCtx{key, section, std::move(on_change)};
    lv_obj_add_event_cb(sw, destroy_toggle_ctx, LV_EVENT_DELETE, ctx);
    lv_obj_add_event_cb(sw, [](lv_event_t * e) {
        auto * ctx = static_cast<ToggleCtx *>(lv_event_get_user_data(e));
        lv_obj_t * target = static_cast<lv_obj_t *>(lv_event_get_target(e));
        bool val = lv_obj_has_state(target, LV_STATE_CHECKED);
        core::default_store().set_bool(ctx->key, val, ctx->section);
        core::default_store().save();
        if (ctx->onChange) {
            ctx->onChange(val);
        }
    }, LV_EVENT_VALUE_CHANGED, ctx);

    if (core::navigation::focus_group()) {
        lv_group_add_obj(core::navigation::focus_group(), sw);
    }

    return row;
}

} // namespace

lv_obj_t * create_settings_screen() {
    lv_obj_t * scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, theme::bg(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // 1. Persistent 5-Icon Navigation Rail on the Left (Settings is active)
    create_nav_rail(scr, NavDestination::Settings);

    // 2. Main Content Area to the right of the rail
    lv_obj_t * content = lv_obj_create(scr);
    lv_obj_remove_style_all(content);
    lv_obj_set_pos(content, theme::kRailWidth + 16, 8);
    lv_obj_set_size(content, 800 - (theme::kRailWidth + 32), 464);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content, 12, 0);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    // 3. Top Title Header
    lv_obj_t * header = lv_obj_create(content);
    lv_obj_remove_style_all(header);
    lv_obj_set_width(header, LV_PCT(100));
    lv_obj_set_height(header, 34);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * title_lbl = lv_label_create(header);
    lv_label_set_text(title_lbl, "Settings");
    lv_obj_set_style_text_font(title_lbl, &lv_font_roboto_28, 0);
    lv_obj_set_style_text_color(title_lbl, theme::text_primary(), 0);

    // 4. Unified Long Scrolling Settings Card
    lv_obj_t * card = lv_obj_create(content);
    theme::style_card(card);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_ver(card, 16, 0);
    lv_obj_set_style_pad_hor(card, 20, 0);
    lv_obj_set_style_pad_row(card, 8, 0);
    lv_obj_set_style_clip_corner(card, false, 0);
    lv_obj_add_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(card, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_AUTO);

    // --- Section 1: Display ---
    create_section_header(card, "DISPLAY");
    create_stepper_row(card, &ui::icons::icon_brightness, "Brightness", 0, 255, 5,
                       "Brightness", "General", VdeField::Brightness);
    create_stepper_row(card, &ui::icons::icon_contrast, "Contrast", 0, 255, 5,
                       "Contrast", "General", VdeField::Contrast);
    create_stepper_row(card, &ui::icons::icon_saturation, "Saturation", 0, 255, 5,
                       "Saturation", "General", VdeField::Saturation);
    create_stepper_row(card, &ui::icons::icon_saturation, "Hue", 0, 255, 5,
                       "Hue", "General", VdeField::Hue);

    // --- Section 2: Audio ---
    create_section_header(card, "AUDIO");
    create_stepper_row(card, &ui::icons::icon_volume, "Media Volume", 0, 100, 5,
                       "MediaVolume", "Audio", VdeField::None,
                       [](int v) { hal::set_stream_volume(hal::AudioStream::Media, v); });
    create_stepper_row(card, &ui::icons::icon_bell, "Guidance Volume", 0, 100, 5,
                       "GuidanceVolume", "Audio", VdeField::None,
                       [](int v) { hal::set_stream_volume(hal::AudioStream::Guidance, v); });
    create_stepper_row(card, &ui::icons::icon_volume, "System Volume", 0, 100, 5,
                       "SystemVolume", "Audio", VdeField::None,
                       [](int v) { hal::set_stream_volume(hal::AudioStream::System, v); });

    // --- Section 3: Vehicle & Camera ---
    create_section_header(card, "VEHICLE & CAMERA");
    create_toggle_row(card, &ui::icons::icon_nav_camera, "OEM Factory Camera",
                      "OriginalCarCamera", "General", false, [](bool oem) {
                          std::printf("%s [HAL:REVCAM] Reversing camera mode set to %s\n",
                                      core::log_timestamp().c_str(), oem ? "OEM Factory Camera" : "Aftermarket Camera");
                      });

    // --- Section 4: System ---
    create_section_header(card, "SYSTEM");
    create_toggle_row(card, &ui::icons::icon_smartphone, "Auto-Start CarLink",
                      "AutoStartCarLink", "General", true);

    return scr;
}

} // namespace staging_ui
