#include "core/navigation.h"

#include <cstdio>

namespace core::navigation {

namespace {
ScreenManager * g_manager = nullptr;
lv_group_t * g_focus_group = nullptr;
}

void init(ScreenManager & manager) {
    g_manager = &manager;
}

void push(ScreenManager::ScreenFactory factory) {
    if (!g_manager) {
        std::fprintf(stderr, "core::navigation::push: called before init()\n");
        return;
    }
    g_manager->push(factory);
}

void pop() {
    if (!g_manager) {
        std::fprintf(stderr, "core::navigation::pop: called before init()\n");
        return;
    }
    g_manager->pop();
}

lv_group_t * focus_group() {
    if (!g_focus_group) {
        g_focus_group = lv_group_create();
    }
    return g_focus_group;
}

}  // namespace core::navigation
