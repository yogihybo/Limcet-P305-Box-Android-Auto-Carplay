#include "ui/settings_screen.h"

#include <array>
#include <cstdio>
#include <string>

#include "core/config_store.h"
#include "core/navigation.h"
#include "hal/display_ctrl.h"
#include "ui/bluetooth_screen.h"
#include "ui/status_bar.h"
#include "ui/theme.h"

namespace ui {

namespace {

// Opened once, process-lifetime -- matches core::default_store()'s own
// lazy-singleton pattern and this app's general "no shutdown path"
// design (see core/reverse_gear_watcher.cpp's destructor comment).
hal::DisplayCtrlHandle & display_handle() {
    static hal::DisplayCtrlHandle handle;
    static bool tried = false;
    if (!tried) {
        hal::init_display_ctrl(handle);  // non-fatal if /dev/ark_display is absent
        tried = true;
    }
    return handle;
}

// ---- generic row scaffolding ----------------------------------------

lv_obj_t * add_row(lv_obj_t * parent, const char * label_text) {
    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                           LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(row, 8, 0);
    lv_obj_set_style_pad_column(row, 12, 0);

    lv_obj_t * label = lv_label_create(row);
    lv_label_set_text(label, label_text);
    lv_obj_set_flex_grow(label, 1);
    return row;
}

// A row that's purely informational -- shows the confirmed-live (or
// confirmed-dead) value with a short note, no control to change it.
// Used for hardware-profile fields this device shouldn't have edited
// live (CanType) or fields confirmed to have no effect at runtime
// (MirroringLinkType, ResolutionType) -- see
// docs/SETTINGS_REFERENCE.md/project_msnproductinfo_config_exploration.
void add_readonly_row(lv_obj_t * parent, const char * label_text, const std::string & value,
                       const char * note) {
    lv_obj_t * row = add_row(parent, label_text);
    lv_obj_t * value_label = lv_label_create(row);
    std::string text = value;
    if (note && note[0] != '\0') {
        text += "  (";
        text += note;
        text += ")";
    }
    lv_label_set_text(value_label, text.c_str());
    theme::style_secondary_text(value_label);
}

// ---- value-changed contexts, shared by steppers/switches/dropdowns ---

enum class VdeField { None, Hue, Saturation, Brightness, Contrast };

struct FieldCtx {
    std::string section;
    std::string key;
    VdeField vde_field = VdeField::None;
};

void destroy_ctx_cb(lv_event_t * e) {
    delete static_cast<FieldCtx *>(lv_event_get_user_data(e));
}

void apply_vde(const FieldCtx & ctx, int value) {
    if (ctx.vde_field == VdeField::None) return;
    hal::DisplayCtrlHandle & h = display_handle();
    hal::VdeConfig cfg;
    hal::get_vde_config(h, hal::DisplayLayer::Osd1, cfg);  // best-effort refresh, ok if it fails
    switch (ctx.vde_field) {
        case VdeField::Hue: cfg.hue = static_cast<unsigned int>(value); break;
        case VdeField::Saturation: cfg.saturation = static_cast<unsigned int>(value); break;
        case VdeField::Brightness: cfg.brightness = static_cast<unsigned int>(value); break;
        case VdeField::Contrast: cfg.contrast = static_cast<unsigned int>(value); break;
        default: break;
    }
    hal::set_vde_config(h, hal::DisplayLayer::Osd1, cfg);
}

// Stepper controls replace what used to be lv_slider_create() drag-
// sliders here -- see ui/theme.h's top comment: dragging a thin slider
// precisely is exactly the kind of sustained-attention, fine-motor-
// control interaction Android Auto's own design guidelines steer away
// from. A big flat "-"/"+" (theme::style_step_button(), each
// theme::kMinTouchTarget square) is a single glance and a single tap,
// and every tap is a discrete, immediately-persisted change -- no
// separate "drag ended" event to distinguish, unlike the old slider
// code's value-changed-vs-released split.
struct StepperCtx {
    std::string section;
    std::string key;
    VdeField vde_field;
    int min;
    int max;
    int step;
    int value;
    lv_obj_t * value_label;
};

struct StepperBtnCtx {
    StepperCtx * shared;
    int dir;  // -1 or +1
};

void destroy_stepper_ctx_cb(lv_event_t * e) {
    delete static_cast<StepperCtx *>(lv_event_get_user_data(e));
}

void destroy_stepper_btn_ctx_cb(lv_event_t * e) {
    delete static_cast<StepperBtnCtx *>(lv_event_get_user_data(e));
}

void stepper_btn_clicked_cb(lv_event_t * e) {
    auto * btn_ctx = static_cast<StepperBtnCtx *>(lv_event_get_user_data(e));
    StepperCtx * ctx = btn_ctx->shared;

    int new_value = ctx->value + btn_ctx->dir * ctx->step;
    if (new_value < ctx->min) new_value = ctx->min;
    if (new_value > ctx->max) new_value = ctx->max;
    if (new_value == ctx->value) return;
    ctx->value = new_value;

    lv_label_set_text_fmt(ctx->value_label, "%d", ctx->value);
    core::default_store().set_int(ctx->key, ctx->value, ctx->section);
    apply_vde(FieldCtx{ctx->section, ctx->key, ctx->vde_field}, ctx->value);
    core::default_store().save();  // discrete tap -- safe to persist immediately, same as switch_changed_cb
}

lv_obj_t * add_stepper_row(lv_obj_t * parent, const char * label_text, int min, int max, int step,
                            const std::string & key, const std::string & section,
                            VdeField vde_field = VdeField::None) {
    lv_obj_t * row = add_row(parent, label_text);

    int initial = core::default_store().get_int(key, (min + max) / 2, section);
    if (initial < min) initial = min;
    if (initial > max) initial = max;
    initial = min + ((initial - min) / step) * step;  // snap to the step grid

    auto * ctx = new StepperCtx{section, key, vde_field, min, max, step, initial, nullptr};
    lv_obj_add_event_cb(row, destroy_stepper_ctx_cb, LV_EVENT_DELETE, ctx);

    lv_obj_t * controls = lv_obj_create(row);
    lv_obj_remove_style_all(controls);
    lv_obj_set_size(controls, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(controls, 10, 0);

    lv_obj_t * minus_btn = lv_button_create(controls);
    lv_obj_remove_style_all(minus_btn);
    theme::style_step_button(minus_btn);
    lv_obj_t * minus_label = lv_label_create(minus_btn);
    lv_label_set_text(minus_label, LV_SYMBOL_MINUS);
    lv_obj_center(minus_label);
    auto * minus_ctx = new StepperBtnCtx{ctx, -1};
    lv_obj_add_event_cb(minus_btn, stepper_btn_clicked_cb, LV_EVENT_CLICKED, minus_ctx);
    lv_obj_add_event_cb(minus_btn, destroy_stepper_btn_ctx_cb, LV_EVENT_DELETE, minus_ctx);
    lv_group_add_obj(core::navigation::focus_group(), minus_btn);

    lv_obj_t * value_label = lv_label_create(controls);
    lv_obj_set_style_text_font(value_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(value_label, theme::text_primary(), 0);
    lv_obj_set_width(value_label, 60);
    lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text_fmt(value_label, "%d", initial);
    ctx->value_label = value_label;

    lv_obj_t * plus_btn = lv_button_create(controls);
    lv_obj_remove_style_all(plus_btn);
    theme::style_step_button(plus_btn);
    lv_obj_t * plus_label = lv_label_create(plus_btn);
    lv_label_set_text(plus_label, LV_SYMBOL_PLUS);
    lv_obj_center(plus_label);
    auto * plus_ctx = new StepperBtnCtx{ctx, +1};
    lv_obj_add_event_cb(plus_btn, stepper_btn_clicked_cb, LV_EVENT_CLICKED, plus_ctx);
    lv_obj_add_event_cb(plus_btn, destroy_stepper_btn_ctx_cb, LV_EVENT_DELETE, plus_ctx);
    lv_group_add_obj(core::navigation::focus_group(), plus_btn);

    if (vde_field != VdeField::None) {
        apply_vde(FieldCtx{section, key, vde_field}, initial);  // push the seeded/live value to hardware once at build time
    }
    return row;
}

void switch_changed_cb(lv_event_t * e) {
    auto * ctx = static_cast<FieldCtx *>(lv_event_get_user_data(e));
    lv_obj_t * sw = static_cast<lv_obj_t *>(lv_event_get_target(e));
    bool checked = lv_obj_has_state(sw, LV_STATE_CHECKED);
    core::default_store().set_bool(ctx->key, checked, ctx->section);
    core::default_store().save();  // discrete toggle -- safe to persist immediately
}

void add_switch_row(lv_obj_t * parent, const char * label_text, const std::string & key,
                     const std::string & section, bool default_value) {
    lv_obj_t * row = add_row(parent, label_text);
    lv_obj_t * sw = lv_switch_create(row);
    if (core::default_store().get_bool(key, default_value, section)) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    auto * ctx = new FieldCtx{section, key};
    lv_obj_add_event_cb(sw, switch_changed_cb, LV_EVENT_VALUE_CHANGED, ctx);
    lv_obj_add_event_cb(sw, destroy_ctx_cb, LV_EVENT_DELETE, ctx);
    lv_group_add_obj(core::navigation::focus_group(), sw);
}

// ---- Language dropdown (Basic tier) -----------------------------------

// Full confirmed table, docs/SETTINGS_REFERENCE.md section 2.4
// (disassembly-confirmed against libMsnCommons.so's GetLanguageValueList/
// GetLanguageNameList). 4096 deliberately excluded -- confirmed to
// decode to an anomalous/reserved slot, not a real selectable language.
struct LanguageEntry {
    int value;
    const char * name;
};
constexpr std::array<LanguageEntry, 15> kLanguages{{
    {4097, "English"},
    {4098, "Chinese Simplified"},
    {4099, "Chinese Traditional"},
    {4100, "Portugues brasileiro"},
    {4101, "Korean"},
    {4102, "Espanol"},
    {4103, "Dansk"},
    {4104, "Portugues"},
    {4105, "Italiano"},
    {4106, "Hebrew"},
    {4107, "Russian"},
    {4108, "Francais"},
    {4109, "Turkce"},
    {4110, "Deutsch"},
    {4111, "Dutch"},
}};

void language_changed_cb(lv_event_t * e) {
    lv_obj_t * dd = static_cast<lv_obj_t *>(lv_event_get_target(e));
    uint32_t idx = lv_dropdown_get_selected(dd);
    if (idx >= kLanguages.size()) return;
    core::default_store().set_int("Language", kLanguages[idx].value, "General");
    core::default_store().save();
}

void add_language_row(lv_obj_t * parent) {
    lv_obj_t * row = add_row(parent, "Language");

    std::string options;
    for (size_t i = 0; i < kLanguages.size(); ++i) {
        if (i) options += "\n";
        options += kLanguages[i].name;
    }

    lv_obj_t * dd = lv_dropdown_create(row);
    lv_obj_set_width(dd, 180);
    lv_dropdown_set_options(dd, options.c_str());

    int current = core::default_store().get_int("Language", kLanguages[0].value, "General");
    uint32_t selected_idx = 0;
    for (size_t i = 0; i < kLanguages.size(); ++i) {
        if (kLanguages[i].value == current) {
            selected_idx = static_cast<uint32_t>(i);
            break;
        }
    }
    lv_dropdown_set_selected(dd, selected_idx);
    lv_obj_add_event_cb(dd, language_changed_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_group_add_obj(core::navigation::focus_group(), dd);
}

// ---- navigation buttons ------------------------------------------------

void back_btn_cb(lv_event_t *) {
    core::navigation::pop();
}

void bluetooth_btn_cb(lv_event_t *) {
    core::navigation::push(ui::create_bluetooth_screen);
}

// ---- tab builders --------------------------------------------------

void build_basic_tab(lv_obj_t * tab) {
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(tab, 4, 0);

    lv_obj_t * display_header = lv_label_create(tab);
    lv_label_set_text(display_header, "Display");
    theme::style_section_label(display_header);

    // Range 0-255 mirrors the real captured Setting.config values
    // (Brightness=128, Contrast=128, Saturation=64 -- see
    // core/config_store.h's top comment) -- the VDE ioctl's own valid
    // range isn't independently confirmed (struct fields are plain
    // `unsigned int`, no documented bound found), so this assumes the
    // same 0-255 scale the persisted config already uses. Flagged as
    // an assumption, not hardware-verified.
    add_stepper_row(tab, "Brightness", 0, 255, 17, "Brightness", "General", VdeField::Brightness);
    add_stepper_row(tab, "Contrast", 0, 255, 17, "Contrast", "General", VdeField::Contrast);
    add_stepper_row(tab, "Saturation", 0, 255, 17, "Saturation", "General", VdeField::Saturation);

    lv_obj_t * audio_header = lv_label_create(tab);
    lv_label_set_text(audio_header, "Audio");
    theme::style_section_label(audio_header);

    add_stepper_row(tab, "Volume", 0, 40, 4, "Volume", "General");

    lv_obj_t * general_header = lv_label_create(tab);
    lv_label_set_text(general_header, "General");
    theme::style_section_label(general_header);

    add_language_row(tab);

    lv_obj_t * bt_row = add_row(tab, "Bluetooth");
    lv_obj_t * bt_btn = lv_button_create(bt_row);
    theme::style_primary_button(bt_btn);
    lv_obj_add_event_cb(bt_btn, bluetooth_btn_cb, LV_EVENT_CLICKED, nullptr);
    lv_group_add_obj(core::navigation::focus_group(), bt_btn);
    lv_obj_t * bt_btn_label = lv_label_create(bt_btn);
    lv_label_set_text(bt_btn_label, "Pairing / Devices >");

    // WiFi -- this device doesn't join networks as a client; it hosts
    // its own dynamic hostapd AP for wireless Android Auto/CarPlay
    // (see docs/ARCHITECTURE.md "Wireless AA discovery" +
    // project_wireless_carplay_aa_channel_plan memory). There is no
    // "join this network" flow to build -- the AP is spun up
    // automatically per-connection, credentials handed to the phone
    // over Bluetooth. This row is informational, not a configurable
    // settings control.
    lv_obj_t * wifi_row = add_row(tab, "WiFi");
    lv_obj_t * wifi_value = lv_label_create(wifi_row);
    int wlan_type = core::default_store().get_int("WLANType", 3, "General");
    char wifi_buf[96];
    std::snprintf(wifi_buf, sizeof(wifi_buf),
                  "Managed automatically (WLANType=%d) -- AP for wireless AA/CarPlay",
                  wlan_type);
    lv_label_set_text(wifi_value, wifi_buf);
    theme::style_secondary_text(wifi_value);
}

void build_advanced_tab(lv_obj_t * tab) {
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(tab, 4, 0);

    lv_obj_t * warn = lv_label_create(tab);
    lv_label_set_text(warn,
                       "Advanced / factory fields. Read-only entries reflect confirmed "
                       "hardware-profile values this project's disassembly work has traced --");
    lv_label_set_long_mode(warn, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(warn, LV_PCT(100));
    theme::style_secondary_text(warn);

    core::ConfigStore & store = core::default_store();

    // Hardware profile (MsnProductInfo.ini) -- read-only. Changing
    // these live has no defined behavior on a provisioned device (they
    // gate boot-time hardware detection, not something this app
    // should let a user edit post-boot) -- see
    // docs/SETTINGS_REFERENCE.md section 1.
    add_readonly_row(tab, "MCU Type", std::to_string(store.get_int("McuType", 6, "General")),
                      "BoxP300 adapter -- see MCU_ADAPTERS.md");
    add_readonly_row(tab, "CAN Type", std::to_string(store.get_int("CanType", 0, "General")),
                      "locked -- CanType=1 breaks touch/knob input, do not change");
    add_readonly_row(tab, "Screen Type", std::to_string(store.get_int("ScreenType", 1, "General")),
                      "may be overwritten live by the connected MCU board");
    add_readonly_row(tab, "Resolution Type",
                      std::to_string(store.get_int("ResolutionType", 1, "General")),
                      "confirmed dead: no code path reads this key");
    add_readonly_row(tab, "BlueTooth Type",
                      std::to_string(store.get_int("BlueToothType", 6, "General")),
                      "module/stack selector, not app-layer BT config");
    add_readonly_row(tab, "Radio Type", std::to_string(store.get_int("RadioType", 0, "General")),
                      "0 = no tuner fitted");
    add_readonly_row(tab, "Mirroring Link Type",
                      std::to_string(store.get_int("MirroringLinkType", 1, "General")),
                      "confirmed no effect on this device (ProductId gate never matches)");

    lv_obj_t * behaviour_header = lv_label_create(tab);
    lv_label_set_text(behaviour_header, "Behaviour");
    theme::style_section_label(behaviour_header);

    // FactoryConfig.ini fields confirmed live via disassembly (section
    // 2 of SETTINGS_REFERENCE.md) -- safe to expose as real editable
    // controls.
    add_stepper_row(tab, "Reversing volume cut (%)", 0, 100, 10, "ReversingVolumeCut", "General");
    add_stepper_row(tab, "AEC delay (ms)", 0, 300, 25, "AECDelay", "General");
    add_switch_row(tab, "Right-hand drive layout", "RightHandCarDriver", "General", false);
    add_switch_row(tab, "Disable window transitions", "DisableWindowEffect", "General", false);
    add_switch_row(tab, "Touch idle auto-calibrate", "TouchCalibrateAction", "General", false);
    add_switch_row(tab, "Auto-start phone projection", "AutoStartCarLink", "General", true);
}

}  // namespace

lv_obj_t * create_settings_screen() {
    lv_obj_t * scr = nullptr;
    theme::create_screen_with_header(&scr, "Settings", back_btn_cb);

    lv_obj_t * tabview = lv_tabview_create(scr);
    // The status bar (ui/status_bar.h) now sits at the literal bottom
    // of the screen, so the tabview needs to stop short of it instead
    // of running to the screen edge.
    lv_obj_set_size(tabview, LV_PCT(100), LV_PCT(74));
    lv_obj_align(tabview, LV_ALIGN_BOTTOM_MID, 0, -status_bar::kHeight);
    theme::style_tabview(tabview);

    lv_obj_t * basic_tab = lv_tabview_add_tab(tabview, "Basic");
    lv_obj_t * advanced_tab = lv_tabview_add_tab(tabview, "Advanced");
    build_basic_tab(basic_tab);
    build_advanced_tab(advanced_tab);

    return scr;
}

}  // namespace ui
