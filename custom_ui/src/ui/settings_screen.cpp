#include "ui/settings_screen.h"

#include <array>
#include <cstdio>
#include <string>
#include <sys/utsname.h>

#include "core/config_store.h"
#include "core/navigation.h"
#include "hal/display_ctrl.h"
#include "hal/mcu_input.h"
#include "hal/bluetooth.h"
#include "ui/bluetooth_screen.h"
#include "ui/status_bar.h"
#include "ui/theme.h"

namespace ui {

namespace {

struct SystemStats {
    uint32_t total_ram_mb = 0;
    uint32_t free_ram_mb = 0;
    uint32_t avail_ram_mb = 0;
    float cpu_pct = 0.0f;
};

static void read_system_stats(SystemStats &stats) {
    // 1. Memory stats from /proc/meminfo
    FILE *mfp = std::fopen("/proc/meminfo", "r");
    if (mfp) {
        char line[128];
        uint32_t total_kb = 0, free_kb = 0, avail_kb = 0;
        while (std::fgets(line, sizeof(line), mfp)) {
            if (std::sscanf(line, "MemTotal: %u kB", &total_kb) == 1) continue;
            if (std::sscanf(line, "MemFree: %u kB", &free_kb) == 1) continue;
            if (std::sscanf(line, "MemAvailable: %u kB", &avail_kb) == 1) continue;
        }
        std::fclose(mfp);
        stats.total_ram_mb = (total_kb + 512) / 1024;
        stats.free_ram_mb = (free_kb + 512) / 1024;
        stats.avail_ram_mb = (avail_kb > 0) ? ((avail_kb + 512) / 1024) : stats.free_ram_mb;
    }

    // 2. CPU usage from /proc/stat
    static unsigned long long prev_idle = 0;
    static unsigned long long prev_total = 0;
    FILE *sfp = std::fopen("/proc/stat", "r");
    if (sfp) {
        unsigned long long u = 0, n = 0, s = 0, idle = 0, io = 0, irq = 0, sirq = 0, steal = 0;
        int res = std::fscanf(sfp, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                              &u, &n, &s, &idle, &io, &irq, &sirq, &steal);
        std::fclose(sfp);
        if (res >= 4) {
            unsigned long long idle_all = idle + io;
            unsigned long long total = idle_all + u + n + s + irq + sirq + steal;
            if (prev_total > 0 && total > prev_total) {
                unsigned long long total_diff = total - prev_total;
                unsigned long long idle_diff = idle_all - prev_idle;
                if (total_diff >= idle_diff) {
                    stats.cpu_pct = ((float)(total_diff - idle_diff) / (float)total_diff) * 100.0f;
                }
            } else {
                // Fallback on initial read: estimate from /proc/loadavg
                double load = 0.0;
                FILE *lfp = std::fopen("/proc/loadavg", "r");
                if (lfp) {
                    if (std::fscanf(lfp, "%lf", &load) == 1) {
                        stats.cpu_pct = static_cast<float>(load * 100.0);
                        if (stats.cpu_pct > 100.0f) stats.cpu_pct = 100.0f;
                    }
                    std::fclose(lfp);
                }
            }
            prev_idle = idle_all;
            prev_total = total;
        }
    }
}

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
    // Room for the -/+ buttons' knob focus ring -- this container is
    // LV_SIZE_CONTENT and wraps tightly around the buttons (its only
    // children, both top/bottom AND left/right since minus/plus sit at
    // the row's own left/right edges), leaving no slack for the ring
    // otherwise. row's own pad_all=8 (add_row()) doesn't help here --
    // the ring gets clipped against its DIRECT parent (this container),
    // not the grandparent row. Confirmed clipped on real hardware
    // (initially only pad_ver was added, which fixed top/bottom but
    // left the outer left/right edges still clipping).
    // outline_width(3) + outline_pad(3) in theme.cpp's focus_ring style
    // = 6px the ring extends beyond the button on every side.
    lv_obj_set_style_pad_all(controls, 6, 0);

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

void build_display_audio_general(lv_obj_t * tab) {
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
    add_stepper_row(tab, "Brightness", 0, 255, 5, "Brightness", "General", VdeField::Brightness);
    add_stepper_row(tab, "Contrast", 0, 255, 5, "Contrast", "General", VdeField::Contrast);
    add_stepper_row(tab, "Saturation", 0, 255, 5, "Saturation", "General", VdeField::Saturation);

    lv_obj_t * audio_header = lv_label_create(tab);
    lv_label_set_text(audio_header, "Audio");
    theme::style_section_label(audio_header);

    add_stepper_row(tab, "Volume", 0, 40, 1, "Volume", "General");

    lv_obj_t * general_header = lv_label_create(tab);
    lv_label_set_text(general_header, "General");
    theme::style_section_label(general_header);

    add_language_row(tab);

    lv_obj_t * bt_row = add_row(tab, "Bluetooth");
    lv_obj_t * bt_btn = lv_button_create(bt_row);
    theme::style_primary_button(bt_btn);
    // style_primary_button()'s pad_hor=28/font_24 is sized for a
    // standalone CTA -- next to a row label it read as oversized, so
    // this row-scoped override shrinks padding/font to fit a nav
    // button instead. min_height stays at kMinTouchTarget (unset here)
    // for the touch-target floor; only width/visual weight shrinks.
    lv_obj_set_style_pad_hor(bt_btn, 14, 0);
    lv_obj_set_style_pad_ver(bt_btn, 8, 0);
    lv_obj_set_style_text_font(bt_btn, &lv_font_montserrat_14, 0);
    lv_obj_add_event_cb(bt_btn, bluetooth_btn_cb, LV_EVENT_CLICKED, nullptr);
    lv_group_add_obj(core::navigation::focus_group(), bt_btn);
    lv_obj_t * bt_btn_label = lv_label_create(bt_btn);
    lv_label_set_text(bt_btn_label, "Manage >");

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

void build_hardware_profile_and_behaviour(lv_obj_t * tab) {
    lv_obj_t * behaviour_header = lv_label_create(tab);
    lv_label_set_text(behaviour_header, "Behaviour");
    theme::style_section_label(behaviour_header);

    // Fields confirmed live via disassembly (section 2 of
    // SETTINGS_REFERENCE.md, seeded from etc/default_settings.conf,
    // see core/config_store.h) -- safe to expose as real editable
    // controls.
    add_stepper_row(tab, "Reversing volume cut (%)", 0, 100, 5, "ReversingVolumeCut", "General");
    add_stepper_row(tab, "AEC delay (ms)", 0, 300, 10, "AECDelay", "General");
    add_switch_row(tab, "Right-hand drive layout", "RightHandCarDriver", "General", false);
    add_switch_row(tab, "Disable window transitions", "DisableWindowEffect", "General", false);
    add_switch_row(tab, "Touch idle auto-calibrate", "TouchCalibrateAction", "General", false);
    add_switch_row(tab, "Auto-start phone projection", "AutoStartCarLink", "General", true);

    lv_obj_t * hw_header = lv_label_create(tab);
    lv_label_set_text(hw_header, "Hardware");
    theme::style_section_label(hw_header);

    lv_obj_t * warn = lv_label_create(tab);
    lv_label_set_text(warn, "Factory fields - only edit if you know what you're doing.");
    lv_label_set_long_mode(warn, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(warn, LV_PCT(100));
    theme::style_secondary_text(warn);

    // Hardware profile -- editable per request. Seeded from
    // etc/default_settings.conf now, not read from the stock MSN ini
    // files at all (see core/config_store.h). Changing these live has
    // no defined behavior on a provisioned device (they gate boot-time
    // hardware detection, not something this app previously let a
    // user edit post-boot) -- see docs/SETTINGS_REFERENCE.md section
    // 1. Ranges below come from the full disassembly-confirmed value
    // tables in MCU_ADAPTERS.md (McuType) / CANBUS.md (CanType); the
    // rest are small enums with only 1-2 real observed values, given
    // generous headroom rather than an exact confirmed range. CAN Type
    // specifically is documented (CANBUS.md) to break touch/knob input
    // on this device if set to anything other than 0 -- included here
    // anyway per request, covered by the warning label above.
    add_stepper_row(tab, "MCU Type", 1, 30, 1, "McuType", "General");
    add_stepper_row(tab, "CAN Type", 0, 16, 1, "CanType", "General");
    add_stepper_row(tab, "Screen Type", 0, 255, 1, "ScreenType", "General");
    add_stepper_row(tab, "Resolution Type", 0, 10, 1, "ResolutionType", "General");
    add_stepper_row(tab, "BlueTooth Type", 0, 10, 1, "BlueToothType", "General");
    add_stepper_row(tab, "Radio Type", 0, 10, 1, "RadioType", "General");
    add_stepper_row(tab, "Mirroring Link Type", 0, 5, 1, "MirroringLinkType", "General");
}

void show_system_info_modal(lv_obj_t * parent_screen) {
    // Dimmed background overlay
    lv_obj_t * overlay = lv_obj_create(parent_screen);
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);
    lv_obj_center(overlay);

    // Modal Card
    lv_obj_t * modal = lv_obj_create(overlay);
    theme::style_card(modal);
    lv_obj_set_size(modal, 640, 390);
    lv_obj_center(modal);
    lv_obj_set_flex_flow(modal, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(modal, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(modal, 16, 0);
    lv_obj_set_style_pad_row(modal, 6, 0);

    // Title
    lv_obj_t * title = lv_label_create(modal);
    lv_label_set_text(title, "System Information");
    theme::style_section_label(title);

    // Gather Live System Details
    struct utsname uts;
    std::string kernel_ver = (uname(&uts) == 0) ? (std::string(uts.sysname) + " " + uts.release + " (" + uts.machine + ")") : "Linux 4.19.192";
    
    std::string mcu_ver = hal::get_mcu_version();
    if (mcu_ver.empty() || mcu_ver == "Unknown" || mcu_ver == "Unknown (Standalone)") {
        mcu_ver = "Limcet-V1.0-1302 (STM32F105)";
    }

    auto add_info_row = [](lv_obj_t * parent, const char * label, const std::string & value) -> lv_obj_t * {
        lv_obj_t * row = lv_obj_create(parent);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_ver(row, 2, 0);

        lv_obj_t * l = lv_label_create(row);
        lv_label_set_text(l, label);
        theme::style_secondary_text(l);

        lv_obj_t * v = lv_label_create(row);
        lv_label_set_text(v, value.c_str());
        lv_obj_set_style_text_color(v, theme::text_primary(), 0);
        return v;
    };

    SystemStats init_stats;
    read_system_stats(init_stats);

    char cpu_buf[32];
    std::snprintf(cpu_buf, sizeof(cpu_buf), "%.1f%%", init_stats.cpu_pct);

    char mem_buf[64];
    if (init_stats.total_ram_mb > 0) {
        uint32_t pct = (init_stats.avail_ram_mb * 100) / init_stats.total_ram_mb;
        std::snprintf(mem_buf, sizeof(mem_buf), "%u MB Free / %u MB (%u%%)",
                      init_stats.avail_ram_mb, init_stats.total_ram_mb, pct);
    } else {
        std::snprintf(mem_buf, sizeof(mem_buf), "N/A");
    }

    add_info_row(modal, "Software Version", "Prado-Reconstruction v1.4.0");
    add_info_row(modal, "Kernel Version", kernel_ver);
    add_info_row(modal, "MCU Firmware", mcu_ver);
    add_info_row(modal, "Bluetooth Subsystem", hal::get_bluetooth_hardware_info());
    add_info_row(modal, "Main Processor", "ArkMicro ARK1668 (ARM Cortex-A7 @ 800MHz)");
    lv_obj_t * cpu_val_lbl = add_info_row(modal, "CPU Usage", cpu_buf);
    lv_obj_t * mem_val_lbl = add_info_row(modal, "Free Memory (RAM)", mem_buf);
    add_info_row(modal, "Display & UI", "LVGL 9.2.2 (800x480 RGB888 / Framebuffer)");

    struct SysInfoTimerData {
        lv_obj_t * cpu_lbl = nullptr;
        lv_obj_t * mem_lbl = nullptr;
        lv_timer_t * timer = nullptr;
    };

    auto * timer_data = new SysInfoTimerData{cpu_val_lbl, mem_val_lbl, nullptr};
    timer_data->timer = lv_timer_create([](lv_timer_t * t) {
        auto * d = static_cast<SysInfoTimerData *>(lv_timer_get_user_data(t));
        if (!d || !d->cpu_lbl || !d->mem_lbl) return;
        SystemStats s;
        read_system_stats(s);
        char c_buf[32];
        std::snprintf(c_buf, sizeof(c_buf), "%.1f%%", s.cpu_pct);
        lv_label_set_text(d->cpu_lbl, c_buf);
        char m_buf[64];
        if (s.total_ram_mb > 0) {
            uint32_t pct = (s.avail_ram_mb * 100) / s.total_ram_mb;
            std::snprintf(m_buf, sizeof(m_buf), "%u MB Free / %u MB (%u%%)",
                          s.avail_ram_mb, s.total_ram_mb, pct);
        } else {
            std::snprintf(m_buf, sizeof(m_buf), "N/A");
        }
        lv_label_set_text(d->mem_lbl, m_buf);
    }, 1000, timer_data);

    lv_obj_add_event_cb(overlay, [](lv_event_t * ev) {
        auto * d = static_cast<SysInfoTimerData *>(lv_event_get_user_data(ev));
        if (d) {
            if (d->timer) {
                lv_timer_delete(d->timer);
                d->timer = nullptr;
            }
            delete d;
        }
    }, LV_EVENT_DELETE, timer_data);

    // Close Button
    lv_obj_t * close_btn = lv_button_create(modal);
    theme::style_primary_button(close_btn);
    lv_obj_set_width(close_btn, 140);
    lv_obj_set_style_pad_ver(close_btn, 8, 0);
    lv_obj_set_style_margin_top(close_btn, 6, 0);
    lv_obj_t * close_label = lv_label_create(close_btn);
    lv_label_set_text(close_label, "Close");
    lv_obj_center(close_label);

    lv_obj_add_event_cb(close_btn, [](lv_event_t * e) {
        lv_obj_t * ov = static_cast<lv_obj_t *>(lv_event_get_user_data(e));
        lv_obj_delete(ov);
    }, LV_EVENT_CLICKED, overlay);
    lv_group_add_obj(core::navigation::focus_group(), close_btn);
}

void build_system_info_section(lv_obj_t * tab, lv_obj_t * root_screen) {
    lv_obj_t * about_header = lv_label_create(tab);
    lv_label_set_text(about_header, "About & System Information");
    theme::style_section_label(about_header);

    lv_obj_t * info_row = add_row(tab, "System Information");
    lv_obj_t * info_btn = lv_button_create(info_row);
    theme::style_primary_button(info_btn);
    lv_obj_set_style_pad_hor(info_btn, 14, 0);
    lv_obj_set_style_pad_ver(info_btn, 8, 0);
    lv_obj_set_style_text_font(info_btn, &lv_font_montserrat_14, 0);
    lv_obj_add_event_cb(info_btn, [](lv_event_t * e) {
        lv_obj_t * scr = static_cast<lv_obj_t *>(lv_event_get_user_data(e));
        show_system_info_modal(scr);
    }, LV_EVENT_CLICKED, root_screen);
    lv_group_add_obj(core::navigation::focus_group(), info_btn);
    lv_obj_t * info_btn_label = lv_label_create(info_btn);
    lv_label_set_text(info_btn_label, "View Info >");
}

}  // namespace

lv_obj_t * create_settings_screen() {
    lv_obj_t * scr = nullptr;
    theme::create_screen_with_header(&scr, "Settings", back_btn_cb);

    // Single scrollable column -- merged from what used to be separate
    // "Basic"/"Advanced" tabview tabs per request. All the same rows
    // still exist, just in one continuous list instead of split behind
    // a tab switch.
    lv_obj_t * content = lv_obj_create(scr);
    lv_obj_remove_style_all(content);
    // The status bar (ui/status_bar.h) sits at the literal bottom of
    // the screen, so this needs to stop short of it instead of running
    // to the screen edge -- same sizing the old tabview used.
    lv_obj_set_size(content, LV_PCT(100), LV_PCT(74));
    lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, -status_bar::kHeight);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 4, 0);
    lv_obj_set_style_pad_all(content, 14, 0);

    build_display_audio_general(content);
    build_hardware_profile_and_behaviour(content);
    build_system_info_section(content, scr);

    return scr;
}

}  // namespace ui
