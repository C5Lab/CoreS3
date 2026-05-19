#include "subghz_listen_settings_screen.h"
#include "subghz_listen_screen.h"
#include "subghz_rf_settings.h"
#include "ui_helpers.h"
#include "esp_log.h"

static const char *TAG = "ls_settings";

#define UI_TOP_BAR_H  36

static subghz_rf_settings_t s_cfg;
static lv_obj_t *s_top_bar;

static lv_obj_t *create_setting_block(lv_obj_t *parent)
{
    lv_obj_t *block = lv_obj_create(parent);
    lv_obj_set_size(block, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(block, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(block, 0, 0);
    lv_obj_set_style_pad_all(block, 0, 0);
    lv_obj_set_style_pad_bottom(block, 8, 0);
    lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(block, 2, 0);
    lv_obj_clear_flag(block, LV_OBJ_FLAG_SCROLLABLE);
    return block;
}

static lv_obj_t *create_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

static void add_setting_desc(lv_obj_t *block, const char *text)
{
    lv_obj_t *desc = lv_label_create(block);
    lv_label_set_text(desc, text);
    lv_obj_set_style_text_color(desc, ui_muted_color(), 0);
    lv_obj_set_style_text_font(desc, &lv_font_montserrat_10, 0);
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(desc, LV_PCT(100));
}

static void style_dropdown(lv_obj_t *dd)
{
    lv_obj_set_width(dd, 100);
    lv_obj_set_style_text_font(dd, &lv_font_montserrat_12, 0);
    lv_obj_set_style_bg_color(dd, ui_card_color(), 0);
    lv_obj_set_style_text_color(dd, ui_text_color(), 0);
    lv_obj_set_style_border_color(dd, ui_border_color(), 0);
    lv_obj_t *list = lv_dropdown_get_list(dd);
    if (list) {
        lv_obj_set_style_bg_color(list, ui_card_color(), 0);
        lv_obj_set_style_text_color(list, ui_text_color(), 0);
        lv_obj_set_style_border_color(list, ui_border_color(), 0);
        lv_obj_set_style_text_font(list, &lv_font_montserrat_12, 0);
    }
}

static void on_dd_raise_bar(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_READY && s_top_bar)
        lv_obj_move_to_index(s_top_bar, -1);
}

static void on_rssi_changed(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    s_cfg.listen_rssi_dbm = subghz_rf_listen_rssi_from_index(
        (int)lv_dropdown_get_selected(dd));
    subghz_rf_settings_save(&s_cfg);
}

static void on_back(lv_event_t *e)
{
    (void)e;
    show_subghz_listen_screen();
}

void show_subghz_listen_settings_screen(void)
{
    subghz_rf_settings_load(&s_cfg);

    lv_obj_t *scr = ui_screen_clear();
    s_top_bar = ui_create_top_bar(scr, "Listen Settings", on_back, NULL);

    lv_obj_t *cont = lv_obj_create(scr);
    lv_obj_set_size(cont, LV_PCT(92), LV_SIZE_CONTENT);
    lv_obj_align_to(cont, s_top_bar, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);
    {
        lv_display_t *disp = lv_display_get_default();
        int32_t vres = disp ? (int32_t)lv_display_get_vertical_resolution(disp) : 240;
        int32_t max_h = vres - UI_TOP_BAR_H - 40;
        if (max_h < 80) max_h = 80;
        lv_obj_set_style_max_height(cont, max_h, 0);
    }
    lv_obj_add_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(cont, ui_card_color(), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(cont, 12, 0);
    lv_obj_set_style_border_width(cont, 1, 0);
    lv_obj_set_style_border_color(cont, ui_border_color(), 0);
    lv_obj_set_style_pad_all(cont, 12, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(cont, 8, 0);

    /* RSSI sensitivity */
    lv_obj_t *rssi_block = create_setting_block(cont);
    lv_obj_t *rssi_row = create_row(rssi_block);
    lv_obj_t *rssi_lbl = lv_label_create(rssi_row);
    lv_label_set_text(rssi_lbl, "RSSI floor");
    lv_obj_set_style_text_color(rssi_lbl, ui_text_color(), 0);
    lv_obj_set_style_text_font(rssi_lbl, &lv_font_montserrat_12, 0);
    lv_obj_t *rssi_dd = lv_dropdown_create(rssi_row);
    lv_dropdown_set_options(rssi_dd, "-85\n-80\n-75\n-70\n-65\n-60");
    lv_dropdown_set_selected(rssi_dd, (uint32_t)subghz_rf_listen_rssi_index(s_cfg.listen_rssi_dbm));
    style_dropdown(rssi_dd);
    lv_obj_add_event_cb(rssi_dd, on_rssi_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(rssi_dd, on_dd_raise_bar, LV_EVENT_READY, NULL);
    add_setting_desc(rssi_block,
        "Receiver noise gate; bursts below this RSSI are dropped before decode.");

    ESP_LOGI(TAG, "Listen settings screen ready");
}
