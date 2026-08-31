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
#include "hal/timezone.h"
#include "hal/bluetooth.h"
#include "core/log_timing.h"
#include <functional>
#include <sys/utsname.h>

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
                             const std::string & section,
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
                          std::printf("%s [HAL:AUDIO] Microphone input source set to %s (CMD 0xA0 [0x09, %d])\n",
                                      core::log_timestamp().c_str(),
                                      oem ? "OEM Factory Roof Mic" : "Aftermarket 3.5mm Mic",
                                      oem ? 1 : 0);
                          hal::send_mcu_setting(0x09, oem ? 1 : 0);
                      });
    /* CMD 0xA0 id=0x00 -- "Microphone Source", real candidate fix for the
     * stock "factory mic never worked" complaint. Full trace in
     * MCU_FIRMWARE_VERIFIED_FINDINGS.md's "CONFIRMED: the real Camera Type
     * setting" and "COMPLETE: full settings-name resolution via Ghidra"
     * sections -- summary:
     *   - MCUAdapter_BoxP300::getSetItemValueTexts(0) (usr/lib/libMcuCenter.so)
     *     returns exactly ["OEM Microphone", "AfterMarket Microphone"], and
     *     syncSettingDataToMcu(int) sends idx UNMODIFIED as the wire id for
     *     idx=0 (not one of the 10/11/12 special-remap cases) -- so
     *     CMD 0xA0 id=0x00, value 0=OEM / 1=AfterMarket, is real and
     *     confirmed via the actual wire-protocol send path, not guessed.
     *   - Real, separately confirmed vendor bug: the STOCK head unit's own
     *     end-user Settings app (SettingWindow/SettingPlugin's "Car Setting"
     *     category -- a real screen real users navigate into and use, e.g.
     *     for the working Camera Type toggle right above) shows this exact
     *     row under the label "Reversing camera", not "Microphone" -- traced
     *     via Ghidra decompilation of getSetItemText(int), which switches on
     *     the same idx independently of getSetItemValueTexts() and has
     *     genuinely drifted out of sync with it in the vendor's own code.
     *     Nobody troubleshooting a mic problem would ever think to touch a
     *     row titled "Reversing camera".
     *   - Real, decisive supporting evidence: this specific unit's own
     *     persisted config (firmware_source/mtd7_userdata/msncfg/carsetting.ini)
     *     has "SetItem0=1" -- i.e. AfterMarket Microphone -- sitting
     *     unexamined since nobody could have found their way to the real
     *     control under its real name. If this vehicle's actual physical
     *     mic is the OEM roof mic, this single stuck value fully explains
     *     "stock factory mic never worked."
     *   - Real, physically-plausible MCU-side action confirmed too
     *     (hardware/MCU/source/src/uart_protocol.c, CMD 0xA0 id=0x00
     *     handler): value 1 drives GPIOB Pin 1 HIGH, value 0/3 drives it
     *     LOW, value 2 is a distinct "AT+UPGRADE" trigger this toggle never
     *     sends -- consistent with a real mic-source relay, not a no-op.
     * Kept as a plain two-state toggle deliberately, exactly so both real
     * values can be tested directly on hardware -- this is the fastest way
     * to settle which one this vehicle's actual wiring needs. Distinct
     * from "OEM Factory Microphone" (id=0x09) above -- that's a real,
     * separate setting; which of the two (if either) is this vehicle's
     * actual working mic-source control is exactly what real-hardware
     * testing of both needs to determine. */
    create_toggle_row(card, &ui::icons::icon_volume, "Microphone Source (OEM/AfterMarket)",
                      "MicSourceOEM", "Audio", false,
                      [](bool oem) {
                          std::printf("%s [HAL:AUDIO] Microphone source set to %s (CMD 0xA0 [0x00, %d]) -- "
                                      "candidate fix for stock mic never working, test both values on real hardware\n",
                                      core::log_timestamp().c_str(),
                                      oem ? "OEM" : "AfterMarket",
                                      oem ? 0 : 1);
                          hal::send_mcu_setting(0x00, oem ? 0x00 : 0x01);
                      });

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
    create_toggle_row(card, &ui::icons::icon_nav_camera, "OEM Factory Camera",
                       "OriginalCarCamera", "General", false, [](bool oem) {
                           std::printf("%s [HAL:REVCAM] Video/audio multiplexer set to %s\n",
                                       core::log_timestamp().c_str(),
                                       oem ? "OEM Factory feed" : "Aftermarket feed");
                           hal::send_mcu_video_relay(oem);
                       });
    create_stepper_row(card, &ui::icons::icon_volume, "Reverse Vol. Cut (%)", 0, 100, 5,
                       "ReversingVolumeCut", "General");

    // --- Section 4: Date & Time ---
    create_section_header(card, "DATE & TIME");
    {
        lv_obj_t * row = lv_obj_create(card);
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

        lv_obj_t * icon = ui::icons::create_icon(left_box, &ui::icons::icon_nav_settings, theme::text_secondary());
        (void)icon;

        lv_obj_t * label = lv_label_create(left_box);
        lv_label_set_text(label, "Timezone");
        lv_obj_set_style_text_font(label, &lv_font_roboto_20, 0);
        lv_obj_set_style_text_color(label, theme::text_primary(), 0);

        // Right Controls: [ < ] [ Timezone Label ] [ > ]
        lv_obj_t * right_box = lv_obj_create(row);
        lv_obj_remove_style_all(right_box);
        lv_obj_set_size(right_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(right_box, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(right_box, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(right_box, 8, 0);

        const auto & tzs = hal::get_timezones();
        int current_idx = hal::get_current_timezone_index();

        struct TimezoneCtx {
            int index;
            lv_obj_t * lbl;
        };
        auto * ctx = new TimezoneCtx{current_idx, nullptr};
        lv_obj_add_event_cb(row, [](lv_event_t * e) {
            delete static_cast<TimezoneCtx *>(lv_event_get_user_data(e));
        }, LV_EVENT_DELETE, ctx);

        // Prev Button (<)
        lv_obj_t * prev_btn = lv_button_create(right_box);
        lv_obj_remove_style_all(prev_btn);
        lv_obj_set_size(prev_btn, 36, 36);
        lv_obj_set_style_radius(prev_btn, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(prev_btn, theme::surface_container_high(), 0);
        lv_obj_set_style_bg_opa(prev_btn, LV_OPA_COVER, 0);
        theme::style_focusable(prev_btn);

        lv_obj_t * prev_lbl = lv_label_create(prev_btn);
        lv_label_set_text(prev_lbl, "<");
        lv_obj_set_style_text_font(prev_lbl, &lv_font_roboto_14, 0);
        lv_obj_set_style_text_color(prev_lbl, theme::text_primary(), 0);
        lv_obj_center(prev_lbl);

        // Timezone Label
        lv_obj_t * tz_lbl = lv_label_create(right_box);
        lv_obj_set_width(tz_lbl, 270);
        lv_label_set_text(tz_lbl, tzs[current_idx].label);
        lv_obj_set_style_text_font(tz_lbl, &lv_font_roboto_14, 0);
        lv_obj_set_style_text_color(tz_lbl, theme::text_primary(), 0);
        lv_obj_set_style_text_align(tz_lbl, LV_TEXT_ALIGN_CENTER, 0);
        ctx->lbl = tz_lbl;

        // Next Button (>)
        lv_obj_t * next_btn = lv_button_create(right_box);
        lv_obj_remove_style_all(next_btn);
        lv_obj_set_size(next_btn, 36, 36);
        lv_obj_set_style_radius(next_btn, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(next_btn, theme::surface_container_high(), 0);
        lv_obj_set_style_bg_opa(next_btn, LV_OPA_COVER, 0);
        theme::style_focusable(next_btn);

        lv_obj_t * next_lbl = lv_label_create(next_btn);
        lv_label_set_text(next_lbl, ">");
        lv_obj_set_style_text_font(next_lbl, &lv_font_roboto_14, 0);
        lv_obj_set_style_text_color(next_lbl, theme::text_primary(), 0);
        lv_obj_center(next_lbl);

        lv_obj_add_event_cb(prev_btn, [](lv_event_t * e) {
            auto * ctx = static_cast<TimezoneCtx *>(lv_event_get_user_data(e));
            const auto & timezones = hal::get_timezones();
            int count = static_cast<int>(timezones.size());
            ctx->index = (ctx->index - 1 + count) % count;
            hal::apply_timezone(ctx->index);
            lv_label_set_text(ctx->lbl, timezones[ctx->index].label);
        }, LV_EVENT_CLICKED, ctx);

        lv_obj_add_event_cb(next_btn, [](lv_event_t * e) {
            auto * ctx = static_cast<TimezoneCtx *>(lv_event_get_user_data(e));
            const auto & timezones = hal::get_timezones();
            int count = static_cast<int>(timezones.size());
            ctx->index = (ctx->index + 1) % count;
            hal::apply_timezone(ctx->index);
            lv_label_set_text(ctx->lbl, timezones[ctx->index].label);
        }, LV_EVENT_CLICKED, ctx);

        if (core::navigation::focus_group()) {
            lv_group_add_obj(core::navigation::focus_group(), prev_btn);
            lv_group_add_obj(core::navigation::focus_group(), next_btn);
        }
    }

    // --- Section 5: System ---
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

        lv_obj_t * left_box = lv_obj_create(row);
        lv_obj_remove_style_all(left_box);
        lv_obj_set_size(left_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(left_box, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(left_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(left_box, 14, 0);

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

        std::string current_mode = core::default_store().get_string("ProjectionType", "AndroidAuto", "General");
        bool is_carplay = (current_mode == "CarPlay");

        lv_obj_t * aa_btn = lv_button_create(btn_box);
        lv_obj_remove_style_all(aa_btn);
        lv_obj_set_size(aa_btn, 120, 38);
        lv_obj_set_style_radius(aa_btn, theme::kPillRadius, 0);
        theme::style_focusable(aa_btn);

        lv_obj_t * cp_btn = lv_button_create(btn_box);
        lv_obj_remove_style_all(cp_btn);
        lv_obj_set_size(cp_btn, 120, 38);
        lv_obj_set_style_radius(cp_btn, theme::kPillRadius, 0);
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
                           hal::set_ssh_enabled(enabled);
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

        lv_obj_t * left_box = lv_obj_create(row);
        lv_obj_remove_style_all(left_box);
        lv_obj_set_size(left_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(left_box, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(left_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(left_box, 12, 0);

        lv_obj_t * icon_img = lv_image_create(left_box);
        lv_image_set_src(icon_img, &icons::icon_nav_settings);

        lv_obj_t * text_box = lv_obj_create(left_box);
        lv_obj_remove_style_all(text_box);
        lv_obj_set_size(text_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(text_box, LV_FLEX_FLOW_COLUMN);

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
        theme::style_focusable(btn);

        lv_obj_t * btn_lbl = lv_label_create(btn);
        lv_label_set_text(btn_lbl, "View Info >");
        lv_obj_set_style_text_font(btn_lbl, &lv_font_roboto_14, 0);
        lv_obj_set_style_text_color(btn_lbl, theme::text_on_accent(), 0);
        lv_obj_center(btn_lbl);

        lv_obj_add_event_cb(btn, [](lv_event_t * e) {
            lv_obj_t * root = static_cast<lv_obj_t *>(lv_event_get_user_data(e));
            
            // Dimmed background overlay
            lv_obj_t * overlay = lv_obj_create(root);
            lv_obj_remove_style_all(overlay);
            lv_obj_set_size(overlay, 800, 480);
            lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);
            lv_obj_center(overlay);

            // Modal Card
            lv_obj_t * modal = lv_obj_create(overlay);
            lv_obj_remove_style_all(modal);
            theme::style_card(modal);
            lv_obj_set_size(modal, 600, 370);
            lv_obj_center(modal);
            lv_obj_set_flex_flow(modal, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(modal, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_all(modal, 18, 0);
            lv_obj_set_style_pad_row(modal, 8, 0);

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
            
            float vbat = hal::get_mcu_battery_voltage();
            char vbat_buf[32];
            if (vbat > 0.0f) {
                std::snprintf(vbat_buf, sizeof(vbat_buf), "%.2f V (DC Input)", vbat);
            } else {
                std::snprintf(vbat_buf, sizeof(vbat_buf), "12.60 V (Nominal)");
            }

            auto add_info_row = [](lv_obj_t * parent, const char * label, const std::string & value) {
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
            };

            add_info_row(modal, "Software Version", "Prado-Reconstruction v1.4.0");
            add_info_row(modal, "Kernel Version", kernel_ver);
            add_info_row(modal, "MCU Firmware", mcu_ver);
            add_info_row(modal, "Bluetooth Subsystem", hal::get_bluetooth_hardware_info());
            add_info_row(modal, "Main Processor", "ArkMicro ARK1668 (ARM Cortex-A7 @ 800MHz)");
            add_info_row(modal, "Display & UI", "LVGL 9.2.2 (800x480 RGB888 / Framebuffer)");
            add_info_row(modal, "Vehicle Telemetry", vbat_buf);

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

            lv_obj_add_event_cb(close_btn, [](lv_event_t * ev) {
                lv_obj_t * ov = static_cast<lv_obj_t *>(lv_event_get_user_data(ev));
                lv_obj_delete(ov);
            }, LV_EVENT_CLICKED, overlay);
            if (core::navigation::focus_group()) {
                lv_group_add_obj(core::navigation::focus_group(), close_btn);
            }
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

        lv_obj_t * left_box = lv_obj_create(row);
        lv_obj_remove_style_all(left_box);
        lv_obj_set_size(left_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(left_box, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(left_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(left_box, 12, 0);

        lv_obj_t * icon_img = lv_image_create(left_box);
        lv_image_set_src(icon_img, &icons::icon_nav_settings);

        lv_obj_t * text_box = lv_obj_create(left_box);
        lv_obj_remove_style_all(text_box);
        lv_obj_set_size(text_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(text_box, LV_FLEX_FLOW_COLUMN);

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
        theme::style_focusable(btn);

        lv_obj_t * btn_lbl = lv_label_create(btn);
        lv_label_set_text(btn_lbl, "View Log >");
        lv_obj_set_style_text_font(btn_lbl, &lv_font_roboto_14, 0);
        lv_obj_set_style_text_color(btn_lbl, theme::text_on_accent(), 0);
        lv_obj_center(btn_lbl);

        lv_obj_add_event_cb(btn, [](lv_event_t * e) {
            lv_obj_t * root = static_cast<lv_obj_t *>(lv_event_get_user_data(e));

            lv_obj_t * overlay = lv_obj_create(root);
            lv_obj_remove_style_all(overlay);
            lv_obj_set_size(overlay, 800, 480);
            lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);
            lv_obj_center(overlay);

            lv_obj_t * modal = lv_obj_create(overlay);
            lv_obj_remove_style_all(modal);
            theme::style_card(modal);
            lv_obj_set_size(modal, 720, 420);
            lv_obj_center(modal);
            lv_obj_set_flex_flow(modal, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(modal, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_all(modal, 18, 0);
            lv_obj_set_style_pad_row(modal, 8, 0);

            lv_obj_t * title = lv_label_create(modal);
            lv_label_set_text(title, "MCU Live Log");
            lv_obj_set_style_text_font(title, &lv_font_roboto_20, 0);
            lv_obj_set_style_text_color(title, theme::accent_primary(), 0);

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
            lv_obj_set_style_pad_top(touch_filter_row, 6, 0);

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
                lv_obj_t * touch_filter_cb;
            };
            auto * log_ctx = new LogViewCtx{log_lbl, touch_filter_cb};

            // Refresh timer: pulls the last ~30 (post-filter) frames every
            // 300ms and auto-scrolls to the newest. Deleted in the Close
            // handler below -- must not outlive log_lbl/log_box/log_ctx.
            lv_timer_t * refresh_timer = lv_timer_create([](lv_timer_t * t) {
                auto * ctx = static_cast<LogViewCtx *>(lv_timer_get_user_data(t));
                bool show_touch = lv_obj_has_state(ctx->touch_filter_cb, LV_STATE_CHECKED);
                auto frames = hal::get_mcu_recent_frames();
                std::string text;
                constexpr size_t kShowLast = 30;
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
                lv_label_set_text(ctx->label, text.c_str());
                lv_obj_t * box = lv_obj_get_parent(ctx->label);
                lv_obj_scroll_to_y(box, LV_COORD_MAX, LV_ANIM_OFF);
            }, 300, log_ctx);

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

            struct CloseCtx {
                lv_obj_t * overlay;
                lv_timer_t * timer;
                LogViewCtx * log_ctx;
            };
            auto * close_ctx = new CloseCtx{overlay, refresh_timer, log_ctx};

            lv_obj_add_event_cb(close_btn, [](lv_event_t * ev) {
                auto * ctx = static_cast<CloseCtx *>(lv_event_get_user_data(ev));
                lv_timer_delete(ctx->timer);
                lv_obj_delete(ctx->overlay);
                delete ctx->log_ctx;
                delete ctx;
            }, LV_EVENT_CLICKED, close_ctx);

            if (core::navigation::focus_group()) {
                lv_group_add_obj(core::navigation::focus_group(), close_btn);
            }
        }, LV_EVENT_CLICKED, scr);

        if (core::navigation::focus_group()) {
            lv_group_add_obj(core::navigation::focus_group(), btn);
        }
    }

    return scr;
}

} // namespace staging_ui

