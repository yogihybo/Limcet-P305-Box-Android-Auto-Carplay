#include "ui/bluetooth_screen.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/config_store.h"
#include "core/navigation.h"
#include "hal/bluetooth.h"
#include "ui/status_bar.h"
#include "ui/theme.h"

namespace ui {

namespace {

void back_btn_cb(lv_event_t *) {
    core::navigation::pop();
}

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
    // Shown over `list` while a load is in flight -- lv_obj_delete()'d
    // (and reset to nullptr) once that load's poll timer sees
    // BtLoadState::ready. Re-created fresh for each load rather than
    // reused, simplest way to avoid tracking a "currently visible"
    // flag across repeated Refresh taps.
    lv_obj_t * spinner;
};

void status_label_set(lv_obj_t * label, const char * text) {
    lv_label_set_text(label, text);
}

// 2026-08-12: runs hal::get_adapter_address() + hal::list_paired_devices()
// off the LVGL main thread -- both go through hal::send_command(),
// which even after fixing its always-blocks-the-full-timeout bug can
// still take a real, nonzero round-trip. Running them synchronously
// inside create_bluetooth_screen() (the previous design) meant the
// screen couldn't even render its first frame until both finished --
// this project's LVGL main loop only gets to flush/draw between
// lv_timer_handler() calls, so any synchronous work in screen creation
// delays the very first frame, not just the data. `state` is a raw
// pointer, not shared_ptr -- the thread is detached and simply keeps
// writing into its own private, heap-allocated BtLoadState even if the
// screen (and its poll timer) has already been torn down by the time
// it finishes; nothing else ever touches that same BtLoadState, so
// there's no use-after-free risk, just a small bounded leak matching
// this file's own established "widgets[] intentionally leaked, no
// shutdown path" convention (see the old comment this replaced).
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
    // release: everything written above must be visible to whichever
    // thread observes ready==true next (the poll timer, on the LVGL
    // main thread, which pairs this with an acquire load).
    state->ready.store(true, std::memory_order_release);
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
    auto * w = static_cast<BtScreenWidgets *>(lv_event_get_user_data(e));

    const char * text = lv_list_get_button_text(w->list, btn);
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

    hal::BluetoothHandle & h = hal::shared_handle();
    bool ok = hal::connect_device(h, connect_id);
    status_label_set(w->status_label, ok ? "HFPCONN sent" : "HFPCONN failed / no response");
}

// Populates `list` from a finished load's results -- shared by the
// initial-load poll callback and the Refresh button's poll callback,
// so the two can't drift out of sync with each other.
void populate_device_list(BtScreenWidgets * w, bool hw_present, bool devices_ok,
                           const std::vector<std::string> & devices) {
    lv_obj_clean(w->list);
    if (!hw_present) {
        lv_list_add_text(w->list, "/dev/bw_serial unavailable");
        status_label_set(w->status_label, "Bluetooth hardware not detected");
        return;
    }
    if (!devices_ok || devices.empty()) {
        lv_list_add_text(w->list, "(no response / no paired devices)");
        status_label_set(w->status_label, "PLIST returned nothing");
        return;
    }
    for (const auto & line : devices) {
        lv_obj_t * btn = lv_list_add_button(w->list, LV_SYMBOL_BLUETOOTH, line.c_str());
        theme::style_list_button(btn);
        lv_obj_add_event_cb(btn, device_row_clicked_cb, LV_EVENT_CLICKED, w);
        lv_group_add_obj(core::navigation::focus_group(), btn);
    }
    status_label_set(w->status_label, "Tap a device to connect (HFP)");
}

// Polls one BtLoadState until ready, then applies its results to the
// screen and stops itself (lv_timer_pause(), not lv_timer_delete() --
// this file's screen_delete_cb is the single authority for actually
// deleting timers, so a load that finishes normally and a screen that
// gets closed mid-load can never race to double-free the same timer).
void bt_load_poll_cb(lv_timer_t * timer) {
    auto * ctx = static_cast<std::pair<BtScreenWidgets *, BtLoadState *> *>(
        lv_timer_get_user_data(timer));
    BtScreenWidgets * w = ctx->first;
    BtLoadState * state = ctx->second;

    // acquire: pairs with bt_load_worker()'s release store -- makes
    // every field written under state->mtx visible here once true.
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
        lv_label_set_text(w->addr_label, ("This device: " + address).c_str());
    } else {
        lv_label_set_text(w->addr_label, "This device: (address unavailable)");
    }

    // populate_device_list() starts with lv_obj_clean(w->list), which
    // already deletes every child of `list` -- including `spinner`
    // (created as a child of `list` in start_bt_load()). Just drop the
    // now-dangling pointer here; calling lv_obj_delete() on it again
    // was a use-after-free (lv_obj_invalidate() on already-freed
    // memory) -- this is what crashed on real hardware right after
    // opening this screen.
    populate_device_list(w, hw_present, devices_ok, devices);
    w->spinner = nullptr;
    lv_obj_clear_state(w->refresh_btn, LV_STATE_DISABLED);

    lv_timer_pause(timer);
    delete state;   // safe: nothing else reads it once ready has been consumed here
    delete ctx;
}

// Spawns a fresh background load (see bt_load_worker()) and a poll
// timer to pick up its result -- used both for the screen's initial
// load and every Refresh tap, so they can't drift into two different
// code paths. Shows a spinner over `list` and disables the Refresh
// button for the duration, so a second tap can't overlap a load
// already in flight (simpler than tracking/cancelling concurrent
// BtLoadStates).
void start_bt_load(BtScreenWidgets * w) {
    lv_obj_clean(w->list);
    if (w->spinner) {
        lv_obj_delete(w->spinner);
    }
    w->spinner = lv_spinner_create(w->list);
    lv_obj_set_size(w->spinner, 48, 48);
    lv_obj_center(w->spinner);
    status_label_set(w->status_label, "Loading...");
    lv_obj_add_state(w->refresh_btn, LV_STATE_DISABLED);

    auto * state = new BtLoadState();
    auto * ctx = new std::pair<BtScreenWidgets *, BtLoadState *>(w, state);
    lv_timer_t * timer = lv_timer_create(bt_load_poll_cb, 100, ctx);

    std::thread(bt_load_worker, state).detach();

    // Not stored on `w` -- deleted directly via its own LV_EVENT_DELETE
    // hook on the screen, same pattern status_bar.cpp's screen_delete_cb
    // uses, rather than this struct owning a timer pointer that would
    // need updating on every start_bt_load() call (Refresh can trigger
    // this more than once per screen visit). lv_obj_get_screen(), not
    // lv_screen_active() -- this can run from the Refresh button's
    // click handler, at which point this screen is still the active
    // one, but relying on "whatever's currently active" instead of
    // walking up from a widget we KNOW belongs to this screen is
    // fragile for no reason.
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

void name_save_btn_cb(lv_event_t * e) {
    lv_obj_t * textarea = static_cast<lv_obj_t *>(lv_event_get_user_data(e));
    const char * name = lv_textarea_get_text(textarea);
    if (!name || name[0] == '\0') return;
    hal::set_device_name(hal::shared_handle(), name);
    core::default_store().set_string("DeviceName", name, "BlueTooth");
    core::default_store().save();
}

void widgets_delete_cb(lv_event_t * e) {
    delete static_cast<BtScreenWidgets *>(lv_event_get_user_data(e));
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

    // Adapter address, informational (ADDR command). Populated
    // asynchronously by start_bt_load() below -- see that function's
    // comment for why this screen no longer blocks on the ADDR/PLIST
    // round trips before rendering its first frame.
    lv_obj_t * addr_label = lv_label_create(content);
    lv_label_set_text(addr_label, "This device: (looking up address...)");
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
    // Fallback matches etc/default_settings.conf's own DeviceName --
    // deliberately not stock's "Limcet Box", see that file's comment.
    std::string current_name = core::default_store().get_string("DeviceName", "Prado CustomUI", "BlueTooth");
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

    // Heap-allocated, freed via LV_EVENT_DELETE on the screen (see
    // widgets_delete_cb) -- unlike the old plain lv_obj_t*[2] this
    // replaces, this one is properly cleaned up rather than
    // intentionally leaked, since it's referenced by every load's poll
    // timer for the screen's whole lifetime, not just at creation.
    auto * widgets = new BtScreenWidgets{addr_label, list, status_label, refresh_btn, nullptr};
    lv_obj_add_event_cb(scr, widgets_delete_cb, LV_EVENT_DELETE, widgets);
    lv_obj_add_event_cb(refresh_btn, refresh_btn_cb, LV_EVENT_CLICKED, widgets);
    lv_group_add_obj(core::navigation::focus_group(), refresh_btn);

    start_bt_load(widgets);

    return scr;
}

}  // namespace ui
