// custom_ui entry point.
//
// Phase 0/1 milestone: LVGL v9 rendering directly to /dev/fb0 (no
// DirectFB, no GPU dependency -- see docs/ARCHITECTURE.md for why),
// with evdev touch input, and one interactive widget to prove the
// touch path works end to end. Everything past this file (screen
// manager, real app screens, HAL abstractions) is still to come --
// see docs/IMPLEMENTATION_PLAN.md.

#include <cstdio>
#include <unistd.h>
#include "lvgl.h"

static void counter_btn_event_cb(lv_event_t * e) {
    lv_obj_t * label = static_cast<lv_obj_t *>(lv_event_get_user_data(e));
    static int count = 0;
    count++;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "Touched: %d", count);
    lv_label_set_text(label, buf);
}

int main() {
    lv_init();

    lv_display_t * disp = lv_linux_fbdev_create();
    if (!disp) {
        std::fprintf(stderr, "custom_ui: lv_linux_fbdev_create() failed\n");
        return 1;
    }
    if (lv_linux_fbdev_set_file(disp, "/dev/fb0") != LV_RESULT_OK) {
        std::fprintf(stderr, "custom_ui: failed to open /dev/fb0\n");
        return 1;
    }

    // ARK1680 resistive touch controller, evdev interface -- see
    // docs/ARCHITECTURE.md's "Touch input" section.
    lv_indev_t * indev = lv_evdev_create(LV_INDEV_TYPE_POINTER, "/dev/input/event0");
    if (!indev) {
        std::fprintf(stderr, "custom_ui: warning: touch input unavailable, continuing display-only\n");
    }

    lv_obj_t * scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x14141e), 0);

    lv_obj_t * title = lv_label_create(scr);
    lv_label_set_text(title, "custom_ui -- Phase 0/1 milestone");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    lv_obj_t * label = lv_label_create(scr);
    lv_label_set_text(label, "Touched: 0");
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 40);

    lv_obj_t * btn = lv_button_create(scr);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, -20);
    lv_obj_add_event_cb(btn, counter_btn_event_cb, LV_EVENT_CLICKED, label);
    lv_obj_t * btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "Touch me");

    std::printf("custom_ui: LVGL initialized, running main loop\n");
    while (true) {
        lv_timer_handler();
        usleep(5000);
    }

    return 0;
}
