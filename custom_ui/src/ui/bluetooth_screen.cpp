#include "ui/bluetooth_screen.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/config_store.h"
#include "core/log_timing.h"
#include "core/navigation.h"
#include "core/sized_thread.h"
#include "hal/androidauto_client.h"
#include "hal/bluetooth.h"
#include "ui/android_auto_screen.h"
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
    lv_obj_t * addr_label = nullptr;
    lv_obj_t * conn_status_label = nullptr;
    lv_obj_t * list = nullptr;
    lv_obj_t * status_label = nullptr;
    lv_obj_t * refresh_btn = nullptr;
    lv_obj_t * spinner = nullptr;
    lv_timer_t * poll_timer = nullptr;
    std::vector<std::string> last_devices;
};

// Forward-declared -- defined below, but remove_device_clicked_cb()
// (added ahead of it in this file) needs to call it.
void start_bt_load(BtScreenWidgets * w);

void status_label_set(lv_obj_t * label, const char * text) {
    if (label) lv_label_set_text(label, text);
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

// 2026-09-04: real hardware bug -- these three click/switch handlers
// used to call hal::connect_device()/hal::remove_paired_device()/
// hal::set_discoverable() (all dbus-send/popen()-based, see
// bluetooth.cpp) directly, inline, on the LVGL thread with no
// application-level timeout at all -- if the system bus hangs, the
// WHOLE UI freezes indefinitely, not just this screen. Same bug class
// as mcu_input.cpp's reader-thread freeze (see that file's header
// comment) -- fixed the same way this file's own bt_load_worker()
// already handles its (also blocking) BlueZ calls: a detached
// core::SizedThread, fire-and-forget where nothing downstream needs to
// wait for completion.
void device_row_clicked_cb(lv_event_t * e) {
    lv_obj_t * btn = static_cast<lv_obj_t *>(lv_event_get_target(e));
    auto * w = static_cast<BtScreenWidgets *>(lv_event_get_user_data(e));

    auto index = reinterpret_cast<uintptr_t>(lv_obj_get_user_data(btn));
    if (index >= w->last_devices.size()) return;
    const std::string & entry = w->last_devices[index];

    std::string mac, name;
    std::string connect_id = hal::split_plist_entry(entry, mac, name) ? mac : entry;
    std::printf("%s [UI-BT] User clicked CONNECT for device: '%s' (MAC=%s, Name='%s')\n",
                core::log_timestamp().c_str(), entry.c_str(), mac.c_str(), name.c_str());

    status_label_set(w->status_label, "Connecting & Starting Android Auto...");

    std::printf("%s [UI-BT] Triggering Android Auto sidecar requestConnect() & switching screen\n",
                core::log_timestamp().c_str());
    core::SizedThread(core::kDefaultThreadStackSize, [connect_id]() {
        hal::BluetoothHandle & h = hal::shared_handle();
        hal::connect_device(h, connect_id);
        hal::AndroidAutoClient client;
        client.requestConnect();
    }).detach();
    // Navigates immediately -- android_auto_screen.cpp's own
    // poll_background_loop() picks up the real connection state as
    // soon as it lands, same eventual-consistency model as everywhere
    // else this bug class was fixed.
    core::navigation::push(ui::create_android_auto_screen);
}

void remove_device_clicked_cb(lv_event_t * e) {
    lv_obj_t * btn = static_cast<lv_obj_t *>(lv_event_get_target(e));
    auto * w = static_cast<BtScreenWidgets *>(lv_event_get_user_data(e));

    auto index = reinterpret_cast<uintptr_t>(lv_obj_get_user_data(btn));
    if (index >= w->last_devices.size()) return;
    const std::string & entry = w->last_devices[index];

    std::string mac, name;
    std::string connect_id = hal::split_plist_entry(entry, mac, name) ? mac : entry;
    std::printf("%s [UI-BT] User clicked DISCONNECT/REMOVE for device: '%s' (MAC=%s)\n",
                core::log_timestamp().c_str(), entry.c_str(), connect_id.c_str());

    status_label_set(w->status_label, "Removing device...");
    // start_bt_load() itself touches LVGL widgets, so it can only run
    // on the LVGL thread -- kick it off via the SAME atomic-ready +
    // lv_timer poll pattern bt_load_poll_cb() already uses, rather
    // than calling it directly from the background thread below.
    auto * ready = new std::atomic<bool>(false);
    auto * poll_ctx = new std::pair<BtScreenWidgets *, std::atomic<bool> *>(w, ready);
    if (w->poll_timer) {
        lv_timer_delete(w->poll_timer);
    }
    w->poll_timer = lv_timer_create(
        [](lv_timer_t * timer) {
            auto * ctx = static_cast<std::pair<BtScreenWidgets *, std::atomic<bool> *> *>(
                lv_timer_get_user_data(timer));
            if (!ctx->second->load(std::memory_order_acquire)) return;
            BtScreenWidgets * w2 = ctx->first;
            w2->poll_timer = nullptr;
            lv_timer_delete(timer);
            delete ctx->second;
            delete ctx;
            start_bt_load(w2);
        },
        100, poll_ctx);
    core::SizedThread(core::kDefaultThreadStackSize, [connect_id, ready]() {
        hal::BluetoothHandle & h = hal::shared_handle();
        hal::remove_paired_device(h, connect_id);
        ready->store(true, std::memory_order_release);
    }).detach();
}

void disconnect_device_clicked_cb(lv_event_t * e) {
    auto * w = static_cast<BtScreenWidgets *>(lv_event_get_user_data(e));
    status_label_set(w->status_label, "Disconnecting...");

    auto * ready = new std::atomic<bool>(false);
    auto * poll_ctx = new std::pair<BtScreenWidgets *, std::atomic<bool> *>(w, ready);
    if (w->poll_timer) {
        lv_timer_delete(w->poll_timer);
    }
    w->poll_timer = lv_timer_create(
        [](lv_timer_t * timer) {
            auto * ctx = static_cast<std::pair<BtScreenWidgets *, std::atomic<bool> *> *>(
                lv_timer_get_user_data(timer));
            if (!ctx->second->load(std::memory_order_acquire)) return;
            BtScreenWidgets * w2 = ctx->first;
            w2->poll_timer = nullptr;
            lv_timer_delete(timer);
            delete ctx->second;
            delete ctx;
            start_bt_load(w2);
        },
        100, poll_ctx);
    core::SizedThread(core::kDefaultThreadStackSize, [ready]() {
        hal::BluetoothHandle & h = hal::shared_handle();
        hal::disconnect_device(h);
        ready->store(true, std::memory_order_release);
    }).detach();
}

void populate_device_list(BtScreenWidgets * w, bool hw_present, bool devices_ok,
                           const std::vector<std::string> & devices) {
    lv_obj_clean(w->list);
    w->last_devices.clear();
    if (!hw_present) {
        lv_obj_t * lbl = lv_label_create(w->list);
        lv_label_set_text(lbl, "Bluetooth hardware not detected");
        lv_obj_set_style_text_color(lbl, staging_ui::theme::text_secondary(), 0);
        status_label_set(w->status_label, "Bluetooth unavailable");
        return;
    }
    std::vector<std::string> valid_devices;
    for (const auto & dev : devices) {
        std::string mac, name;
        if (hal::split_plist_entry(dev, mac, name) && !mac.empty()) {
            valid_devices.push_back(dev);
        }
    }
    w->last_devices = valid_devices;
    if (!devices_ok || valid_devices.empty()) {
        lv_obj_t * lbl = lv_label_create(w->list);
        lv_label_set_text(lbl, "(no paired devices)");
        lv_obj_set_style_text_color(lbl, staging_ui::theme::text_secondary(), 0);
        status_label_set(w->status_label, "Ready to pair new device");
        return;
    }

    std::string connected_mac = hal::get_connected_device_mac();

    for (size_t i = 0; i < valid_devices.size(); ++i) {
        std::string mac, name;
        std::string label = hal::split_plist_entry(valid_devices[i], mac, name) && !name.empty()
                                 ? name
                                 : valid_devices[i];
        bool is_connected = !connected_mac.empty() && (connected_mac == mac);

        lv_obj_t * row = lv_obj_create(w->list);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, 48);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(row, 8, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        // Left Container (Icon + Device Name)
        lv_obj_t * left_box = lv_obj_create(row);
        lv_obj_remove_style_all(left_box);
        lv_obj_set_flex_grow(left_box, 1);
        lv_obj_set_height(left_box, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(left_box, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(left_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(left_box, 8, 0);
        lv_obj_clear_flag(left_box, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t * icon = ui::icons::create_icon(left_box, &ui::icons::icon_smartphone,
            is_connected ? staging_ui::theme::accent_primary() : staging_ui::theme::text_secondary());
        (void)icon;

        lv_obj_t * dev_name = lv_label_create(left_box);
        lv_label_set_text(dev_name, label.c_str());
        lv_obj_set_style_text_font(dev_name, &lv_font_roboto_14, 0);
        lv_obj_set_style_text_color(dev_name, staging_ui::theme::text_primary(), 0);
        lv_obj_set_flex_grow(dev_name, 1);
        lv_label_set_long_mode(dev_name, LV_LABEL_LONG_DOT);

        // Right Controls Container (Connect/Disconnect + Remove)
        lv_obj_t * right_box = lv_obj_create(row);
        lv_obj_remove_style_all(right_box);
        lv_obj_set_size(right_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(right_box, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(right_box, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(right_box, 8, 0);
        lv_obj_clear_flag(right_box, LV_OBJ_FLAG_SCROLLABLE);

        // Contextual Primary Action: Disconnect if connected, Connect if disconnected
        if (is_connected) {
            lv_obj_t * disc_btn = lv_button_create(right_box);
            lv_obj_remove_style_all(disc_btn);
            lv_obj_set_size(disc_btn, 82, 34);
            lv_obj_set_style_radius(disc_btn, staging_ui::theme::kPillRadius, 0);
            lv_obj_set_style_bg_color(disc_btn, staging_ui::theme::surface_container_high(), 0);
            lv_obj_set_style_bg_opa(disc_btn, LV_OPA_COVER, 0);
            staging_ui::theme::style_focusable(disc_btn);
            lv_obj_add_event_cb(disc_btn, disconnect_device_clicked_cb, LV_EVENT_CLICKED, w);

            lv_obj_t * disc_lbl = lv_label_create(disc_btn);
            lv_label_set_text(disc_lbl, "Disconnect");
            lv_obj_set_style_text_font(disc_lbl, &lv_font_roboto_14, 0);
            lv_obj_set_style_text_color(disc_lbl, lv_color_hex(0xf28b82), 0);
            lv_obj_center(disc_lbl);

            if (core::navigation::focus_group()) {
                lv_group_add_obj(core::navigation::focus_group(), disc_btn);
            }
        } else {
            lv_obj_t * conn_btn = lv_button_create(right_box);
            lv_obj_remove_style_all(conn_btn);
            lv_obj_set_size(conn_btn, 72, 34);
            lv_obj_set_style_radius(conn_btn, staging_ui::theme::kPillRadius, 0);
            lv_obj_set_style_bg_color(conn_btn, staging_ui::theme::accent_primary(), 0);
            lv_obj_set_style_bg_opa(conn_btn, LV_OPA_COVER, 0);
            staging_ui::theme::style_focusable(conn_btn);
            lv_obj_set_user_data(conn_btn, reinterpret_cast<void *>(static_cast<uintptr_t>(i)));
            lv_obj_add_event_cb(conn_btn, device_row_clicked_cb, LV_EVENT_CLICKED, w);

            lv_obj_t * conn_lbl = lv_label_create(conn_btn);
            lv_label_set_text(conn_lbl, "Connect");
            lv_obj_set_style_text_font(conn_lbl, &lv_font_roboto_14, 0);
            lv_obj_set_style_text_color(conn_lbl, staging_ui::theme::text_on_accent(), 0);
            lv_obj_center(conn_lbl);

            if (core::navigation::focus_group()) {
                lv_group_add_obj(core::navigation::focus_group(), conn_btn);
            }
        }

        // Compact circular Unpair/Remove Button (34x34)
        lv_obj_t * rem_btn = lv_button_create(right_box);
        lv_obj_remove_style_all(rem_btn);
        lv_obj_set_size(rem_btn, 34, 34);
        lv_obj_set_style_radius(rem_btn, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(rem_btn, staging_ui::theme::surface_container_high(), 0);
        lv_obj_set_style_bg_opa(rem_btn, LV_OPA_COVER, 0);
        staging_ui::theme::style_focusable(rem_btn);
        lv_obj_set_user_data(rem_btn, reinterpret_cast<void *>(static_cast<uintptr_t>(i)));
        lv_obj_add_event_cb(rem_btn, remove_device_clicked_cb, LV_EVENT_CLICKED, w);

        lv_obj_t * rem_icon = lv_label_create(rem_btn);
        lv_label_set_text(rem_icon, LV_SYMBOL_TRASH);
        lv_obj_set_style_text_font(rem_icon, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(rem_icon, staging_ui::theme::text_secondary(), 0);
        lv_obj_center(rem_icon);

        if (core::navigation::focus_group()) {
            lv_group_add_obj(core::navigation::focus_group(), rem_btn);
        }
    }
    status_label_set(w->status_label, "Select device to connect or unpair");
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

    if (w->addr_label) {
        if (address_ok && !address.empty()) {
            lv_label_set_text(w->addr_label, address.c_str());
        } else {
            lv_label_set_text(w->addr_label, "Not available");
        }
    }

    if (w->conn_status_label) {
        auto telem = hal::get_telemetry();
        if (hw_present && telem.connected) {
            std::string name = telem.connected_device_name;
            if (name.empty()) name = hal::get_connected_device_mac();
            if (!name.empty()) {
                lv_label_set_text(w->conn_status_label, ("Connected: " + name).c_str());
            } else {
                lv_label_set_text(w->conn_status_label, "Connected");
            }
            lv_obj_set_style_text_color(w->conn_status_label, staging_ui::theme::accent_primary(), 0);
        } else {
            lv_label_set_text(w->conn_status_label, "Disconnected");
            lv_obj_set_style_text_color(w->conn_status_label, staging_ui::theme::text_secondary(), 0);
        }
    }

    w->poll_timer = nullptr;
    populate_device_list(w, hw_present, devices_ok, devices);
    w->spinner = nullptr;
    lv_obj_clear_state(w->refresh_btn, LV_STATE_DISABLED);

    lv_timer_delete(timer);
    delete state;
    delete ctx;
}

void start_bt_load(BtScreenWidgets * w) {
    if (w->poll_timer) {
        lv_timer_delete(w->poll_timer);
        w->poll_timer = nullptr;
    }
    lv_obj_clean(w->list);
    w->spinner = lv_spinner_create(w->list);
    lv_obj_set_size(w->spinner, 40, 40);
    lv_obj_center(w->spinner);
    status_label_set(w->status_label, "Loading paired devices...");
    lv_obj_add_state(w->refresh_btn, LV_STATE_DISABLED);

    auto * state = new BtLoadState();
    auto * ctx = new std::pair<BtScreenWidgets *, BtLoadState *>(w, state);
    w->poll_timer = lv_timer_create(bt_load_poll_cb, 100, ctx);

    core::SizedThread(core::kDefaultThreadStackSize, bt_load_worker, state).detach();
}

void refresh_btn_cb(lv_event_t * e) {
    auto * w = static_cast<BtScreenWidgets *>(lv_event_get_user_data(e));
    start_bt_load(w);
}

void discoverable_switch_cb(lv_event_t * e) {
    lv_obj_t * sw = static_cast<lv_obj_t *>(lv_event_get_target(e));
    bool checked = lv_obj_has_state(sw, LV_STATE_CHECKED);
    // 2026-09-04: async -- see device_row_clicked_cb()'s own comment.
    // Fire-and-forget: the switch's own visual state already reflects
    // the user's intent immediately, nothing needs to wait for this.
    core::SizedThread(core::kDefaultThreadStackSize, [checked]() {
        hal::set_discoverable(hal::shared_handle(), checked);
    }).detach();
}

void widgets_delete_cb(lv_event_t * e) {
    auto * w = static_cast<BtScreenWidgets *>(lv_event_get_user_data(e));
    if (w) {
        if (w->poll_timer) {
            lv_timer_delete(w->poll_timer);
            w->poll_timer = nullptr;
        }
        delete w;
    }
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

    // Bluetooth Address
    lv_obj_t * addr_box = lv_obj_create(card_info);
    lv_obj_remove_style_all(addr_box);
    lv_obj_set_size(addr_box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(addr_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(addr_box, 4, 0);

    lv_obj_t * addr_lbl = lv_label_create(addr_box);
    lv_label_set_text(addr_lbl, "Bluetooth Address");
    lv_obj_set_style_text_font(addr_lbl, &lv_font_roboto_14, 0);
    lv_obj_set_style_text_color(addr_lbl, staging_ui::theme::text_secondary(), 0);

    lv_obj_t * addr_label = lv_label_create(addr_box);
    lv_label_set_text(addr_label, "--:--:--:--:--:--");
    lv_obj_set_style_text_font(addr_label, &lv_font_roboto_20, 0);
    lv_obj_set_style_text_color(addr_label, staging_ui::theme::text_primary(), 0);

    // Connection Status
    lv_obj_t * conn_box = lv_obj_create(card_info);
    lv_obj_remove_style_all(conn_box);
    lv_obj_set_size(conn_box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(conn_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(conn_box, 4, 0);

    lv_obj_t * conn_lbl = lv_label_create(conn_box);
    lv_label_set_text(conn_lbl, "Connection Status");
    lv_obj_set_style_text_font(conn_lbl, &lv_font_roboto_14, 0);
    lv_obj_set_style_text_color(conn_lbl, staging_ui::theme::text_secondary(), 0);

    lv_obj_t * conn_status_label = lv_label_create(conn_box);
    lv_label_set_text(conn_status_label, "Checking...");
    lv_obj_set_style_text_font(conn_status_label, &lv_font_roboto_20, 0);
    lv_obj_set_style_text_color(conn_status_label, staging_ui::theme::text_secondary(), 0);

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

    auto * widgets = new BtScreenWidgets();
    widgets->addr_label = addr_label;
    widgets->conn_status_label = conn_status_label;
    widgets->list = list;
    widgets->status_label = status_label;
    widgets->refresh_btn = refresh_btn;
    lv_obj_add_event_cb(scr, widgets_delete_cb, LV_EVENT_DELETE, widgets);
    lv_obj_add_event_cb(refresh_btn, refresh_btn_cb, LV_EVENT_CLICKED, widgets);

    if (core::navigation::focus_group()) {
        lv_group_add_obj(core::navigation::focus_group(), refresh_btn);
    }

    start_bt_load(widgets);

    return scr;
}

}  // namespace ui
