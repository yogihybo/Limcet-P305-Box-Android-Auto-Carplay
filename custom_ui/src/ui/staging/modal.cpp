#include "ui/staging/modal.h"

#include <utility>

#include "core/navigation.h"
#include "ui/staging/fonts.h"
#include "ui/staging/theme.h"

namespace staging_ui {

namespace {

struct ModalCtx {
    lv_obj_t * overlay;
    lv_obj_t * owner;  // nulled once the owner screen is being deleted
    lv_group_t * group;
};

void owner_delete_cb(lv_event_t * e);

void overlay_delete_cb(lv_event_t * e) {
    auto * ctx = static_cast<ModalCtx *>(lv_event_get_user_data(e));
    // Hand the knob back to the current screen BEFORE freeing the group
    // (lv_group_delete() would otherwise leave the indev with no group).
    // If a screen change already rebound the knob, this is a harmless
    // re-bind to that new screen's group.
    core::navigation::bind_input_group(nullptr);
    lv_group_delete(ctx->group);
    if (ctx->owner) {
        lv_obj_remove_event_cb_with_user_data(ctx->owner, owner_delete_cb, ctx);
    }
    delete ctx;
}

void owner_delete_cb(lv_event_t * e) {
    auto * ctx = static_cast<ModalCtx *>(lv_event_get_user_data(e));
    // overlay_delete_cb() must not touch the owner's event list while the
    // owner is mid-deletion; it frees ctx, so nothing here may run after.
    ctx->owner = nullptr;
    lv_obj_delete(ctx->overlay);
}

lv_obj_t * make_button(lv_obj_t * parent, const char * text, lv_color_t bg, lv_color_t fg, bool outlined) {
    lv_obj_t * btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 120, 44);
    lv_obj_set_style_radius(btn, theme::kPillRadius, 0);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    if (outlined) {
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, theme::text_secondary(), 0);
    }
    theme::style_focusable(btn);
    lv_obj_t * lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_roboto_14, 0);
    lv_obj_set_style_text_color(lbl, fg, 0);
    lv_obj_center(lbl);
    return btn;
}

struct ConfirmCtx {
    lv_obj_t * overlay;
    std::function<void()> on_confirm;
};

}  // namespace

lv_obj_t * open_modal(lv_obj_t * owner_screen, bool cover_rail) {
    lv_obj_t * overlay = lv_obj_create(lv_layer_sys());
    lv_obj_remove_style_all(overlay);
    if (cover_rail) {
        lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    } else {
        lv_obj_set_size(overlay, 800 - theme::kRailWidth, 480);
        lv_obj_set_pos(overlay, theme::kRailWidth, 0);
    }
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    // Swallow taps on the dimmed area so they don't fall through.
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);

    auto * ctx = new ModalCtx{overlay, owner_screen, lv_group_create()};
    lv_group_set_wrap(ctx->group, true);
    lv_obj_set_user_data(overlay, ctx);
    lv_obj_add_event_cb(overlay, overlay_delete_cb, LV_EVENT_DELETE, ctx);
    if (owner_screen) {
        lv_obj_add_event_cb(owner_screen, owner_delete_cb, LV_EVENT_DELETE, ctx);
    }
    core::navigation::bind_input_group(ctx->group);
    return overlay;
}

void modal_add_focusable(lv_obj_t * overlay, lv_obj_t * obj, bool focus) {
    auto * ctx = static_cast<ModalCtx *>(lv_obj_get_user_data(overlay));
    if (!ctx) return;
    lv_group_add_obj(ctx->group, obj);
    if (focus) lv_group_focus_obj(obj);
}

void close_modal(lv_obj_t * overlay) {
    lv_obj_delete(overlay);
}

void confirm_dialog(lv_obj_t * owner_screen, const char * title, const char * body,
                    const char * confirm_label, bool destructive,
                    std::function<void()> on_confirm) {
    lv_obj_t * overlay = open_modal(owner_screen, /*cover_rail=*/true);

    lv_obj_t * box = lv_obj_create(overlay);
    lv_obj_set_size(box, 400, LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, theme::surface_container_high(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_radius(box, theme::kCardRadius, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(box, 24, 0);
    lv_obj_set_style_pad_row(box, 16, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * t = lv_label_create(box);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, &lv_font_roboto_20, 0);
    lv_obj_set_style_text_color(t, theme::text_primary(), 0);

    lv_obj_t * b = lv_label_create(box);
    lv_label_set_text(b, body);
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(b, LV_PCT(100));
    lv_obj_set_style_text_font(b, &lv_font_roboto_14, 0);
    lv_obj_set_style_text_color(b, theme::text_secondary(), 0);
    lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t * row = lv_obj_create(box);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 16, 0);
    lv_obj_set_style_pad_all(row, 4, 0);  // room for the focus outline
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * cancel = make_button(row, "Cancel", theme::surface_container_high(), theme::text_primary(), true);
    lv_obj_t * confirm = make_button(row, confirm_label,
                                     destructive ? theme::danger() : theme::accent_primary(),
                                     theme::text_on_accent(), false);

    auto * ctx = new ConfirmCtx{overlay, std::move(on_confirm)};
    lv_obj_add_event_cb(box, [](lv_event_t * e) {
        delete static_cast<ConfirmCtx *>(lv_event_get_user_data(e));
    }, LV_EVENT_DELETE, ctx);

    lv_obj_add_event_cb(cancel, [](lv_event_t * e) {
        close_modal(static_cast<ConfirmCtx *>(lv_event_get_user_data(e))->overlay);
    }, LV_EVENT_CLICKED, ctx);
    lv_obj_add_event_cb(confirm, [](lv_event_t * e) {
        auto * c = static_cast<ConfirmCtx *>(lv_event_get_user_data(e));
        std::function<void()> fn = std::move(c->on_confirm);
        close_modal(c->overlay);  // frees c
        if (fn) fn();
    }, LV_EVENT_CLICKED, ctx);

    modal_add_focusable(overlay, cancel, /*focus=*/true);
    modal_add_focusable(overlay, confirm);
}

}  // namespace staging_ui
