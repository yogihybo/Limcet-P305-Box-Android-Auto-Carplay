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

    // Creates the screen via factory, loads it, and pushes it onto the
    // stack. The very first push() becomes the root screen -- pop()
    // will never remove it.
    void push(ScreenFactory factory);

    // Deletes the current screen and loads whatever's underneath.
    // No-op if only the root screen remains.
    void pop();

    lv_obj_t * current() const;
    size_t depth() const { return stack_.size(); }

private:
    std::vector<lv_obj_t *> stack_;
};

// Shared transition timing -- one place to retune both directions at
// once, see screen_manager.cpp.
constexpr uint32_t kScreenAnimTimeMs = 200;

}  // namespace core
