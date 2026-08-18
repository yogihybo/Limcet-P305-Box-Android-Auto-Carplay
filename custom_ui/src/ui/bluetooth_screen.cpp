#include "ui/bluetooth_screen.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/config_store.h"
#include "core/log_timing.h"
#include "core/navigation.h"
#include "hal/bluetooth.h"
#include "ui/staging/nav_rail.h"
#include "ui/staging/theme.h"
#include "ui/staging/fonts.h"
#include "ui/staging/icons.h"

namespace ui {

namespace {

// Backing state for one background load (address + paired-device
// list). Heap-allocated, one per load attempt (screen creation, or
// each Refresh tap) -- see BtLoadWorker's own comment for why this
// project's usual "leak it, no shutdown path" convention applies here
// too rather than trying to cancel/join the thread.
struct BtLoadState {
    std::mutex mtx;
    std::atomic<bool> ready{false};
    bool hw_present = false;
    bool address_ok = false;
    std::string address;
    bool devices_ok = false;
    std::vector<std::string> devices;
};

struct BtScreenWidgets {
    lv_obj_t * addr_label;
    lv_obj_t * list;
    lv_obj_t * status_label;
    lv_obj_t * refresh_btn;
    lv_obj_t * spinner;
    std::vector<std::string> last_devices;
};

// Forward-declared -- defined below, but remove_device_clicked_cb()
// (added ahead of it in this file) needs to call it.
void start_bt_load(BtScreenWidgets * w);

void status_label_set(lv_obj_t * label, const char * text) {
    lv_label_set_text(label, text);
}

void bt_load_worker(BtLoadState * state) {
    hal::BluetoothHandle & h = hal::shared_handle();
    bool hw_present = h.fd >= 0;

    std::string address;
    bool address_ok = hw_present && hal::get_adapter_address(h, address);

    std::vector<std::string> devices;
    bool devices_ok = hw_present && hal::list_paired_devices(h, devices);

    {
        std::lock_guard<std::mutex> lock(state->mtx);
        state->hw_present = hw_present;
        state->address_ok = address_ok;
        state->address = std::move(address);
        state->devices_ok = devices_ok;
        state->devices = std::move(devices);
    }
    state->ready.store(true, std::memory_order_release);
}

void device_row_clicked_cb(lv_event_t * e) {
    lv_obj_t * btn = static_cast<lv_obj_t *>(lv_event_get_target(e));
    auto * w = static_cast<BtScreenWidgets *>(lv_event_get_user_data(e));

    auto index = reinterpret_cast<uintptr_t>(lv_obj_get_user_data(btn));
    if (index >= w->last_devices.size()) return;
    const std::string & entry = w->last_devices[index];
    std::printf("%s ui::bluetooth_screen: device row tapped: '%s'\n", core::log_timestamp().c_str(), entry.c_str());

    std::string mac, name;
    std::string connect_id = hal::split_plist_entry(entry, mac, name) ? mac : entry;

    hal::BluetoothHandle & h = hal::shared_handle();
    bool ok = hal::connect_device(h, connect_id);
    status_label_set(w->status_label, ok ? "HFPCONN sent" : "HFPCONN failed / no response");
}

void remove_device_clicked_cb(lv_event_t * e) {
    lv_obj_t * btn = static_cast<lv_obj_t *>(lv_event_get_target(e));
    auto * w = static_cast<BtScreenWidgets *>(lv_event_get_user_data(e));

    auto index = reinterpret_cast<uintptr_t>(lv_obj_get_user_data(btn));
    if (index >= w->last_devices.size()) return;
    const std::string & entry = w->last_devices[index];
    std::printf("%s ui::bluetooth_screen: remove device tapped: '%s'\n", core::log_timestamp().c_str(), entry.c_str());

    hal::BluetoothHandle & h = hal::shared_handle();
    hal::disconnect_device(h);

    // "Disconnected", not "Removed" -- see hal::disconnect_device()'s
    // own comment: no AT command exists to actually unpair/forget a
    // device, only to disconnect the currently active link. The device
    // stays paired and will still show up in start_bt_load()'s next
    // PLIST refresh below.
    status_label_set(w->status_label, "Disconnected");
    start_bt_load(w);
}

void populate_device_list(BtScreenWidgets * w, bool hw_present, bool devices_ok,
                           const std::vector<std::string> & devices) {
    lv_obj_clean(w->list);
    w->last_devices = devices;
    if (!hw_present) {
        lv_obj_t * lbl = lv_label_create(w->list);
        lv_label_set_text(lbl, "/dev/bw_serial unavailable");
        lv_obj_set_style_text_color(lbl, staging_ui::theme::text_secondary(), 0);
        status_label_set(w->status_label, "Bluetooth hardware not detected");
        return;
    }
    if (!devices_ok || devices.empty()) {
        lv_obj_t * lbl = lv_label_create(w->list);
        lv_label_set_text(lbl, "(no paired devices)");
        lv_obj_set_style_text_color(lbl, staging_ui::theme::text_secondary(), 0);
        status_label_set(w->status_label, "PLIST returned nothing");
        return;
    }
    for (size_t i = 0; i < devices.size(); ++i) {
        std::string mac, name;
        std::string label = hal::split_plist_entry(devices[i], mac, name) && !name.empty()
                                 ? name
                                 : devices[i];

        lv_obj_t * row = lv_obj_create(w->list);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, 48);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(row, 8, 0);

        lv_obj_t * left_box = lv_obj_create(row);
        lv_obj_remove_style_all(left_box);
        lv_obj_set_size(left_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(left_box, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(left_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(left_box, 10, 0);

        lv_obj_t * icon = ui::icons::create_icon(left_box, &ui::icons::icon_smartphone, staging_ui::theme::accent_primary());
        (void)icon;

        lv_obj_t * dev_name = lv_label_create(left_box);
        lv_label_set_text(dev_name, label.c_str());
        lv_obj_set_style_text_font(dev_name, &lv_font_roboto_20, 0);
        lv_obj_set_style_text_color(dev_name, staging_ui::theme::text_primary(), 0);

        // Right Controls Container (Connect + Remove)
        lv_obj_t * right_box = lv_obj_create(row);
        lv_obj_remove_style_all(right_box);
        lv_obj_set_size(right_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(right_box, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(right_box, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(right_box, 8, 0);

        // Connect Button
        lv_obj_t * btn = lv_button_create(right_box);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, 76, 34);
        lv_obj_set_style_radius(btn, staging_ui::theme::kPillRadius, 0);
        lv_obj_set_style_bg_color(btn, staging_ui::theme::accent_primary(), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        staging_ui::theme::style_focusable(btn);
        lv_obj_set_user_data(btn, reinterpret_cast<void *>(static_cast<uintptr_t>(i)));
        lv_obj_add_event_cb(btn, device_row_clicked_cb, LV_EVENT_CLICKED, w);

        lv_obj_t * btn_lbl = lv_label_create(btn);
        lv_label_set_text(btn_lbl, "Connect");
        lv_obj_set_style_text_font(btn_lbl, &lv_font_roboto_14, 0);
        lv_obj_set_style_text_color(btn_lbl, staging_ui::theme::text_on_accent(), 0);
        lv_obj_center(btn_lbl);

        // Remove Button
        lv_obj_t * rem_btn = lv_button_create(right_box);
        lv_obj_remove_style_all(rem_btn);
        lv_obj_set_size(rem_btn, 68, 34);
        lv_obj_set_style_radius(rem_btn, staging_ui::theme::kPillRadius, 0);
        lv_obj_set_style_bg_color(rem_btn, staging_ui::theme::surface_container_high(), 0);
        lv_obj_set_style_bg_opa(rem_btn, LV_OPA_COVER, 0);
        staging_ui::theme::style_focusable(rem_btn);
        lv_obj_set_user_data(rem_btn, reinterpret_cast<void *>(static_cast<uintptr_t>(i)));
        lv_obj_add_event_cb(rem_btn, remove_device_clicked_cb, LV_EVENT_CLICKED, w);

        lv_obj_t * rem_lbl = lv_label_create(rem_btn);
        // "Disconnect", not "Remove" -- see hal::disconnect_device()'s
        // own comment: no AT command exists to actually unpair/forget
        // a device on this hardware, only to disconnect the active
        // link. A button labeled "Remove" that doesn't remove anything
        // would be a real, user-visible lie about what it does.
        lv_label_set_text(rem_lbl, "Disconnect");
        lv_obj_set_style_text_font(rem_lbl, &lv_font_roboto_14, 0);
        lv_obj_set_style_text_color(rem_lbl, lv_color_hex(0xf28b82), 0);
        lv_obj_center(rem_lbl);

        if (core::navigation::focus_group()) {
            lv_group_add_obj(core::navigation::focus_group(), btn);
            lv_group_add_obj(core::navigation::focus_group(), rem_btn);
        }
    }
    status_label_set(w->status_label, "Tap Connect to link, Remove to unpair");
}

void bt_load_poll_cb(lv_timer_t * timer) {
    auto * ctx = static_cast<std::pair<BtScreenWidgets *, BtLoadState *> *>(
        lv_timer_get_user_data(timer));
    BtScreenWidgets * w = ctx->first;
    BtLoadState * state = ctx->second;

    if (!state->ready.load(std::memory_order_acquire)) {
        return;
    }

    bool hw_present, address_ok, devices_ok;
    std::string address;
    std::vector<std::string> devices;
    {
        std::lock_guard<std::mutex> lock(state->mtx);
        hw_present = state->hw_present;
        address_ok = state->address_ok;
        address = state->address;
        devices_ok = state->devices_ok;
        devices = state->devices;
    }

    if (address_ok) {
        lv_label_set_text(w->addr_label, ("PIN: 0000  (" + address + ")").c_str());
    } else {
        lv_label_set_text(w->addr_label, "PIN: 0000");
    }

    populate_device_list(w, hw_present, devices_ok, devices);
    w->spinner = nullptr;
    lv_obj_clear_state(w->refresh_btn, LV_STATE_DISABLED);

    lv_timer_pause(timer);
    delete state;
    delete ctx;
}

void start_bt_load(BtScreenWidgets * w) {
    lv_obj_clean(w->list);
    if (w->spinner) {
        lv_obj_delete(w->spinner);
    }
    w->spinner = lv_spinner_create(w->list);
    lv_obj_set_size(w->spinner, 40, 40);
    lv_obj_center(w->spinner);
    status_label_set(w->status_label, "Loading paired devices...");
    lv_obj_add_state(w->refresh_btn, LV_STATE_DISABLED);

    auto * state = new BtLoadState();
    auto * ctx = new std::pair<BtScreenWidgets *, BtLoadState *>(w, state);
    lv_timer_t * timer = lv_timer_create(bt_load_poll_cb, 100, ctx);

    std::thread(bt_load_worker, state).detach();

    lv_obj_add_event_cb(lv_obj_get_screen(w->list), [](lv_event_t * e) {
        lv_timer_delete(static_cast<lv_timer_t *>(lv_event_get_user_data(e)));
    }, LV_EVENT_DELETE, timer);
}

void refresh_btn_cb(lv_event_t * e) {
    auto * w = static_cast<BtScreenWidgets *>(lv_event_get_user_data(e));
    start_bt_load(w);
}

void discoverable_switch_cb(lv_event_t * e) {
    lv_obj_t * sw = static_cast<lv_obj_t *>(lv_event_get_target(e));
    bool checked = lv_obj_has_state(sw, LV_STATE_CHECKED);
    hal::set_discoverable(hal::shared_handle(), checked);
}

void widgets_delete_cb(lv_event_t * e) {
    delete static_cast<BtScreenWidgets *>(lv_event_get_user_data(e));
}

}  // namespace

lv_obj_t * create_bluetooth_screen() {
    lv_obj_t * scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, staging_ui::theme::bg(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // 1. Persistent 5-Icon Navigation Rail (Bluetooth active)
    staging_ui::create_nav_rail(scr, staging_ui::NavDestination::Bluetooth);

    // 2. Main Content Area matching Home Dashboard geometry
    lv_obj_t * content = lv_obj_create(scr);
    lv_obj_remove_style_all(content);
    lv_obj_set_pos(content, staging_ui::theme::kRailWidth + 16, 8);
    lv_obj_set_size(content, 800 - (staging_ui::theme::kRailWidth + 32), 464);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 8, 0);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    // Top Header Row (Centered Title)
    lv_obj_t * header_row = lv_obj_create(content);
    lv_obj_remove_style_all(header_row);
    lv_obj_set_width(header_row, LV_PCT(100));
    lv_obj_set_height(header_row, 34);
    lv_obj_set_flex_flow(header_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * title_lbl = lv_label_create(header_row);
    lv_label_set_text(title_lbl, "Bluetooth");
    lv_obj_set_style_text_font(title_lbl, &lv_font_roboto_28, 0);
    lv_obj_set_style_text_color(title_lbl, staging_ui::theme::text_primary(), 0);

    // Dual-Card Container (Side by Side)
    lv_obj_t * cards_row = lv_obj_create(content);
    lv_obj_remove_style_all(cards_row);
    lv_obj_set_width(cards_row, LV_PCT(100));
    lv_obj_set_flex_grow(cards_row, 1);
    lv_obj_set_flex_flow(cards_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cards_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(cards_row, 16, 0);
    lv_obj_clear_flag(cards_row, LV_OBJ_FLAG_SCROLLABLE);

    // Left Card: Device Info (336px wide x 416px high)
    lv_obj_t * card_info = lv_obj_create(cards_row);
    staging_ui::theme::style_card(card_info);
    lv_obj_set_width(card_info, 336);
    lv_obj_set_height(card_info, LV_PCT(100));
    lv_obj_set_flex_flow(card_info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card_info, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(card_info, 24, 0);

    lv_obj_t * info_header = lv_label_create(card_info);
    lv_label_set_text(info_header, "Local Bluetooth");
    lv_obj_set_style_text_font(info_header, &lv_font_roboto_28, 0);
    lv_obj_set_style_text_color(info_header, staging_ui::theme::text_primary(), 0);

    // Device Name
    lv_obj_t * name_box = lv_obj_create(card_info);
    lv_obj_remove_style_all(name_box);
    lv_obj_set_size(name_box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(name_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(name_box, 4, 0);

    lv_obj_t * name_lbl = lv_label_create(name_box);
    lv_label_set_text(name_lbl, "Device Name");
    lv_obj_set_style_text_font(name_lbl, &lv_font_roboto_14, 0);
    lv_obj_set_style_text_color(name_lbl, staging_ui::theme::text_secondary(), 0);

    std::string current_name = core::default_store().get_string("DeviceName", "Prado CustomUI", "BlueTooth");
    lv_obj_t * name_val = lv_label_create(name_box);
    lv_label_set_text(name_val, current_name.c_str());
    lv_obj_set_style_text_font(name_val, &lv_font_roboto_20, 0);
    lv_obj_set_style_text_color(name_val, staging_ui::theme::text_primary(), 0);

    // PIN / Address
    lv_obj_t * pin_box = lv_obj_create(card_info);
    lv_obj_remove_style_all(pin_box);
    lv_obj_set_size(pin_box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(pin_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(pin_box, 4, 0);

    lv_obj_t * pin_lbl = lv_label_create(pin_box);
    lv_label_set_text(pin_lbl, "PIN");
    lv_obj_set_style_text_font(pin_lbl, &lv_font_roboto_14, 0);
    lv_obj_set_style_text_color(pin_lbl, staging_ui::theme::text_secondary(), 0);

    lv_obj_t * addr_label = lv_label_create(pin_box);
    lv_label_set_text(addr_label, "0000");
    lv_obj_set_style_text_font(addr_label, &lv_font_roboto_20, 0);
    lv_obj_set_style_text_color(addr_label, staging_ui::theme::text_primary(), 0);

    // Discoverable Switch
    lv_obj_t * disc_row = lv_obj_create(card_info);
    lv_obj_remove_style_all(disc_row);
    lv_obj_set_width(disc_row, LV_PCT(100));
    lv_obj_set_height(disc_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(disc_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(disc_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * disc_lbl = lv_label_create(disc_row);
    lv_label_set_text(disc_lbl, "Discoverable");
    lv_obj_set_style_text_font(disc_lbl, &lv_font_roboto_20, 0);
    lv_obj_set_style_text_color(disc_lbl, staging_ui::theme::text_primary(), 0);

    lv_obj_t * discoverable_sw = lv_switch_create(disc_row);
    lv_obj_add_event_cb(discoverable_sw, discoverable_switch_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    if (core::navigation::focus_group()) {
        lv_group_add_obj(core::navigation::focus_group(), discoverable_sw);
    }

    // Right Card: Paired Devices List (336px wide x 416px high)
    lv_obj_t * card_devices = lv_obj_create(cards_row);
    staging_ui::theme::style_card(card_devices);
    lv_obj_set_width(card_devices, 336);
    lv_obj_set_height(card_devices, LV_PCT(100));
    lv_obj_set_flex_flow(card_devices, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card_devices, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(card_devices, 24, 0);
    lv_obj_set_style_pad_row(card_devices, 12, 0);

    lv_obj_t * dev_header_row = lv_obj_create(card_devices);
    lv_obj_remove_style_all(dev_header_row);
    lv_obj_set_width(dev_header_row, LV_PCT(100));
    lv_obj_set_height(dev_header_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(dev_header_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dev_header_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * dev_title = lv_label_create(dev_header_row);
    lv_label_set_text(dev_title, "Paired Devices");
    lv_obj_set_style_text_font(dev_title, &lv_font_roboto_28, 0);
    lv_obj_set_style_text_color(dev_title, staging_ui::theme::text_primary(), 0);

    lv_obj_t * refresh_btn = lv_button_create(dev_header_row);
    lv_obj_remove_style_all(refresh_btn);
    lv_obj_set_size(refresh_btn, 80, 32);
    lv_obj_set_style_radius(refresh_btn, staging_ui::theme::kPillRadius, 0);
    lv_obj_set_style_bg_color(refresh_btn, staging_ui::theme::surface_container_high(), 0);
    lv_obj_set_style_bg_opa(refresh_btn, LV_OPA_COVER, 0);
    staging_ui::theme::style_focusable(refresh_btn);

    lv_obj_t * refresh_lbl = lv_label_create(refresh_btn);
    lv_label_set_text(refresh_lbl, "Refresh");
    lv_obj_set_style_text_font(refresh_lbl, &lv_font_roboto_14, 0);
    lv_obj_set_style_text_color(refresh_lbl, staging_ui::theme::accent_primary(), 0);
    lv_obj_center(refresh_lbl);

    lv_obj_t * list = lv_obj_create(card_devices);
    lv_obj_remove_style_all(list);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 8, 0);

    lv_obj_t * status_label = lv_label_create(card_devices);
    lv_label_set_text(status_label, "");
    lv_obj_set_style_text_font(status_label, &lv_font_roboto_14, 0);
    lv_obj_set_style_text_color(status_label, staging_ui::theme::text_secondary(), 0);

    auto * widgets = new BtScreenWidgets{addr_label, list, status_label, refresh_btn, nullptr, {}};
    lv_obj_add_event_cb(scr, widgets_delete_cb, LV_EVENT_DELETE, widgets);
    lv_obj_add_event_cb(refresh_btn, refresh_btn_cb, LV_EVENT_CLICKED, widgets);

    if (core::navigation::focus_group()) {
        lv_group_add_obj(core::navigation::focus_group(), refresh_btn);
    }

    start_bt_load(widgets);

    return scr;
}

}  // namespace ui
