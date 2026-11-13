// Shared modal-dialog plumbing for overlays that must sit above the nav
// rail (which lives on lv_layer_top(), so dialogs go on lv_layer_sys()).
//
// Each modal gets its OWN lv_group_t, bound to the rotary knob while it's
// open: previously dialog buttons were appended to the underlying
// screen's group, unfocused, so a knob-only user had to turn through every
// widget on the screen (still selectable behind the dialog) to reach
// "Cancel". The group is unbound and freed when the overlay is deleted.
//
// The overlay is also deleted automatically if `owner_screen` is deleted
// (navigating away, or reverse gear opening the camera overlay) -- a
// layer_sys overlay isn't a child of the screen, so nothing else would
// remove it, and it would sit over whatever screen came next.
#pragma once

#include <functional>
#include "lvgl.h"

namespace staging_ui {

// Creates a dimmed overlay on lv_layer_sys(). cover_rail=false leaves the
// nav rail visible (overlay spans only the area right of it).
lv_obj_t * open_modal(lv_obj_t * owner_screen, bool cover_rail);

// Adds `obj` to the modal's knob focus group (optionally focusing it).
void modal_add_focusable(lv_obj_t * overlay, lv_obj_t * obj, bool focus = false);

// Closes (deletes) the modal. Safe to call from a click handler of a
// widget inside it.
void close_modal(lv_obj_t * overlay);

// Two-button confirmation dialog (Cancel focused by default, so a stray
// knob press never confirms). on_confirm runs on the LVGL thread after
// the dialog has closed.
void confirm_dialog(lv_obj_t * owner_screen, const char * title, const char * body,
                    const char * confirm_label, bool destructive,
                    std::function<void()> on_confirm);

}  // namespace staging_ui
