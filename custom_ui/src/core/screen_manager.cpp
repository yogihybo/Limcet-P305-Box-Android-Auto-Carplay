#include "core/screen_manager.h"

namespace core {

void ScreenManager::push(ScreenFactory factory) {
    lv_obj_t * screen = factory();
    lv_screen_load(screen);
    stack_.push_back(screen);
}

void ScreenManager::pop() {
    if (stack_.size() <= 1) {
        return;  // never pop the root screen
    }
    lv_obj_t * top = stack_.back();
    stack_.pop_back();
    lv_screen_load(stack_.back());
    lv_obj_delete(top);
}

lv_obj_t * ScreenManager::current() const {
    return stack_.empty() ? nullptr : stack_.back();
}

}  // namespace core
