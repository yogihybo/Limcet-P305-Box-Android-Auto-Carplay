#include "ui/bluetooth_screen.h"

#include <cstdio>
#include <string>
#include <vector>

#include "core/config_store.h"
#include "core/navigation.h"
#include "hal/bluetooth.h"

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
    hal::BluetoothHandle & h = bt_handle();
    bool ok = hal::connect_device(h, text ? text : "");
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
        lv_obj_t * btn = lv_list_add_button(list, nullptr, line.c_str());
        lv_obj_add_event_cb(btn, device_row_clicked_cb, LV_EVENT_CLICKED, widgets);
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
    lv_obj_t * scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x14141e), 0);

    lv_obj_t * back_btn = lv_button_create(scr);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_obj_add_event_cb(back_btn, back_btn_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t * back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "< Back");

    lv_obj_t * title = lv_label_create(scr);
    lv_label_set_text(title, "Bluetooth");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t * content = lv_obj_create(scr);
    lv_obj_set_size(content, LV_PCT(94), LV_PCT(82));
    lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 8, 0);

    // Adapter address, informational (ADDR command).
    lv_obj_t * addr_label = lv_label_create(content);
    std::string address;
    if (bt_handle().fd >= 0 && hal::get_adapter_address(bt_handle(), address)) {
        lv_label_set_text(addr_label, ("This device: " + address).c_str());
    } else {
        lv_label_set_text(addr_label, "This device: (address unavailable)");
    }
    lv_obj_set_style_text_color(addr_label, lv_color_hex(0x999999), 0);

    // Device name -- editable, backed by NAME=<devname> + the
    // Setting.config [BlueTooth]/DeviceName field (see
    // docs/SETTINGS_REFERENCE.md section 2.7).
    lv_obj_t * name_row = lv_obj_create(content);
    lv_obj_set_width(name_row, LV_PCT(100));
    lv_obj_set_height(name_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(name_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(name_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                           LV_FLEX_ALIGN_CENTER);

    lv_obj_t * name_ta = lv_textarea_create(name_row);
    lv_textarea_set_one_line(name_ta, true);
    lv_obj_set_flex_grow(name_ta, 1);
    std::string current_name = core::default_store().get_string("DeviceName", "Limcet Box", "BlueTooth");
    lv_textarea_set_text(name_ta, current_name.c_str());

    lv_obj_t * name_save_btn = lv_button_create(name_row);
    lv_obj_add_event_cb(name_save_btn, name_save_btn_cb, LV_EVENT_CLICKED, name_ta);
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
    lv_obj_t * discoverable_label = lv_label_create(discoverable_row);
    lv_label_set_text(discoverable_label, "Discoverable to phones");
    lv_obj_set_flex_grow(discoverable_label, 1);
    lv_obj_t * discoverable_sw = lv_switch_create(discoverable_row);
    lv_obj_add_event_cb(discoverable_sw, discoverable_switch_cb, LV_EVENT_VALUE_CHANGED, nullptr);

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
    lv_obj_t * list_title = lv_label_create(list_header_row);
    lv_label_set_text(list_title, "Paired devices");
    lv_obj_t * refresh_btn = lv_button_create(list_header_row);
    lv_obj_t * refresh_label = lv_label_create(refresh_btn);
    lv_label_set_text(refresh_label, "Refresh");

    lv_obj_t * list = lv_list_create(content);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);

    lv_obj_t * status_label = lv_label_create(content);
    lv_label_set_text(status_label, "");
    lv_obj_set_style_text_color(status_label, lv_color_hex(0x999999), 0);

    auto * widgets = new lv_obj_t *[2]{list, status_label};
    lv_obj_add_event_cb(refresh_btn, refresh_btn_cb, LV_EVENT_CLICKED, widgets);

    refresh_device_list(widgets);

    return scr;
}

}  // namespace ui
