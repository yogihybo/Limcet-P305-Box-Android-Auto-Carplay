#include "core/navigation.h"

#include <cstdio>

#include "core/log_timing.h"

namespace core::navigation {

namespace {
ScreenManager * g_manager = nullptr;
}

void init(ScreenManager & manager) {
    g_manager = &manager;
}

void push(ScreenManager::ScreenFactory factory) {
    if (!g_manager) {
        std::fprintf(stderr, "%s core::navigation::push: called before init()\n", core::log_timestamp().c_str());
        return;
    }
    g_manager->push(factory);
}

void replace(ScreenManager::ScreenFactory factory) {
    if (!g_manager) {
        std::fprintf(stderr, "%s core::navigation::replace: called before init()\n", core::log_timestamp().c_str());
        return;
    }
    g_manager->replace(factory);
}

void pop() {
    if (!g_manager) {
        std::fprintf(stderr, "%s core::navigation::pop: called before init()\n", core::log_timestamp().c_str());
        return;
    }
    g_manager->pop();
}

size_t depth() {
    if (!g_manager) {
        return 0;
    }
    return g_manager->depth();
}

lv_group_t * focus_group() {
    // 2026-09-05: see this function's own header comment -- delegates
    // to ScreenManager, which owns a real per-screen group now.
    if (!g_manager) {
        std::fprintf(stderr, "%s core::navigation::focus_group: called before init()\n", core::log_timestamp().c_str());
        // Should never actually be reached in practice (main.cpp no
        // longer calls this before init()/the first push()) -- kept
        // only so a genuine misuse degrades to "a widget that doesn't
        // participate in rotary focus" rather than a null-pointer
        // dereference in whatever calls lv_group_add_obj() on the
        // result.
        static lv_group_t * g_fallback = lv_group_create();
        return g_fallback;
    }
    return g_manager->current_group();
}

}  // namespace core::navigation
