#include "ui/staging/settings_screen.h"
#include "ui/staging/theme.h"
#include "ui/staging/fonts.h"
#include "ui/staging/nav_rail.h"
#include "ui/staging/icons.h"
#include "core/config_store.h"
#include "core/navigation.h"
#include "hal/audio.h"
#include "hal/display_ctrl.h"
#include "hal/mcu_input.h"
#include "hal/ssh_access.h"
#include "hal/bluetooth.h"
#include "core/log_timing.h"
#include "core/sized_thread.h"
#include <functional>
#include <memory>
#include <sys/utsname.h>
#include <cstdio>

namespace staging_ui {

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

hal::DisplayCtrlHandle & display_handle() {
    static hal::DisplayCtrlHandle handle;
    static bool tried = false;
    if (!tried) {
        hal::init_display_ctrl(handle);
        tried = true;
    }
    return handle;
}

struct StepperCtx {
    std::string section;
    std::string key;
    int min;
    int max;
    int step;
    int value;
    lv_obj_t * value_label;
    lv_obj_t * level_bar;
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
    if (ctx->extra_apply) {
        // 2026-09-04: real hardware bug -- extra_apply() (e.g. the
        // Media/Guidance/System volume steppers' hal::set_stream_volume(),
        // which shells out to `amixer` via std::system() with NO timeout
        // at all) used to run inline here, on the LVGL thread. Volume
        // steppers get tapped repeatedly in normal use, so this was an
        // easy real trigger for the exact class of freeze main.cpp's own
        // apply_reversing_volume_cut() fix already addressed for the
        // reverse-gear path -- see that fix's own comment. Moved off the
        // LVGL thread the same way: fire-and-forget on a detached thread,
        // since nothing here needs to wait for amixer to finish.
        core::SizedThread(core::kDefaultThreadStackSize, ctx->extra_apply, ctx->value).detach();
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
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * lbl = lv_label_create(row);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_font(lbl, &lv_font_roboto_14, 0);
    lv_obj_set_style_text_color(lbl, theme::accent_primary(), 0);
    return row;
}

lv_obj_t * create_stepper_row(lv_obj_t * parent, const lv_image_dsc_t * icon_dsc, const char * label_text,
                             int min, int max, int step, const std::string & key,
                             const std::string & section,
                             std::function<void(int)> extra_apply = nullptr) {
    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 64);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(row, 16, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    // Left Icon + Label
    lv_obj_t * left_box = lv_obj_create(row);
    lv_obj_remove_style_all(left_box);
    lv_obj_set_size(left_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(left_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(left_box, 14, 0);
    lv_obj_clear_flag(left_box, LV_OBJ_FLAG_SCROLLABLE);

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

    auto * ctx = new StepperCtx{section, key, min, max, step, initial, nullptr, nullptr, extra_apply};
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
    lv_obj_clear_flag(right_box, LV_OBJ_FLAG_SCROLLABLE);

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
    lv_obj_remove_flag(minus_btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    auto * minus_ctx = new StepperBtnCtx{ctx, -1};
    lv_obj_add_event_cb(minus_btn, destroy_btn_ctx, LV_EVENT_DELETE, minus_ctx);
    lv_obj_add_event_cb(minus_btn, stepper_click_cb, LV_EVENT_CLICKED, minus_ctx);
    lv_obj_t * minus_icon = ui::icons::create_icon(minus_btn, &ui::icons::icon_minus, theme::text_primary());
    lv_obj_set_style_image_recolor(minus_icon, theme::text_on_accent(), LV_STATE_FOCUSED);
    lv_obj_center(minus_icon);

    // Plus Button
    lv_obj_t * plus_btn = lv_button_create(right_box);
    theme::style_stepper_button(plus_btn);
    lv_obj_remove_flag(plus_btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
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

    if (extra_apply) {
        // 2026-09-05: real hardware bug found via code review -- this
        // used to call extra_apply(initial) synchronously right here,
        // during screen construction. For the Media/Guidance/System
        // volume steppers, extra_apply is hal::set_stream_volume(),
        // which shells out via std::system("amixer ...") with no
        // timeout (same call already fixed for the click-handler case
        // below, in stepper_click_cb) -- three of these back to back
        // (one per volume stepper) made every visit to the Settings
        // screen visibly stutter while the LVGL thread blocked on
        // three sequential subshell forks. Async for the same reason
        // and via the same detached-thread pattern as that fix -- the
        // real intent here (making sure amixer's actual level matches
        // the persisted setting every time this screen opens, in case
        // something else changed it) is preserved, it just no longer
        // blocks rendering the screen itself.
        core::SizedThread(core::kDefaultThreadStackSize, extra_apply, initial).detach();
    }

    return row;
}

struct ToggleCtx {
    std::string key;
    std::string section;
    std::function<void(bool)> onChange;
    // 2026-09-05: real hardware bug found via code review -- the
    // "Aftermarket Reverse Camera" toggle's own on-disk key
    // ("OriginalCarCamera") and downstream consumer (main.cpp's
    // factoryCamera) both use the opposite polarity from what this
    // label promises (checked -> true is stored, but true there means
    // OEM Factory mode, not Aftermarket). Rather than touch every other
    // toggle's shared storage behavior, or rename the on-disk key
    // (losing anyone's already-saved preference under the old,
    // mislabeled toggle), this one flag inverts what gets stored/read
    // for JUST this one toggle -- every other create_toggle_row() call
    // leaves it false (the default) and is completely unaffected.
    bool invert_stored_value = false;
};

void destroy_toggle_ctx(lv_event_t * e) {
    delete static_cast<ToggleCtx *>(lv_event_get_user_data(e));
}

lv_obj_t * create_toggle_row(lv_obj_t * parent, const lv_image_dsc_t * icon_dsc, const char * label_text,
                            const std::string & key, const std::string & section, bool def_val,
                            std::function<void(bool)> on_change = nullptr,
                            bool invert_stored_value = false) {
    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 64);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(row, 16, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * left_box = lv_obj_create(row);
    lv_obj_remove_style_all(left_box);
    lv_obj_set_size(left_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(left_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(left_box, 14, 0);
    lv_obj_clear_flag(left_box, LV_OBJ_FLAG_SCROLLABLE);

    if (icon_dsc) {
        lv_obj_t * icon = ui::icons::create_icon(left_box, icon_dsc, theme::text_secondary());
        (void)icon;
    }

    lv_obj_t * label = lv_label_create(left_box);
    lv_label_set_text(label, label_text);
    lv_obj_set_style_text_font(label, &lv_font_roboto_20, 0);
    lv_obj_set_style_text_color(label, theme::text_primary(), 0);

    lv_obj_t * sw = lv_switch_create(row);
    lv_obj_remove_flag(sw, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    // def_val (like the on-disk value itself) is always expressed in
    // the KEY's own polarity, unaffected by invert_stored_value -- only
    // the switch's displayed checked state gets inverted, below.
    bool stored = core::default_store().get_bool(key, def_val, section);
    bool state = invert_stored_value ? !stored : stored;
    if (state) lv_obj_add_state(sw, LV_STATE_CHECKED);

    auto * ctx = new ToggleCtx{key, section, std::move(on_change), invert_stored_value};
    lv_obj_add_event_cb(sw, destroy_toggle_ctx, LV_EVENT_DELETE, ctx);
    lv_obj_add_event_cb(sw, [](lv_event_t * e) {
        auto * ctx = static_cast<ToggleCtx *>(lv_event_get_user_data(e));
        lv_obj_t * target = static_cast<lv_obj_t *>(lv_event_get_target(e));
        bool checked = lv_obj_has_state(target, LV_STATE_CHECKED);
        bool to_store = ctx->invert_stored_value ? !checked : checked;
        core::default_store().set_bool(ctx->key, to_store, ctx->section);
        core::default_store().save();
        if (ctx->onChange) {
            // onChange always receives the switch's own natural checked
            // state (matching what the label describes), never the
            // possibly-inverted on-disk value.
            ctx->onChange(checked);
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
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scroll_dir(card, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_AUTO);

    // --- Section 1: Display ---
    create_section_header(card, "DISPLAY");
    create_stepper_row(card, &ui::icons::icon_brightness, "Backlight", 0, 100, 5,
                       "Backlight", "General",
                       [](int v) { hal::set_backlight_brightness(v); });

    // --- Section 2: Audio ---
    create_section_header(card, "AUDIO");
    create_stepper_row(card, &ui::icons::icon_volume, "Media Volume", 0, 100, 5,
                       "MediaVolume", "Audio",
                       [](int v) { hal::set_stream_volume(hal::AudioStream::Media, v); });
    create_stepper_row(card, &ui::icons::icon_bell, "Guidance Volume", 0, 100, 5,
                       "GuidanceVolume", "Audio",
                       [](int v) { hal::set_stream_volume(hal::AudioStream::Guidance, v); });
    create_stepper_row(card, &ui::icons::icon_volume, "System Volume", 0, 100, 5,
                       "SystemVolume", "Audio",
                       [](int v) { hal::set_stream_volume(hal::AudioStream::System, v); });

    // 3-Band Parametric Equalizer & Dynamic Loudness
    create_stepper_row(card, &ui::icons::icon_volume, "Equalizer: Bass (dB)", -6, 6, 1,
                       "Bass", "Audio",
                       [](int) { hal::sync_audio_eq(); });
    create_stepper_row(card, &ui::icons::icon_volume, "Equalizer: Middle (dB)", -6, 6, 1,
                       "Mid", "Audio",
                       [](int) { hal::sync_audio_eq(); });
    create_stepper_row(card, &ui::icons::icon_volume, "Equalizer: Treble (dB)", -6, 6, 1,
                       "Treble", "Audio",
                       [](int) { hal::sync_audio_eq(); });
    create_toggle_row(card, &ui::icons::icon_volume, "Dynamic Loudness",
                      "Loudness", "Audio", false,
                      [](bool) { hal::sync_audio_eq(); });
    create_toggle_row(card, &ui::icons::icon_volume, "OEM Factory Microphone",
                      "OEMMicrophone", "Audio", false,
                      [](bool oem) {
                          // Vendor stock protocol (libMcuCenter.so / MCUAdapter_BoxP300):
                          // CMD 0xA0 id=0x11 (Microphone): 0=OEM Microphone, 1=AfterMarket Microphone.
                          // (id=0x09 was a historical repo error: 0x09 is actually Reversing Mode / Trajectory).
                          std::printf("%s [HAL:AUDIO] Microphone input source set to %s (CMD 0xA0 [0x11, %d])\n",
                                      core::log_timestamp().c_str(),
                                      oem ? "OEM Factory Roof Mic" : "Aftermarket 3.5mm Mic",
                                      oem ? 0 : 1);
                          hal::send_mcu_setting(0x11, oem ? 0 : 1);
                      });
    /* REMOVED (2026-09-02, real hardware CONFIRMED, not the earlier
     * candidate/unconfirmed framing): this standalone "Microphone
     * Source (OEM/AfterMarket)" toggle sent CMD 0xA0 id=0x00 -- real
     * hardware testing (methodical: toggled repeatedly, tested with
     * real reverse gear after each toggle, id=0x11 held fixed to rule
     * out interaction) confirmed this id actually controls the OEM
     * camera relay. Turned out to be a real indexing error in this
     * project's own earlier analysis, not a vendor bug: independently
     * re-derived getSetItemValueTexts(0) and found its real strings are
     * "AfterMarket Camera"/"Factory Camera"/"AfterMarket 360"/"Factory
     * 360" -- id=0x00 is consistently, correctly the camera setting by
     * both name and function (see docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md's
     * own correction section). A same-methodology follow-up test found
     * id=0x11 (the "OEM Factory Camera" toggle below) "didn't seem to
     * do anything" on its own -- id=0x00 is the confirmed-working
     * lever. Rather than leave this as a separate, confusingly-labeled
     * duplicate control, it's now folded directly into "OEM Factory
     * Camera" below -- see hal::McuInputHal::
     * sync_video_relay()'s own comment for the real send logic. */

    // --- Section 3: Vehicle & Camera ---
    create_section_header(card, "VEHICLE & CAMERA");
    /* Real OEM/Aftermarket "Camera Type" setting -- CONFIRMED 2026-08-29 by
     * direct disassembly of MCUAdapter_BoxP300::syncSettingDataToMcu(int)/
     * getSetItemValueTexts(int) (usr/lib/libMcuCenter.so), the confirmed-
     * active MCU adapter class's own real settings-sync function: CMD 0xA0
     * id=0x01, value 0=AfterMarket Camera / 1=Factory(OEM) Camera. This
     * REPLACES an earlier lead (CanBus_Raise_Toyota::enableOEMSound in
     * usr/lib/libCanBus.so) that turned out to be dead code on this
     * hardware -- confirmed via a whole-rootfs dlopen-reference sweep,
     * see MCU_FIRMWARE_VERIFIED_FINDINGS.md's "RETRACTION" section. NOT
     * the same feature as stock's separate 7-way reversing-camera video
     * FORMAT picker (FactoryWindow::on_btnCameraType_clicked() in
     * usr/lib/libSetting.so, a genuinely different real stock feature --
     * deliberately not implemented here, see "RESOLVED: the real camera
     * setting" section for that finding, kept for the record). This
     * toggle is the one that controls whether the video multiplexer
     * reverts to the stock OEM feed or stays on the aftermarket feed --
     * see hal::send_mcu_video_relay() and MCU_FIRMWARE_VERIFIED_FINDINGS.md's
     * "CONFIRMED: the real Camera Type setting" section for the full trace. */
    // 2026-09-05: real hardware bug found via code review -- this
    // toggle used to store the switch's own raw checked-state directly
    // into "OriginalCarCamera" (true=checked -> OEM Factory mode), the
    // exact inverse of what its "Aftermarket Reverse Camera" label
    // promises (checked should mean aftermarket is active). invert=true
    // flips storage/read for just this toggle -- see ToggleCtx's own
    // comment. onChange now receives `aftermarket` (the switch's
    // natural checked state, matching the label) instead of the old
    // `oem` naming, which was really just the raw on-disk value.
    // def_val=true (on-disk-key polarity, per explicit request) -- a
    // fresh/unconfigured device now defaults to Factory/OEM mode, so
    // this switch shows UNCHECKED by default (invert_stored_value maps
    // that true -> displayed false), matching main.cpp's own
    // factoryCam/factoryCamera defaults above.
    create_toggle_row(card, &ui::icons::icon_nav_camera, "Aftermarket Reverse Camera",
                       "OriginalCarCamera", "General", true, [](bool aftermarket) {
                           bool oem = !aftermarket;
                           std::printf("%s [HAL:REVCAM] Video/audio multiplexer set to %s\n",
                                       core::log_timestamp().c_str(),
                                       oem ? "OEM Factory feed" : "Aftermarket feed");
                           hal::send_mcu_video_relay(oem);
                       }, /*invert_stored_value=*/true);
    create_stepper_row(card, &ui::icons::icon_volume, "Reverse Vol. Cut (%)", 0, 100, 5,
                       "ReversingVolumeCut", "General");

    // --- Section 4: System ---
    create_section_header(card, "SYSTEM");
    create_toggle_row(card, &ui::icons::icon_smartphone, "Auto-Start CarLink",
                      "AutoStartCarLink", "General", true);

    // Phone Projection Mode: Android Auto vs Apple CarPlay
    {
        lv_obj_t * row = lv_obj_create(card);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, 72);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(row, 16, 0);
        lv_obj_set_style_pad_ver(row, 4, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t * left_box = lv_obj_create(row);
        lv_obj_remove_style_all(left_box);
        lv_obj_set_size(left_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(left_box, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(left_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(left_box, 14, 0);
        lv_obj_clear_flag(left_box, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t * icon = ui::icons::create_icon(left_box, &ui::icons::icon_smartphone, theme::text_secondary());
        (void)icon;

        lv_obj_t * label = lv_label_create(left_box);
        lv_label_set_text(label, "Phone Projection");
        lv_obj_set_style_text_font(label, &lv_font_roboto_20, 0);
        lv_obj_set_style_text_color(label, theme::text_primary(), 0);

        // Segmented button group: [ Android Auto | CarPlay ]
        lv_obj_t * btn_box = lv_obj_create(row);
        lv_obj_remove_style_all(btn_box);
        lv_obj_set_size(btn_box, 270, 52);
        lv_obj_set_flex_flow(btn_box, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(btn_box, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(btn_box, 10, 0);
        lv_obj_set_style_pad_all(btn_box, 6, 0);
        lv_obj_clear_flag(btn_box, LV_OBJ_FLAG_SCROLLABLE);

        std::string current_mode = core::default_store().get_string("ProjectionType", "AndroidAuto", "General");
        bool is_carplay = (current_mode == "CarPlay");

        lv_obj_t * aa_btn = lv_button_create(btn_box);
        lv_obj_remove_style_all(aa_btn);
        lv_obj_set_size(aa_btn, 120, 38);
        lv_obj_set_style_radius(aa_btn, theme::kPillRadius, 0);
        lv_obj_remove_flag(aa_btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
        theme::style_focusable(aa_btn);

        lv_obj_t * cp_btn = lv_button_create(btn_box);
        lv_obj_remove_style_all(cp_btn);
        lv_obj_set_size(cp_btn, 120, 38);
        lv_obj_set_style_radius(cp_btn, theme::kPillRadius, 0);
        lv_obj_remove_flag(cp_btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
        theme::style_focusable(cp_btn);

        auto update_btn_styles = [aa_btn, cp_btn](bool carplay_active) {
            // AA Button style
            lv_obj_set_style_bg_color(aa_btn, carplay_active ? theme::surface_container_high() : theme::accent_primary(), 0);
            lv_obj_set_style_bg_opa(aa_btn, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(aa_btn, carplay_active ? 1 : 0, 0);
            lv_obj_set_style_border_color(aa_btn, theme::surface_border(), 0);

            // CarPlay Button style
            lv_obj_set_style_bg_color(cp_btn, carplay_active ? theme::accent_primary() : theme::surface_container_high(), 0);
            lv_obj_set_style_bg_opa(cp_btn, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(cp_btn, carplay_active ? 0 : 1, 0);
            lv_obj_set_style_border_color(cp_btn, theme::surface_border(), 0);

            lv_obj_t * aa_lbl = lv_obj_get_child(aa_btn, 0);
            if (aa_lbl) {
                lv_obj_set_style_text_color(aa_lbl, carplay_active ? theme::text_primary() : theme::text_on_accent(), 0);
            }
            lv_obj_t * cp_lbl = lv_obj_get_child(cp_btn, 0);
            if (cp_lbl) {
                lv_obj_set_style_text_color(cp_lbl, carplay_active ? theme::text_on_accent() : theme::text_primary(), 0);
            }
        };

        lv_obj_t * aa_lbl = lv_label_create(aa_btn);
        lv_label_set_text(aa_lbl, "Android Auto");
        lv_obj_set_style_text_font(aa_lbl, &lv_font_roboto_14, 0);
        lv_obj_center(aa_lbl);

        lv_obj_t * cp_lbl = lv_label_create(cp_btn);
        lv_label_set_text(cp_lbl, "CarPlay");
        lv_obj_set_style_text_font(cp_lbl, &lv_font_roboto_14, 0);
        lv_obj_center(cp_lbl);

        update_btn_styles(is_carplay);

        struct ProjectionCtx {
            std::function<void(bool)> update_fn;
        };
        auto * ctx = new ProjectionCtx{update_btn_styles};
        lv_obj_add_event_cb(btn_box, [](lv_event_t * e) {
            delete static_cast<ProjectionCtx *>(lv_event_get_user_data(e));
        }, LV_EVENT_DELETE, ctx);

        lv_obj_add_event_cb(aa_btn, [](lv_event_t * e) {
            auto * ctx = static_cast<ProjectionCtx *>(lv_event_get_user_data(e));
            core::default_store().set_string("ProjectionType", "AndroidAuto", "General");
            core::default_store().save();
            ctx->update_fn(false);
            std::printf("%s [UI] Phone projection set to Android Auto\n", core::log_timestamp().c_str());
        }, LV_EVENT_CLICKED, ctx);

        lv_obj_add_event_cb(cp_btn, [](lv_event_t * e) {
            auto * ctx = static_cast<ProjectionCtx *>(lv_event_get_user_data(e));
            core::default_store().set_string("ProjectionType", "CarPlay", "General");
            core::default_store().save();
            ctx->update_fn(true);
            std::printf("%s [UI] Phone projection set to Apple CarPlay\n", core::log_timestamp().c_str());
        }, LV_EVENT_CLICKED, ctx);

        if (core::navigation::focus_group()) {
            lv_group_add_obj(core::navigation::focus_group(), aa_btn);
            lv_group_add_obj(core::navigation::focus_group(), cp_btn);
        }
    }

    // 12. SYSTEM & ABOUT SECTION
    create_section_header(card, "SYSTEM & ABOUT");

    /* 2026-09-01: root's password is intentionally empty on this rootfs
     * (see firmware_overlay_dyn/etc/ssh/sshd_config's PermitEmptyPasswords
     * yes) so SSH just works with zero prompts on the private carplay_wifi
     * network -- genuinely convenient, but also means sshd being reachable
     * at all is a real (private-network-only, but still real) exposure.
     * rcS no longer starts sshd unconditionally at boot -- this toggle
     * (persisted, applied at startup below and live here) is what actually
     * controls it, via hal::set_ssh_enabled(). See hal/ssh_access.h. */
    create_toggle_row(card, &ui::icons::icon_nav_settings, "SSH Access",
                       "SshAccess", "General", false, [](bool enabled) {
                           // 2026-09-04: real hardware bug -- set_ssh_enabled()
                           // shells out via std::system() (pidof/mkdir/sshd/
                           // killall), no timeout at all -- was running inline
                           // on the LVGL thread. Same fix as the volume steppers
                           // above: fire-and-forget on a detached thread.
                           core::SizedThread(core::kDefaultThreadStackSize, [enabled]() {
                               hal::set_ssh_enabled(enabled);
                           }).detach();
                       });
    {
        lv_obj_t * row = lv_obj_create(card);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, 56);
        lv_obj_set_style_bg_color(row, theme::surface_container_high(), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, theme::kCardRadius, 0);
        lv_obj_set_style_pad_hor(row, 16, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t * left_box = lv_obj_create(row);
        lv_obj_remove_style_all(left_box);
        lv_obj_set_size(left_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(left_box, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(left_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(left_box, 12, 0);
        lv_obj_clear_flag(left_box, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t * icon_img = lv_image_create(left_box);
        lv_image_set_src(icon_img, &icons::icon_nav_settings);

        lv_obj_t * text_box = lv_obj_create(left_box);
        lv_obj_remove_style_all(text_box);
        lv_obj_set_size(text_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(text_box, LV_FLEX_FLOW_COLUMN);
        lv_obj_clear_flag(text_box, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t * lbl = lv_label_create(text_box);
        lv_label_set_text(lbl, "System Information");
        lv_obj_set_style_text_font(lbl, &lv_font_roboto_14, 0);
        lv_obj_set_style_text_color(lbl, theme::text_primary(), 0);

        lv_obj_t * sub_lbl = lv_label_create(text_box);
        lv_label_set_text(sub_lbl, "Kernel, MCU, Bluetooth, and Hardware details");
        lv_obj_set_style_text_font(sub_lbl, &lv_font_roboto_14, 0);
        lv_obj_set_style_text_color(sub_lbl, theme::text_secondary(), 0);

        lv_obj_t * btn = lv_button_create(row);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, 110, 36);
        lv_obj_set_style_radius(btn, theme::kPillRadius, 0);
        lv_obj_set_style_bg_color(btn, theme::accent_primary(), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
        theme::style_focusable(btn);

        lv_obj_t * btn_lbl = lv_label_create(btn);
        lv_label_set_text(btn_lbl, "View Info >");
        lv_obj_set_style_text_font(btn_lbl, &lv_font_roboto_14, 0);
        lv_obj_set_style_text_color(btn_lbl, theme::text_on_accent(), 0);
        lv_obj_center(btn_lbl);

        lv_obj_add_event_cb(btn, [](lv_event_t * e) {
            // 2026-09-05: retrieved (not captured -- this must stay a
            // plain, non-capturing lambda to match lv_event_cb_t) so the
            // new scr-delete cleanup handler below can be registered
            // against the actual screen object.
            lv_obj_t * scr = static_cast<lv_obj_t *>(lv_event_get_user_data(e));
            // 2026-09-02: create_nav_rail() deliberately places the nav
            // rail on lv_layer_top() so it stays visible over every
            // screen (nav_rail.cpp's own comment). An overlay parented to
            // the settings screen itself therefore always renders BENEATH
            // the rail regardless of sibling add-order -- real hardware
            // bug report: "the mcu log window is partially hidden behind
            // the side bar" (same root cause hits this modal too, just
            // less obviously since it's opened less often). Fix: parent
            // to lv_layer_sys(), the one LVGL layer guaranteed above
            // lv_layer_top() -- no longer needs the screen pointer at all.
            //
            // 2026-09-02 follow-up: sized/positioned to the region beside
            // the rail (not the full 800px screen) so the modal centers
            // in the actual available card area with zero overlap of the
            // rail, rather than just no longer being covered by it.
            lv_obj_t * overlay = lv_obj_create(lv_layer_sys());
            lv_obj_remove_style_all(overlay);
            lv_obj_set_size(overlay, 800 - theme::kRailWidth, 480);
            lv_obj_set_pos(overlay, theme::kRailWidth, 0);
            lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);

            // Modal Card -- centers within the (rail-excluded) overlay above
            lv_obj_t * modal = lv_obj_create(overlay);
            lv_obj_remove_style_all(modal);
            theme::style_card(modal);
            lv_obj_set_size(modal, 600, 390);
            lv_obj_center(modal);
            lv_obj_set_flex_flow(modal, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(modal, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_all(modal, 18, 0);
            lv_obj_set_style_pad_row(modal, 6, 0);

            // Title
            lv_obj_t * title = lv_label_create(modal);
            lv_label_set_text(title, "System Information");
            lv_obj_set_style_text_font(title, &lv_font_roboto_20, 0);
            lv_obj_set_style_text_color(title, theme::accent_primary(), 0);

            // Gather Live System Details
            struct utsname uts;
            std::string kernel_ver = (uname(&uts) == 0) ? (std::string(uts.sysname) + " " + uts.release + " (" + uts.machine + ")") : "Linux 4.19.192";
            
            std::string mcu_ver = hal::get_mcu_version();
            if (mcu_ver.empty() || mcu_ver == "Unknown" || mcu_ver == "Unknown (Standalone)") {
                mcu_ver = "Limcet-V1.0-1302 (STM32F105)";
            }

            auto add_info_row = [](lv_obj_t * parent, const char * label, const std::string & value) -> lv_obj_t * {
                lv_obj_t * i_row = lv_obj_create(parent);
                lv_obj_remove_style_all(i_row);
                lv_obj_set_width(i_row, LV_PCT(100));
                lv_obj_set_height(i_row, LV_SIZE_CONTENT);
                lv_obj_set_flex_flow(i_row, LV_FLEX_FLOW_ROW);
                lv_obj_set_flex_align(i_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
                lv_obj_set_style_pad_ver(i_row, 3, 0);

                lv_obj_t * l = lv_label_create(i_row);
                lv_label_set_text(l, label);
                lv_obj_set_style_text_font(l, &lv_font_roboto_14, 0);
                lv_obj_set_style_text_color(l, theme::text_secondary(), 0);

                lv_obj_t * v = lv_label_create(i_row);
                lv_label_set_text(v, value.c_str());
                lv_obj_set_style_text_font(v, &lv_font_roboto_14, 0);
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
            lv_obj_remove_style_all(close_btn);
            lv_obj_set_size(close_btn, 140, 36);
            lv_obj_set_style_radius(close_btn, theme::kPillRadius, 0);
            lv_obj_set_style_bg_color(close_btn, theme::accent_primary(), 0);
            lv_obj_set_style_bg_opa(close_btn, LV_OPA_COVER, 0);
            lv_obj_set_style_margin_top(close_btn, 8, 0);
            theme::style_focusable(close_btn);

            lv_obj_t * close_label = lv_label_create(close_btn);
            lv_label_set_text(close_label, "Close");
            lv_obj_set_style_text_font(close_label, &lv_font_roboto_14, 0);
            lv_obj_set_style_text_color(close_label, theme::text_on_accent(), 0);
            lv_obj_center(close_label);

            // 2026-09-05: real hardware bug found via code review --
            // `overlay` is parented to lv_layer_sys(), NOT to `scr`, so
            // navigating away (which deletes `scr`) never cascades into
            // deleting this modal -- it was left stuck on top of
            // whatever screen the user navigated to, permanently
            // blocking touch there, unless the user happened to press
            // Close first. Fixed with a shared "closed" flag: whichever
            // fires first (the Close button, or scr's own deletion on
            // navigation) does the real cleanup and marks it done; the
            // other becomes a safe no-op.
            auto closed = std::make_shared<bool>(false);
            using OverlayCloseCtx = std::pair<lv_obj_t *, std::shared_ptr<bool>>;

            auto * close_btn_ctx = new OverlayCloseCtx(overlay, closed);
            lv_obj_add_event_cb(close_btn, [](lv_event_t * ev) {
                auto * ctx = static_cast<OverlayCloseCtx *>(lv_event_get_user_data(ev));
                if (!*ctx->second) {
                    lv_obj_delete(ctx->first);
                    *ctx->second = true;
                }
                delete ctx;
            }, LV_EVENT_CLICKED, close_btn_ctx);
            if (core::navigation::focus_group()) {
                lv_group_add_obj(core::navigation::focus_group(), close_btn);
            }

            auto * scr_close_ctx = new OverlayCloseCtx(overlay, closed);
            lv_obj_add_event_cb(scr, [](lv_event_t * ev) {
                auto * ctx = static_cast<OverlayCloseCtx *>(lv_event_get_user_data(ev));
                if (!*ctx->second) {
                    lv_obj_delete(ctx->first);
                    *ctx->second = true;
                }
                delete ctx;
            }, LV_EVENT_DELETE, scr_close_ctx);
        }, LV_EVENT_CLICKED, scr);

        if (core::navigation::focus_group()) {
            lv_group_add_obj(core::navigation::focus_group(), btn);
        }
    }

    // 13. MCU LIVE LOG (2026-08-31) -- live view of real, decoded MCU UART
    // traffic (every frame, any cmd), so protocol questions can be
    // resolved by watching custom_ui's own settings toggles/knob input/
    // reverse-gear transitions live, without the stock app's separate
    // (and single-screen-at-a-time) factory-menu MCU Monitor, and
    // without a physical UART tap -- this HAL already reads the port for
    // the running app, so it can just expose what it's already seeing.
    // See hal::McuInputHal::get_recent_frames()'s own header comment.
    {
        lv_obj_t * row = lv_obj_create(card);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, 56);
        lv_obj_set_style_bg_color(row, theme::surface_container_high(), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, theme::kCardRadius, 0);
        lv_obj_set_style_pad_hor(row, 16, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t * left_box = lv_obj_create(row);
        lv_obj_remove_style_all(left_box);
        lv_obj_set_size(left_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(left_box, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(left_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(left_box, 12, 0);
        lv_obj_clear_flag(left_box, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t * icon_img = lv_image_create(left_box);
        lv_image_set_src(icon_img, &icons::icon_nav_settings);

        lv_obj_t * text_box = lv_obj_create(left_box);
        lv_obj_remove_style_all(text_box);
        lv_obj_set_size(text_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(text_box, LV_FLEX_FLOW_COLUMN);
        lv_obj_clear_flag(text_box, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t * lbl = lv_label_create(text_box);
        lv_label_set_text(lbl, "MCU Live Log");
        lv_obj_set_style_text_font(lbl, &lv_font_roboto_14, 0);
        lv_obj_set_style_text_color(lbl, theme::text_primary(), 0);

        lv_obj_t * sub_lbl = lv_label_create(text_box);
        lv_label_set_text(sub_lbl, "Live decoded UART frames to/from the MCU");
        lv_obj_set_style_text_font(sub_lbl, &lv_font_roboto_14, 0);
        lv_obj_set_style_text_color(sub_lbl, theme::text_secondary(), 0);

        lv_obj_t * btn = lv_button_create(row);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, 110, 36);
        lv_obj_set_style_radius(btn, theme::kPillRadius, 0);
        lv_obj_set_style_bg_color(btn, theme::accent_primary(), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
        theme::style_focusable(btn);

        lv_obj_t * btn_lbl = lv_label_create(btn);
        lv_label_set_text(btn_lbl, "View Log >");
        lv_obj_set_style_text_font(btn_lbl, &lv_font_roboto_14, 0);
        lv_obj_set_style_text_color(btn_lbl, theme::text_on_accent(), 0);
        lv_obj_center(btn_lbl);

        lv_obj_add_event_cb(btn, [](lv_event_t * e) {
            // 2026-09-05: retrieved (not captured -- this must stay a
            // plain, non-capturing lambda to match lv_event_cb_t) so the
            // new scr-delete cleanup handler below can be registered
            // against the actual screen object.
            lv_obj_t * scr = static_cast<lv_obj_t *>(lv_event_get_user_data(e));
            // 2026-09-02: real hardware bug report -- "the mcu log window
            // is partially hidden behind the side bar." Root cause:
            // create_nav_rail() deliberately parents the nav rail to
            // lv_layer_top() so it stays visible over every screen
            // (nav_rail.cpp's own comment); an overlay parented to the
            // settings screen itself always renders BENEATH that layer
            // regardless of sibling add-order. Fixed by parenting to
            // lv_layer_sys(), the one LVGL layer guaranteed above
            // lv_layer_top() -- no longer needs the screen pointer at all.
            //
            // 2026-09-02 follow-up: sized/positioned to the region beside
            // the rail (not the full 800px screen) so the modal centers
            // in the actual available card area with zero overlap of the
            // rail, rather than just no longer being covered by it. Also
            // narrowed 720->680 -- 720 left only 4px of margin inside the
            // 728px-wide (800 - kRailWidth) available area, too tight to
            // read comfortably as a real margin.
            lv_obj_t * overlay = lv_obj_create(lv_layer_sys());
            lv_obj_remove_style_all(overlay);
            lv_obj_set_size(overlay, 800 - theme::kRailWidth, 480);
            lv_obj_set_pos(overlay, theme::kRailWidth, 0);
            lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);
            lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

            // Modal card -- centers within the (rail-excluded) overlay above
            lv_obj_t * modal = lv_obj_create(overlay);
            lv_obj_remove_style_all(modal);
            theme::style_card(modal);
            lv_obj_set_size(modal, 680, 420);
            lv_obj_center(modal);
            lv_obj_set_flex_flow(modal, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(modal, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_all(modal, 18, 0);
            lv_obj_set_style_pad_row(modal, 8, 0);
            lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t * header_row = lv_obj_create(modal);
            lv_obj_remove_style_all(header_row);
            lv_obj_set_width(header_row, LV_PCT(100));
            lv_obj_set_height(header_row, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(header_row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(header_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_clear_flag(header_row, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t * title = lv_label_create(header_row);
            lv_label_set_text(title, "MCU Live Log");
            lv_obj_set_style_text_font(title, &lv_font_roboto_20, 0);
            lv_obj_set_style_text_color(title, theme::accent_primary(), 0);

            lv_obj_t * scroll_status = lv_label_create(header_row);
            lv_label_set_text(scroll_status, "Tracking Live");
            lv_obj_set_style_text_font(scroll_status, &lv_font_roboto_14, 0);
            lv_obj_set_style_text_color(scroll_status, theme::success(), 0);

            // Scrollable log body -- the timer below keeps this in sync
            // with hal::get_mcu_recent_frames() while the modal is open.
            lv_obj_t * log_box = lv_obj_create(modal);
            lv_obj_remove_style_all(log_box);
            lv_obj_set_width(log_box, LV_PCT(100));
            lv_obj_set_flex_grow(log_box, 1);
            lv_obj_set_style_bg_color(log_box, theme::surface_container_high(), 0);
            lv_obj_set_style_bg_opa(log_box, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(log_box, theme::kCardRadius, 0);
            lv_obj_set_style_pad_all(log_box, 8, 0);
            lv_obj_set_scroll_dir(log_box, LV_DIR_VER);
            lv_obj_add_flag(log_box, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(log_box, LV_OBJ_FLAG_SCROLL_MOMENTUM);
            lv_obj_remove_flag(log_box, LV_OBJ_FLAG_SCROLL_ELASTIC);
            lv_obj_set_scrollbar_mode(log_box, LV_SCROLLBAR_MODE_AUTO);

            lv_obj_t * log_lbl = lv_label_create(log_box);
            lv_label_set_long_mode(log_lbl, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(log_lbl, LV_PCT(100));
            lv_label_set_text(log_lbl, "(waiting for MCU frames...)");
            lv_obj_set_style_text_font(log_lbl, &lv_font_roboto_14, 0);
            lv_obj_set_style_text_color(log_lbl, theme::text_primary(), 0);

            // 2026-09-01: real hardware feedback -- CMD 0x20 (touch) frames
            // fire on every touch sample and drown out everything else in
            // the ring buffer / visible history. Filtered out of the
            // DISPLAY by default (the ring buffer itself still captures
            // them -- get_mcu_recent_frames() is unfiltered -- this is a
            // view-level filter only, cheap and simple, doesn't touch the
            // HAL). Toggle the switch below to show them again. Using
            // lv_switch, not lv_checkbox -- LV_USE_CHECKBOX is disabled in
            // this project's lv_conf.h (would need a full LVGL rebuild),
            // and lv_switch is already the one boolean-toggle widget used
            // everywhere else in this app.
            lv_obj_t * touch_filter_row = lv_obj_create(modal);
            lv_obj_remove_style_all(touch_filter_row);
            lv_obj_set_width(touch_filter_row, LV_PCT(100));
            lv_obj_set_height(touch_filter_row, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(touch_filter_row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(touch_filter_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_top(touch_filter_row, 4, 0);
            lv_obj_clear_flag(touch_filter_row, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t * touch_filter_label = lv_label_create(touch_filter_row);
            lv_label_set_text(touch_filter_label, "Show touch events (CMD 0x20)");
            lv_obj_set_style_text_font(touch_filter_label, &lv_font_roboto_14, 0);
            lv_obj_set_style_text_color(touch_filter_label, theme::text_secondary(), 0);

            lv_obj_t * touch_filter_cb = lv_switch_create(touch_filter_row);
            if (core::navigation::focus_group()) {
                lv_group_add_obj(core::navigation::focus_group(), touch_filter_cb);
            }

            struct LogViewCtx {
                lv_obj_t * label;
                lv_obj_t * status_label;
                lv_obj_t * touch_filter_cb;
                std::string last_text;
                bool auto_scroll = true;
            };
            auto * log_ctx = new LogViewCtx{log_lbl, scroll_status, touch_filter_cb, "", true};

            // Detect user drag scrolling: if user scrolls back into history, pause auto-scroll
            // so there is zero snapback; resume tracking when scrolled back to bottom.
            lv_obj_add_event_cb(log_box, [](lv_event_t * e) {
                auto * ctx = static_cast<LogViewCtx *>(lv_event_get_user_data(e));
                lv_obj_t * box = static_cast<lv_obj_t *>(lv_event_get_target(e));
                int32_t sb = lv_obj_get_scroll_bottom(box);
                if (sb <= 20) {
                    if (!ctx->auto_scroll) {
                        ctx->auto_scroll = true;
                        if (ctx->status_label) {
                            lv_label_set_text(ctx->status_label, "Tracking Live");
                            lv_obj_set_style_text_color(ctx->status_label, theme::success(), 0);
                        }
                    }
                } else {
                    if (ctx->auto_scroll) {
                        ctx->auto_scroll = false;
                        if (ctx->status_label) {
                            lv_label_set_text(ctx->status_label, "History (tap Live to track)");
                            lv_obj_set_style_text_color(ctx->status_label, theme::text_secondary(), 0);
                        }
                    }
                }
            }, LV_EVENT_SCROLL, log_ctx);

            // Rotary knob navigation: turning tuning knob scrolls through log history
            lv_obj_add_event_cb(log_box, [](lv_event_t * e) {
                auto * ctx = static_cast<LogViewCtx *>(lv_event_get_user_data(e));
                uint32_t key = lv_event_get_key(e);
                lv_obj_t * box = static_cast<lv_obj_t *>(lv_event_get_target(e));
                if (key == LV_KEY_UP || key == LV_KEY_PREV) {
                    ctx->auto_scroll = false;
                    if (ctx->status_label) {
                        lv_label_set_text(ctx->status_label, "History (tap Live to track)");
                        lv_obj_set_style_text_color(ctx->status_label, theme::text_secondary(), 0);
                    }
                    lv_obj_scroll_by_bounded(box, 0, 60, LV_ANIM_OFF);
                } else if (key == LV_KEY_DOWN || key == LV_KEY_NEXT) {
                    lv_obj_scroll_by_bounded(box, 0, -60, LV_ANIM_OFF);
                    if (lv_obj_get_scroll_bottom(box) <= 20) {
                        ctx->auto_scroll = true;
                        if (ctx->status_label) {
                            lv_label_set_text(ctx->status_label, "Tracking Live");
                            lv_obj_set_style_text_color(ctx->status_label, theme::success(), 0);
                        }
                    }
                }
            }, LV_EVENT_KEY, log_ctx);

            auto resume_live_tracking = [](LogViewCtx * ctx) {
                ctx->auto_scroll = true;
                ctx->last_text.clear();
                if (ctx->status_label) {
                    lv_label_set_text(ctx->status_label, "Tracking Live");
                    lv_obj_set_style_text_color(ctx->status_label, theme::success(), 0);
                }
                bool show_touch = lv_obj_has_state(ctx->touch_filter_cb, LV_STATE_CHECKED);
                auto frames = hal::get_mcu_recent_frames();
                std::string text;
                constexpr size_t kShowLast = 150;
                size_t shown = 0;
                for (auto it = frames.rbegin(); it != frames.rend() && shown < kShowLast; ++it) {
                    if (!show_touch && it->find("cmd=0x20 ") != std::string::npos) {
                        continue;
                    }
                    text = *it + "\n" + text;
                    ++shown;
                }
                if (text.empty()) {
                    text = "(waiting for MCU frames...)";
                }
                ctx->last_text = text;
                lv_label_set_text(ctx->label, text.c_str());
                lv_obj_t * box = lv_obj_get_parent(ctx->label);
                lv_obj_scroll_to_y(box, LV_COORD_MAX, LV_ANIM_OFF);
            };

            // Tapping status badge resumes live tracking immediately
            lv_obj_add_flag(scroll_status, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(scroll_status, [](lv_event_t * e) {
                auto * ctx = static_cast<LogViewCtx *>(lv_event_get_user_data(e));
                auto * resume_fn = static_cast<decltype(resume_live_tracking)*>(lv_event_get_param(e));
                (void)resume_fn;
                ctx->auto_scroll = true;
                ctx->last_text.clear();
                if (ctx->status_label) {
                    lv_label_set_text(ctx->status_label, "Tracking Live");
                    lv_obj_set_style_text_color(ctx->status_label, theme::success(), 0);
                }
                lv_obj_t * box = lv_obj_get_parent(ctx->label);
                lv_obj_scroll_to_y(box, LV_COORD_MAX, LV_ANIM_OFF);
            }, LV_EVENT_CLICKED, log_ctx);

            lv_obj_add_event_cb(touch_filter_cb, [](lv_event_t * e) {
                auto * ctx = static_cast<LogViewCtx *>(lv_event_get_user_data(e));
                ctx->auto_scroll = true;
                ctx->last_text.clear();
                if (ctx->status_label) {
                    lv_label_set_text(ctx->status_label, "Tracking Live");
                    lv_obj_set_style_text_color(ctx->status_label, theme::success(), 0);
                }
                lv_obj_t * box = lv_obj_get_parent(ctx->label);
                lv_obj_scroll_to_y(box, LV_COORD_MAX, LV_ANIM_OFF);
            }, LV_EVENT_VALUE_CHANGED, log_ctx);

            // Refresh timer: pulls recent frames every 300ms.
            // When auto_scroll is enabled, auto-scrolls to the newest frame.
            // When reviewing earlier logs (auto_scroll == false), completely pauses label updates
            // so text position and layout remain 100% rock-solid without any snapback.
            lv_timer_t * refresh_timer = lv_timer_create([](lv_timer_t * t) {
                auto * ctx = static_cast<LogViewCtx *>(lv_timer_get_user_data(t));
                if (!ctx->auto_scroll) {
                    return;
                }
                lv_obj_t * box = lv_obj_get_parent(ctx->label);
                bool show_touch = lv_obj_has_state(ctx->touch_filter_cb, LV_STATE_CHECKED);
                auto frames = hal::get_mcu_recent_frames();
                std::string text;
                constexpr size_t kShowLast = 150;
                size_t shown = 0;
                for (auto it = frames.rbegin(); it != frames.rend() && shown < kShowLast; ++it) {
                    if (!show_touch && it->find("cmd=0x20 ") != std::string::npos) {
                        continue;
                    }
                    text = *it + "\n" + text;
                    ++shown;
                }
                if (text.empty()) {
                    text = "(waiting for MCU frames...)";
                }

                if (text != ctx->last_text) {
                    ctx->last_text = text;
                    lv_label_set_text(ctx->label, text.c_str());
                    if (!lv_obj_is_scrolling(box)) {
                        lv_obj_scroll_to_y(box, LV_COORD_MAX, LV_ANIM_OFF);
                    }
                }
            }, 300, log_ctx);

            // Bottom action bar: Close button on left, Earlier/Later/Live scroll controls on right
            lv_obj_t * footer_row = lv_obj_create(modal);
            lv_obj_remove_style_all(footer_row);
            lv_obj_set_width(footer_row, LV_PCT(100));
            lv_obj_set_height(footer_row, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(footer_row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(footer_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_top(footer_row, 4, 0);
            lv_obj_clear_flag(footer_row, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t * close_btn = lv_button_create(footer_row);
            lv_obj_remove_style_all(close_btn);
            lv_obj_set_size(close_btn, 110, 36);
            lv_obj_set_style_radius(close_btn, theme::kPillRadius, 0);
            lv_obj_set_style_bg_color(close_btn, theme::surface_container_high(), 0);
            lv_obj_set_style_bg_opa(close_btn, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(close_btn, 1, 0);
            lv_obj_set_style_border_color(close_btn, theme::surface_border(), 0);
            theme::style_focusable(close_btn);

            lv_obj_t * close_label = lv_label_create(close_btn);
            lv_label_set_text(close_label, "Close");
            lv_obj_set_style_text_font(close_label, &lv_font_roboto_14, 0);
            lv_obj_set_style_text_color(close_label, theme::text_primary(), 0);
            lv_obj_center(close_label);

            lv_obj_t * nav_btns = lv_obj_create(footer_row);
            lv_obj_remove_style_all(nav_btns);
            lv_obj_set_size(nav_btns, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(nav_btns, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(nav_btns, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(nav_btns, 8, 0);
            lv_obj_clear_flag(nav_btns, LV_OBJ_FLAG_SCROLLABLE);

            // Earlier (Scroll Up) Button
            lv_obj_t * earlier_btn = lv_button_create(nav_btns);
            lv_obj_remove_style_all(earlier_btn);
            lv_obj_set_size(earlier_btn, 104, 36);
            lv_obj_set_style_radius(earlier_btn, theme::kPillRadius, 0);
            lv_obj_set_style_bg_color(earlier_btn, theme::surface_container_high(), 0);
            lv_obj_set_style_bg_opa(earlier_btn, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(earlier_btn, 1, 0);
            lv_obj_set_style_border_color(earlier_btn, theme::surface_border(), 0);
            theme::style_focusable(earlier_btn);

            lv_obj_t * earlier_lbl = lv_label_create(earlier_btn);
            lv_label_set_text(earlier_lbl, "Earlier");
            lv_obj_set_style_text_font(earlier_lbl, &lv_font_roboto_14, 0);
            lv_obj_set_style_text_color(earlier_lbl, theme::text_primary(), 0);
            lv_obj_center(earlier_lbl);

            lv_obj_add_event_cb(earlier_btn, [](lv_event_t * e) {
                auto * ctx = static_cast<LogViewCtx *>(lv_event_get_user_data(e));
                ctx->auto_scroll = false;
                if (ctx->status_label) {
                    lv_label_set_text(ctx->status_label, "History (tap Live to track)");
                    lv_obj_set_style_text_color(ctx->status_label, theme::text_secondary(), 0);
                }
                lv_obj_t * box = lv_obj_get_parent(ctx->label);
                lv_obj_scroll_by_bounded(box, 0, 180, LV_ANIM_OFF);
            }, LV_EVENT_CLICKED, log_ctx);

            // Later (Scroll Down) Button
            lv_obj_t * later_btn = lv_button_create(nav_btns);
            lv_obj_remove_style_all(later_btn);
            lv_obj_set_size(later_btn, 96, 36);
            lv_obj_set_style_radius(later_btn, theme::kPillRadius, 0);
            lv_obj_set_style_bg_color(later_btn, theme::surface_container_high(), 0);
            lv_obj_set_style_bg_opa(later_btn, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(later_btn, 1, 0);
            lv_obj_set_style_border_color(later_btn, theme::surface_border(), 0);
            theme::style_focusable(later_btn);

            lv_obj_t * later_lbl = lv_label_create(later_btn);
            lv_label_set_text(later_lbl, "Later");
            lv_obj_set_style_text_font(later_lbl, &lv_font_roboto_14, 0);
            lv_obj_set_style_text_color(later_lbl, theme::text_primary(), 0);
            lv_obj_center(later_lbl);

            lv_obj_add_event_cb(later_btn, [](lv_event_t * e) {
                auto * ctx = static_cast<LogViewCtx *>(lv_event_get_user_data(e));
                lv_obj_t * box = lv_obj_get_parent(ctx->label);
                lv_obj_scroll_by_bounded(box, 0, -180, LV_ANIM_OFF);
                int32_t sb = lv_obj_get_scroll_bottom(box);
                if (sb <= 20) {
                    ctx->auto_scroll = true;
                    if (ctx->status_label) {
                        lv_label_set_text(ctx->status_label, "Tracking Live");
                        lv_obj_set_style_text_color(ctx->status_label, theme::success(), 0);
                    }
                }
            }, LV_EVENT_CLICKED, log_ctx);

            // Live (Scroll to Bottom & Resume Track) Button
            lv_obj_t * live_btn = lv_button_create(nav_btns);
            lv_obj_remove_style_all(live_btn);
            lv_obj_set_size(live_btn, 90, 36);
            lv_obj_set_style_radius(live_btn, theme::kPillRadius, 0);
            lv_obj_set_style_bg_color(live_btn, theme::accent_primary(), 0);
            lv_obj_set_style_bg_opa(live_btn, LV_OPA_COVER, 0);
            theme::style_focusable(live_btn);

            lv_obj_t * live_lbl = lv_label_create(live_btn);
            lv_label_set_text(live_lbl, "Live");
            lv_obj_set_style_text_font(live_lbl, &lv_font_roboto_14, 0);
            lv_obj_set_style_text_color(live_lbl, theme::text_on_accent(), 0);
            lv_obj_center(live_lbl);

            lv_obj_add_event_cb(live_btn, [](lv_event_t * e) {
                auto * ctx = static_cast<LogViewCtx *>(lv_event_get_user_data(e));
                ctx->auto_scroll = true;
                ctx->last_text.clear();
                if (ctx->status_label) {
                    lv_label_set_text(ctx->status_label, "Tracking Live");
                    lv_obj_set_style_text_color(ctx->status_label, theme::success(), 0);
                }
                bool show_touch = lv_obj_has_state(ctx->touch_filter_cb, LV_STATE_CHECKED);
                auto frames = hal::get_mcu_recent_frames();
                std::string text;
                constexpr size_t kShowLast = 150;
                size_t shown = 0;
                for (auto it = frames.rbegin(); it != frames.rend() && shown < kShowLast; ++it) {
                    if (!show_touch && it->find("cmd=0x20 ") != std::string::npos) {
                        continue;
                    }
                    text = *it + "\n" + text;
                    ++shown;
                }
                if (text.empty()) {
                    text = "(waiting for MCU frames...)";
                }
                ctx->last_text = text;
                lv_label_set_text(ctx->label, text.c_str());
                lv_obj_t * box = lv_obj_get_parent(ctx->label);
                lv_obj_scroll_to_y(box, LV_COORD_MAX, LV_ANIM_OFF);
            }, LV_EVENT_CLICKED, log_ctx);

            // 2026-09-05: real hardware bug found via code review --
            // same issue as the System Information modal above, worse
            // here since this one also has a 300ms refresh_timer:
            // `overlay` lives on lv_layer_sys(), not as a child of
            // `scr`, so navigating away without pressing Close left
            // both the modal AND its timer running forever in the
            // background -- permanently blocking touch on whatever
            // screen the user navigated to, while leaking CPU. Same
            // shared "closed" flag fix as that modal: whichever fires
            // first (Close button or scr's own deletion) does the real
            // cleanup, the other becomes a safe no-op.
            struct CloseCtx {
                lv_obj_t * overlay;
                lv_timer_t * timer;
                LogViewCtx * log_ctx;
                std::shared_ptr<bool> closed;
            };
            auto closed = std::make_shared<bool>(false);
            auto * close_ctx = new CloseCtx{overlay, refresh_timer, log_ctx, closed};

            lv_obj_add_event_cb(close_btn, [](lv_event_t * ev) {
                auto * ctx = static_cast<CloseCtx *>(lv_event_get_user_data(ev));
                if (!*ctx->closed) {
                    lv_timer_delete(ctx->timer);
                    lv_obj_delete(ctx->overlay);
                    delete ctx->log_ctx;
                    *ctx->closed = true;
                }
                delete ctx;
            }, LV_EVENT_CLICKED, close_ctx);

            if (core::navigation::focus_group()) {
                lv_group_add_obj(core::navigation::focus_group(), close_btn);
                lv_group_add_obj(core::navigation::focus_group(), earlier_btn);
                lv_group_add_obj(core::navigation::focus_group(), later_btn);
                lv_group_add_obj(core::navigation::focus_group(), live_btn);
            }

            auto * scr_close_ctx = new CloseCtx{overlay, refresh_timer, log_ctx, closed};
            lv_obj_add_event_cb(scr, [](lv_event_t * ev) {
                auto * ctx = static_cast<CloseCtx *>(lv_event_get_user_data(ev));
                if (!*ctx->closed) {
                    lv_timer_delete(ctx->timer);
                    lv_obj_delete(ctx->overlay);
                    delete ctx->log_ctx;
                    *ctx->closed = true;
                }
                delete ctx;
            }, LV_EVENT_DELETE, scr_close_ctx);
        }, LV_EVENT_CLICKED, scr);

        if (core::navigation::focus_group()) {
            lv_group_add_obj(core::navigation::focus_group(), btn);
        }
    }

    return scr;
}

} // namespace staging_ui

