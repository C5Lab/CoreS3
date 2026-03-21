#include "ui_helpers.h"

/* ------------------------------------------------------------------ */
lv_obj_t *ui_screen_clear(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, UI_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    return scr;
}

/* ------------------------------------------------------------------ */
lv_obj_t *ui_create_top_bar(lv_obj_t *parent, const char *title,
                            lv_event_cb_t on_back, void *user_data)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, LV_PCT(100), 36);
    lv_obj_set_style_bg_color(bar, UI_BAR_COLOR, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_hor(bar, 6, 0);
    lv_obj_set_style_pad_ver(bar, 0, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    if (on_back) {
        lv_obj_t *btn = lv_btn_create(bar);
        lv_obj_set_size(btn, 60, 28);
        lv_obj_set_style_bg_color(btn, UI_ACCENT_BLUE, 0);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_set_user_data(btn, user_data);
        lv_obj_add_event_cb(btn, on_back, LV_EVENT_CLICKED, user_data);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, LV_SYMBOL_LEFT " Back");
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_center(lbl);
    }

    lv_obj_t *lbl = lv_label_create(bar);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_color(lbl, UI_TEXT_COLOR, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_flex_grow(lbl, 1);
    if (on_back)
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);

    return bar;
}

/* ------------------------------------------------------------------ */
lv_obj_t *ui_create_tile(lv_obj_t *parent, const char *label_text,
                          lv_color_t color, lv_event_cb_t on_click,
                          void *user_data)
{
    lv_obj_t *tile = lv_obj_create(parent);
    lv_obj_set_size(tile, LV_PCT(46), 42);
    lv_obj_set_style_bg_color(tile, color, 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(tile, 12, 0);
    lv_obj_set_style_border_width(tile, 0, 0);
    lv_obj_set_style_pad_all(tile, 4, 0);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_set_style_bg_color(tile, lv_color_darken(color, LV_OPA_20), LV_STATE_PRESSED);

    lv_obj_t *lbl = lv_label_create(tile);
    lv_label_set_text(lbl, label_text);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl, LV_PCT(100));
    lv_obj_center(lbl);

    if (on_click) {
        lv_obj_set_user_data(tile, user_data);
        lv_obj_add_event_cb(tile, on_click, LV_EVENT_CLICKED, user_data);
    }

    return tile;
}
