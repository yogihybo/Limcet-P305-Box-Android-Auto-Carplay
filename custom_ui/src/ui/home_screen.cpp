#include "ui/home_screen.h"

#include <cstdio>

namespace ui {

namespace {

void counter_btn_event_cb(lv_event_t * e) {
    lv_obj_t * label = static_cast<lv_obj_t *>(lv_event_get_user_data(e));
    static int count = 0;
    count++;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "Touched: %d", count);
    lv_label_set_text(label, buf);
}

}  // namespace

lv_obj_t * create_home_screen() {
    lv_obj_t * scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x14141e), 0);

    lv_obj_t * title = lv_label_create(scr);
    lv_label_set_text(title, "custom_ui -- Phase 1 milestone");
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

    return scr;
}

}  // namespace ui
