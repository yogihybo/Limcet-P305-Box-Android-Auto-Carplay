// Minimal screen stack manager. Each screen is a plain lv_obj_t*
// created via a factory function; push() creates+loads a new screen,
// pop() tears it down and reloads whatever was underneath. Deliberately
// simple for this first milestone -- no transition animations, no
// screen registry/lookup by name. Extend as real app screens (launcher,
// settings, app switcher -- see docs/IMPLEMENTATION_PLAN.md Phase 3/5)
// need more than this.
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

}  // namespace core
