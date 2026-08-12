#include "ui/bluetooth_screen.h"

#include <cstdio>
#include <string>
#include <vector>

#include "core/config_store.h"
#include "core/navigation.h"
#include "hal/bluetooth.h"
#include "ui/status_bar.h"
#include "ui/theme.h"

namespace ui {

namespace {

// Opened once, process-lifetime -- same rationale as
// settings_screen.cpp's display_handle().
hal::BluetoothHandle & bt_handle() {
    static hal::BluetoothHandle handle;
    static bool tried = false;
    if (!tried) {
        hal::init_bluetooth(handle);  // non-fatal if /dev/bw_serial is absent
        tried = true;
    }
    return handle;
}

void back_btn_cb(lv_event_t *) {
    core::navigation::pop();
}

void status_label_set(lv_obj_t * label, const char * text) {
    lv_label_set_text(label, text);
}

// Re-issues PLIST and repopulates the device list widget with one
// button row per raw response line. See hal/bluetooth.h's top comment
// -- PLIST's exact field grammar (name/MAC/RSSI sub-fields) was never
// confirmed against real captured traffic, so each row shows the
// whole raw line, not parsed fields. Tapping a row sends
// HFPCONN=<raw line text> -- if that's not literally a bare MAC
// address once real device output is seen, this will need a real
// parser; documented as a known gap, not a guess dressed up as fact.
// LVGL button-click events don't bubble to a parent lv_list by
// default (would need LV_OBJ_FLAG_EVENT_BUBBLE on every row) -- simpler
// to register the click handler directly on each row button as it's
// created, below.
void device_row_clicked_cb(lv_event_t * e) {
    lv_obj_t * btn = static_cast<lv_obj_t *>(lv_event_get_target(e));
    auto * widgets = static_cast<lv_obj_t **>(lv_event_get_user_data(e));
    lv_obj_t * list = widgets[0];
    lv_obj_t * status_label = widgets[1];

    const char * text = lv_list_get_button_text(list, btn);
    std::string entry = text ? text : "";
    std::printf("ui::bluetooth_screen: device row tapped: '%s'\n", entry.c_str());

    // hal::list_paired_devices() now strips the "+PLIST=" prefix (see
    // hal/bluetooth.h) but PLIST's own per-entry field grammar past
    // that is still unconfirmed -- split_mac_and_name() applies the
    // same "12 hex chars + 1 separator + name" shape confirmed for a
    // real +AAPDEV= line (same vendor stack) and falls back to treating
    // the whole entry as the identifier if it doesn't match, so this
    // stays correct either way.
    std::string mac, name;
    std::string connect_id = hal::split_mac_and_name(entry, mac, name) ? mac : entry;

    hal::BluetoothHandle & h = bt_handle();
    bool ok = hal::connect_device(h, connect_id);
    status_label_set(status_label, ok ? "HFPCONN sent" : "HFPCONN failed / no response");
}

void refresh_device_list(lv_obj_t ** widgets) {
    lv_obj_t * list = widgets[0];
    lv_obj_t * status_label = widgets[1];

    lv_obj_clean(list);
    hal::BluetoothHandle & h = bt_handle();
    if (h.fd < 0) {
        lv_list_add_text(list, "/dev/bw_serial unavailable");
        status_label_set(status_label, "Bluetooth hardware not detected");
        return;
    }

    std::vector<std::string> lines;
    if (!hal::list_paired_devices(h, lines) || lines.empty()) {
        lv_list_add_text(list, "(no response / no paired devices)");
        status_label_set(status_label, "PLIST returned nothing");
        return;
    }

    for (const auto & line : lines) {
        lv_obj_t * btn = lv_list_add_button(list, LV_SYMBOL_BLUETOOTH, line.c_str());
        theme::style_list_button(btn);
        lv_obj_add_event_cb(btn, device_row_clicked_cb, LV_EVENT_CLICKED, widgets);
        lv_group_add_obj(core::navigation::focus_group(), btn);
    }
    status_label_set(status_label, "Tap a device to connect (HFP)");
}

void refresh_btn_cb(lv_event_t * e) {
    auto * widgets = static_cast<lv_obj_t **>(lv_event_get_user_data(e));
    refresh_device_list(widgets);
}

void discoverable_switch_cb(lv_event_t * e) {
    lv_obj_t * sw = static_cast<lv_obj_t *>(lv_event_get_target(e));
    bool checked = lv_obj_has_state(sw, LV_STATE_CHECKED);
    hal::set_discoverable(bt_handle(), checked);
}

void name_save_btn_cb(lv_event_t * e) {
    lv_obj_t * textarea = static_cast<lv_obj_t *>(lv_event_get_user_data(e));
    const char * name = lv_textarea_get_text(textarea);
    if (!name || name[0] == '\0') return;
    hal::set_device_name(bt_handle(), name);
    core::default_store().set_string("DeviceName", name, "BlueTooth");
    core::default_store().save();
}

}  // namespace

lv_obj_t * create_bluetooth_screen() {
    lv_obj_t * scr = nullptr;
    theme::create_screen_with_header(&scr, "Bluetooth", back_btn_cb);

    lv_obj_t * content = lv_obj_create(scr);
    theme::style_card(content);
    // The status bar (ui/status_bar.h) now sits at the literal bottom
    // of the screen, so this card needs to stop short of it instead of
    // running to the screen edge.
    lv_obj_set_size(content, LV_PCT(94), LV_PCT(68));
    lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, -(status_bar::kHeight + 6));
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    // lv_obj_create() is scrollable by default -- content's own children
    // (address/name/discoverable/list-header rows + the device list)
    // overflow its fixed LV_PCT(68) height, so without this the OUTER
    // card itself became the scroll target instead of the inner `list`
    // below, which is the one actually meant to scroll. Real hardware
    // symptom this caused: paired-device list partially hidden with
    // scrolling not doing anything useful.
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    // Tightened from 14/8 -- see the device list's own comment below:
    // this card has 5 fixed-height rows stacked above a flex_grow list
    // that needs real room to be usably scrollable, not just
    // technically scrollable.
    lv_obj_set_style_pad_all(content, 10, 0);
    lv_obj_set_style_pad_row(content, 6, 0);

    // Adapter address, informational (ADDR command).
    lv_obj_t * addr_label = lv_label_create(content);
    std::string address;
    if (bt_handle().fd >= 0 && hal::get_adapter_address(bt_handle(), address)) {
        lv_label_set_text(addr_label, ("This device: " + address).c_str());
    } else {
        lv_label_set_text(addr_label, "This device: (address unavailable)");
    }
    theme::style_secondary_text(addr_label);

    // Device name -- editable, backed by NAME=<devname> + the
    // Setting.config [BlueTooth]/DeviceName field (see
    // docs/SETTINGS_REFERENCE.md section 2.7).
    lv_obj_t * name_row = lv_obj_create(content);
    lv_obj_set_width(name_row, LV_PCT(100));
    lv_obj_set_height(name_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(name_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(name_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                           LV_FLEX_ALIGN_CENTER);
    // Room for name_save_btn's knob focus ring -- this row is
    // LV_SIZE_CONTENT and wraps tightly around its tallest child (the
    // button), leaving no slack for the ring otherwise. Same fix as
    // home_screen.cpp's tile grid.
    lv_obj_set_style_pad_ver(name_row, 6, 0);

    lv_obj_t * name_ta = lv_textarea_create(name_row);
    lv_textarea_set_one_line(name_ta, true);
    lv_obj_set_flex_grow(name_ta, 1);
    std::string current_name = core::default_store().get_string("DeviceName", "Limcet Box", "BlueTooth");
    lv_textarea_set_text(name_ta, current_name.c_str());

    lv_obj_t * name_save_btn = lv_button_create(name_row);
    theme::style_primary_button(name_save_btn);
    // style_primary_button()'s pad_ver=16 + HARD min_height=64 (a
    // style, padding alone can't shrink it below that floor) is sized
    // for a standalone CTA -- with several such rows stacked above the
    // paired-device list in this screen's fixed-height `content` card,
    // they were consuming almost all of it, squeezing the list (the
    // one actually meant to scroll) down to a ~14px sliver. Overriding
    // min_height too (not just padding/font, unlike
    // settings_screen.cpp's Bluetooth row button, which only had a
    // width problem, not a shared vertical budget to protect).
    lv_obj_set_style_pad_hor(name_save_btn, 14, 0);
    lv_obj_set_style_pad_ver(name_save_btn, 8, 0);
    lv_obj_set_style_text_font(name_save_btn, &lv_font_montserrat_14, 0);
    lv_obj_set_style_min_height(name_save_btn, 40, 0);
    lv_obj_add_event_cb(name_save_btn, name_save_btn_cb, LV_EVENT_CLICKED, name_ta);
    lv_group_add_obj(core::navigation::focus_group(), name_ta);
    lv_group_add_obj(core::navigation::focus_group(), name_save_btn);
    lv_obj_t * name_save_label = lv_label_create(name_save_btn);
    lv_label_set_text(name_save_label, "Save");

    // Discoverable toggle -- SCAN=1 (see hal/bluetooth.h: this makes
    // the head unit visible to phones; it does not make the head unit
    // actively scan for other devices, no matter what the toggle name
    // implies -- pairing is always phone-initiated on this stack).
    lv_obj_t * discoverable_row = lv_obj_create(content);
    lv_obj_set_width(discoverable_row, LV_PCT(100));
    lv_obj_set_height(discoverable_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(discoverable_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(discoverable_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                           LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(discoverable_row, 6, 0);  // focus ring clearance, see name_row above
    lv_obj_t * discoverable_label = lv_label_create(discoverable_row);
    lv_label_set_text(discoverable_label, "Discoverable to phones");
    lv_obj_set_flex_grow(discoverable_label, 1);
    lv_obj_t * discoverable_sw = lv_switch_create(discoverable_row);
    lv_obj_add_event_cb(discoverable_sw, discoverable_switch_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_group_add_obj(core::navigation::focus_group(), discoverable_sw);

    // Paired-device list + refresh. widgets[] is heap-allocated (2
    // lv_obj_t* -- list, status label) and intentionally leaked for
    // the screen's lifetime, same pattern as every other
    // process-lifetime singleton in this file -- ScreenManager::pop()
    // deletes the LVGL objects themselves but this array is a plain
    // heap block LVGL doesn't know about; freeing it on LV_EVENT_DELETE
    // would be more correct but the leak is bounded (one screen visit
    // = one small array) and this app has no shutdown path today
    // (see core/reverse_gear_watcher.cpp's destructor comment).
    lv_obj_t * list_header_row = lv_obj_create(content);
    lv_obj_set_width(list_header_row, LV_PCT(100));
    lv_obj_set_height(list_header_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(list_header_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(list_header_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                           LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(list_header_row, 6, 0);  // focus ring clearance, see name_row above
    lv_obj_t * list_title = lv_label_create(list_header_row);
    lv_label_set_text(list_title, "Paired devices");
    theme::style_section_label(list_title);
    lv_obj_t * refresh_btn = lv_button_create(list_header_row);
    theme::style_primary_button(refresh_btn);
    // Same shrink as name_save_btn above -- see its comment.
    lv_obj_set_style_pad_hor(refresh_btn, 14, 0);
    lv_obj_set_style_pad_ver(refresh_btn, 8, 0);
    lv_obj_set_style_text_font(refresh_btn, &lv_font_montserrat_14, 0);
    lv_obj_set_style_min_height(refresh_btn, 40, 0);
    lv_obj_t * refresh_label = lv_label_create(refresh_btn);
    lv_label_set_text(refresh_label, "Refresh");

    lv_obj_t * list = lv_list_create(content);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    // Room for the first/last row's knob focus ring against the
    // list's own scroll-clip boundary -- theme::style_list_button()'s
    // margin_ver only creates a gap BETWEEN rows, not at the very top/
    // bottom of the list itself. Confirmed clipped on real hardware
    // without this.
    lv_obj_set_style_pad_ver(list, 6, 0);

    lv_obj_t * status_label = lv_label_create(content);
    lv_label_set_text(status_label, "");
    theme::style_secondary_text(status_label);

    auto * widgets = new lv_obj_t *[2]{list, status_label};
    lv_obj_add_event_cb(refresh_btn, refresh_btn_cb, LV_EVENT_CLICKED, widgets);
    lv_group_add_obj(core::navigation::focus_group(), refresh_btn);

    refresh_device_list(widgets);

    return scr;
}

}  // namespace ui
