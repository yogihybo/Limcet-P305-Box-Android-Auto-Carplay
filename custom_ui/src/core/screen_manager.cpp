#include "core/screen_manager.h"

namespace core {

void ScreenManager::push(ScreenFactory factory) {
    lv_obj_t * screen = factory();
    // Slide the new screen in from the right, "deeper into the app" --
    // matches pop()'s reverse direction below. auto_del=false: the
    // outgoing screen isn't being destroyed, it's still on stack_
    // underneath the new one, waiting for a future pop().
    lv_screen_load_anim(screen, LV_SCREEN_LOAD_ANIM_MOVE_LEFT, kScreenAnimTimeMs, 0,
                         /*auto_del=*/false);
    stack_.push_back(screen);
}

void ScreenManager::replace(ScreenFactory factory) {
    if (stack_.empty()) {
        push(factory);
        return;
    }
    lv_obj_t * screen = factory();
    if (stack_.size() > 1) {
        stack_.pop_back();
    } else {
        stack_.clear();
    }
    lv_screen_load_anim(screen, LV_SCREEN_LOAD_ANIM_FADE_ON, kScreenAnimTimeMs, 0,
                         /*auto_del=*/true);
    stack_.push_back(screen);
}

void ScreenManager::pop() {
    if (stack_.size() <= 1) {
        return;  // never pop the root screen
    }
    stack_.pop_back();
    // Reverse of push()'s slide -- "back out" to the right. auto_del=true:
    // LVGL deletes the outgoing (former top) screen itself once the
    // animation finishes -- this replaces the old, unconditional
    // lv_obj_delete(top) call the pre-animation hard-cut version used;
    // calling that here too would double-free once the animation
    // completes.
    lv_screen_load_anim(stack_.back(), LV_SCREEN_LOAD_ANIM_MOVE_RIGHT, kScreenAnimTimeMs, 0,
                         /*auto_del=*/true);
}

lv_obj_t * ScreenManager::current() const {
    return stack_.empty() ? nullptr : stack_.back();
}

}  // namespace core
