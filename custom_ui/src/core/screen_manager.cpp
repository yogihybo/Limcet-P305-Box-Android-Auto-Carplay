#include "core/screen_manager.h"

namespace core {

namespace {

// 2026-09-05: deletes a screen's own dedicated lv_group_t once the
// screen object itself is torn down -- registered once per
// screen/group pair in push()/replace() below, fires whenever that
// screen is later popped or replaced (auto_del=true's own deferred
// deletion, whenever LVGL actually runs it). By that point every one
// of the screen's own member widgets has already been individually
// deleted (each of which LVGL itself detaches from its group on
// deletion, see lv_group_delete()'s own symmetric per-object cleanup),
// so this never touches a group with live members still attached --
// it just frees the now-empty group struct, avoiding a real lv_group_t
// leak on every pop()/replace().
void delete_screen_group_cb(lv_event_t * e) {
    lv_group_delete(static_cast<lv_group_t *>(lv_event_get_user_data(e)));
}

}  // namespace

void ScreenManager::push(ScreenFactory factory) {
    // 2026-09-05: see StackEntry's own comment in screen_manager.h --
    // this screen gets its OWN group, not the shared one every screen
    // used to pile onto. pending_group_ makes it visible to
    // current_group() (and therefore core::navigation::focus_group())
    // for the duration of factory()'s own synchronous call, before
    // it's pushed onto stack_ below.
    lv_group_t * group = lv_group_create();
    pending_group_ = group;
    lv_obj_t * screen = factory();
    pending_group_ = nullptr;

    lv_obj_add_event_cb(screen, delete_screen_group_cb, LV_EVENT_DELETE, group);

    // Slide the new screen in from the right, "deeper into the app" --
    // matches pop()'s reverse direction below. auto_del=false: the
    // outgoing screen isn't being destroyed, it's still on stack_
    // underneath the new one, waiting for a future pop().
    lv_screen_load_anim(screen, LV_SCREEN_LOAD_ANIM_MOVE_LEFT, kScreenAnimTimeMs, 0,
                         /*auto_del=*/false);
    stack_.push_back({screen, group});
    if (indev_) {
        lv_indev_set_group(indev_, group);
    }
    if (rebind_hook_) {
        rebind_hook_(group);
    }
}

void ScreenManager::replace(ScreenFactory factory) {
    if (stack_.empty()) {
        push(factory);
        return;
    }
    lv_group_t * group = lv_group_create();
    pending_group_ = group;
    lv_obj_t * screen = factory();
    pending_group_ = nullptr;

    lv_obj_add_event_cb(screen, delete_screen_group_cb, LV_EVENT_DELETE, group);

    if (stack_.size() > 1) {
        stack_.pop_back();
    } else {
        stack_.clear();
    }
    lv_screen_load_anim(screen, LV_SCREEN_LOAD_ANIM_FADE_ON, kScreenAnimTimeMs, 0,
                         /*auto_del=*/true);
    stack_.push_back({screen, group});
    if (indev_) {
        lv_indev_set_group(indev_, group);
    }
    if (rebind_hook_) {
        rebind_hook_(group);
    }
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
    // completes. That deletion is also what fires delete_screen_group_cb
    // above, freeing the popped screen's own group -- rebinding the
    // indev to the RESTORED screen's group here, synchronously, before
    // that deferred deletion runs, is what keeps lv_group_delete()'s
    // own "clear any indev pointing at me" step from touching the
    // group we just switched TO (see this fix's own commit message for
    // why the ordering here is safe).
    lv_screen_load_anim(stack_.back().screen, LV_SCREEN_LOAD_ANIM_MOVE_RIGHT, kScreenAnimTimeMs, 0,
                         /*auto_del=*/true);
    if (indev_) {
        lv_indev_set_group(indev_, stack_.back().group);
    }
    // See set_group_rebind_hook()'s own header comment -- this is the
    // one case where nothing else (no factory()/create_nav_rail() call)
    // would ever re-attach the shared nav-rail buttons to the screen
    // being restored here.
    if (rebind_hook_) {
        rebind_hook_(stack_.back().group);
    }
}

lv_obj_t * ScreenManager::current() const {
    return stack_.empty() ? nullptr : stack_.back().screen;
}

}  // namespace core
