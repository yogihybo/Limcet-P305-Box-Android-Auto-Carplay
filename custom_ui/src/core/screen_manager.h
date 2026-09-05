// Minimal screen stack manager. Each screen is a plain lv_obj_t*
// created via a factory function; push() creates+loads a new screen,
// pop() tears it down and reloads whatever was underneath. No screen
// registry/lookup by name. Extend as real app screens (launcher,
// settings, app switcher -- see docs/IMPLEMENTATION_PLAN.md Phase 3/5)
// need more than this.
//
// push()/pop() use lv_screen_load_anim() (a short slide, see the .cpp)
// rather than lv_screen_load()'s hard cut -- a real Android Auto-style
// launcher never jump-cuts between screens, and on a fixed 800x480
// automotive panel a directionless flash between two solid-dark
// screens reads as a glitch, not a polished transition. Applied
// uniformly through this one chokepoint (every screen pushes/pops
// through here, see core/navigation.h), including
// reverse_camera_screen.cpp's transparent hardware-video overlay --
// that screen's own top comment explains it's relying on the LCDC's
// multi-layer compositor to show real video underneath our (mostly)
// transparent LVGL layer; animating a slide-in on top of that layer is
// UNVERIFIED and could conceivably look wrong (a brief visible
// composite glitch while our layer is sliding rather than already
// fully in place) -- not something this dev environment can check
// (no real framebuffer/hardware here). Flagged as a real, open gap for
// the hardware test pass, not silently assumed fine.
#pragma once

#include <vector>
#include "lvgl.h"

namespace core {

class ScreenManager {
public:
    using ScreenFactory = lv_obj_t * (*)();

    // 2026-09-05: real hardware bug found via code review -- every
    // screen used to add its own focusable widgets to ONE process-wide
    // lv_group_t (core::navigation::focus_group()). push() never
    // deletes the screen underneath (auto_del=false, kept alive for a
    // future pop()), and LVGL only ever removes an object from its
    // group when that OBJECT is deleted -- so a screen reached via
    // push() left every widget from every screen still further down
    // the stack registered in the SAME group, invisibly. Rotating the
    // knob on the new screen could focus (and pressing it could
    // activate) a widget belonging to a screen the user can't even
    // see. Fixed by giving each screen its OWN dedicated lv_group_t
    // (see StackEntry below), created on push()/replace() and rebound
    // onto this indev immediately; must be called once, before the
    // first push(), so that first real screen gets a real binding.
    void set_indev(lv_indev_t * indev) { indev_ = indev; }

    // Creates the screen via factory, loads it, and pushes it onto the
    // stack. The very first push() becomes the root screen -- pop()
    // will never remove it.
    void push(ScreenFactory factory);

    // Replaces the current top screen with a new one.
    void replace(ScreenFactory factory);

    // Deletes the current screen and loads whatever's underneath.
    // No-op if only the root screen remains.
    void pop();

    lv_obj_t * current() const;
    size_t depth() const { return stack_.size(); }

    // 2026-09-05: the lv_group_t belonging to whichever screen is
    // CURRENTLY on top -- or, while a factory() call is synchronously
    // in progress (push()/replace() below), the NEW screen's
    // not-yet-stacked group, so that screen's own widget-registration
    // calls (core::navigation::focus_group(), called from inside
    // factory()) target the right group from their very first call.
    // core::navigation::focus_group() delegates to this once a
    // ScreenManager exists -- see that function's own comment.
    lv_group_t * current_group() const {
        if (pending_group_) return pending_group_;
        return stack_.empty() ? nullptr : stack_.back().group;
    }

private:
    struct StackEntry {
        lv_obj_t * screen;
        lv_group_t * group;
    };
    std::vector<StackEntry> stack_;
    lv_indev_t * indev_ = nullptr;
    lv_group_t * pending_group_ = nullptr;
};

// Shared transition timing -- one place to retune both directions at
// once, see screen_manager.cpp.
constexpr uint32_t kScreenAnimTimeMs = 200;

}  // namespace core
