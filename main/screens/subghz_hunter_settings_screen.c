#include "subghz_hunter_settings_screen.h"
#include "subghz_hunter_screen.h"
#include "subghz_rf_settings.h"
#include "ui_helpers.h"
#include "esp_log.h"

static const char *TAG = "hnt_settings";

#define UI_TOP_BAR_H  36

static subghz_rf_settings_t s_cfg;
static lv_obj_t *s_top_bar;
static lv_obj_t *s_single_sw;
static lv_obj_t *s_single_block;

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

static void update_single_visibility(void)
{
    if (!s_single_block) return;
    if (s_cfg.hunter_raw)
        lv_obj_add_flag(s_single_block, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_clear_flag(s_single_block, LV_OBJ_FLAG_HIDDEN);
}

static void persist_and_save(void)
{
    subghz_rf_settings_save(&s_cfg);
}

static void on_trigger_changed(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    s_cfg.hunter_trigger_dbm = subghz_rf_hunter_trigger_from_index(
        (int)lv_dropdown_get_selected(dd));
    persist_and_save();
}

static void on_timeout_changed(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    s_cfg.hunter_timeout_ms = subghz_rf_hunter_timeout_from_index(
        (int)lv_dropdown_get_selected(dd));
    persist_and_save();
}

static void on_mode_changed(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    s_cfg.hunter_raw = (lv_dropdown_get_selected(dd) == 1);
    if (s_cfg.hunter_raw)
        s_cfg.hunter_single = false;
    update_single_visibility();
    persist_and_save();
}

static void on_single_changed(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    s_cfg.hunter_single = lv_obj_has_state(sw, LV_STATE_CHECKED);
    persist_and_save();
}

static void on_fast_changed(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    s_cfg.hunter_fast = lv_obj_has_state(sw, LV_STATE_CHECKED);
    persist_and_save();
}

static void on_back(lv_event_t *e)
{
    (void)e;
    show_subghz_hunter_screen_resume();
}

void show_subghz_hunter_settings_screen(void)
{
    subghz_rf_settings_load(&s_cfg);

    lv_obj_t *scr = ui_screen_clear();
    s_top_bar = ui_create_top_bar(scr, "Hunter Settings", on_back, NULL);

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

    /* Trigger */
    lv_obj_t *trig_block = create_setting_block(cont);
    lv_obj_t *trig_row = create_row(trig_block);
    lv_obj_t *trig_lbl = lv_label_create(trig_row);
    lv_label_set_text(trig_lbl, "Trigger");
    lv_obj_set_style_text_color(trig_lbl, ui_text_color(), 0);
    lv_obj_set_style_text_font(trig_lbl, &lv_font_montserrat_12, 0);
    lv_obj_t *trig_dd = lv_dropdown_create(trig_row);
    lv_dropdown_set_options(trig_dd, "-85\n-80\n-75\n-70\n-65\n-60");
    lv_dropdown_set_selected(trig_dd, (uint32_t)subghz_rf_hunter_trigger_index(s_cfg.hunter_trigger_dbm));
    style_dropdown(trig_dd);
    lv_obj_add_event_cb(trig_dd, on_trigger_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(trig_dd, on_dd_raise_bar, LV_EVENT_READY, NULL);
    add_setting_desc(trig_block,
        "RSSI level a signal must exceed before it is reported.");

    /* Timeout */
    lv_obj_t *tmo_block = create_setting_block(cont);
    lv_obj_t *tmo_row = create_row(tmo_block);
    lv_obj_t *tmo_lbl = lv_label_create(tmo_row);
    lv_label_set_text(tmo_lbl, "Capture");
    lv_obj_set_style_text_color(tmo_lbl, ui_text_color(), 0);
    lv_obj_set_style_text_font(tmo_lbl, &lv_font_montserrat_12, 0);
    lv_obj_t *tmo_dd = lv_dropdown_create(tmo_row);
    lv_dropdown_set_options(tmo_dd, "1 s\n2 s\n3 s\n5 s");
    lv_dropdown_set_selected(tmo_dd, (uint32_t)subghz_rf_hunter_timeout_index(s_cfg.hunter_timeout_ms));
    style_dropdown(tmo_dd);
    lv_obj_add_event_cb(tmo_dd, on_timeout_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(tmo_dd, on_dd_raise_bar, LV_EVENT_READY, NULL);
    add_setting_desc(tmo_block,
        "How long to wait for a burst after locking onto a frequency.");

    /* Mode */
    lv_obj_t *mode_block = create_setting_block(cont);
    lv_obj_t *mode_row = create_row(mode_block);
    lv_obj_t *mode_lbl = lv_label_create(mode_row);
    lv_label_set_text(mode_lbl, "Mode");
    lv_obj_set_style_text_color(mode_lbl, ui_text_color(), 0);
    lv_obj_set_style_text_font(mode_lbl, &lv_font_montserrat_12, 0);
    lv_obj_t *mode_dd = lv_dropdown_create(mode_row);
    lv_dropdown_set_options(mode_dd, "Decode\nRaw");
    lv_dropdown_set_selected(mode_dd, s_cfg.hunter_raw ? 1 : 0);
    style_dropdown(mode_dd);
    lv_obj_add_event_cb(mode_dd, on_mode_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(mode_dd, on_dd_raise_bar, LV_EVENT_READY, NULL);
    add_setting_desc(mode_block,
        "Decode saves protocol keys; Raw saves timing edges only.");

    /* Single burst */
    s_single_block = create_setting_block(cont);
    lv_obj_t *single_row = create_row(s_single_block);
    lv_obj_t *single_lbl = lv_label_create(single_row);
    lv_label_set_text(single_lbl, "Single burst");
    lv_obj_set_style_text_color(single_lbl, ui_text_color(), 0);
    lv_obj_set_style_text_font(single_lbl, &lv_font_montserrat_12, 0);
    s_single_sw = lv_switch_create(single_row);
    if (s_cfg.hunter_single) lv_obj_add_state(s_single_sw, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(s_single_sw, ui_muted_color(), 0);
    lv_obj_set_style_bg_color(s_single_sw, UI_ACCENT_PINK, LV_STATE_CHECKED | LV_PART_INDICATOR);
    lv_obj_add_event_cb(s_single_sw, on_single_changed, LV_EVENT_VALUE_CHANGED, NULL);
    add_setting_desc(s_single_block,
        "Capture after one remote press instead of two.");
    update_single_visibility();

    /* Fast scan */
    lv_obj_t *fast_block = create_setting_block(cont);
    lv_obj_t *fast_row = create_row(fast_block);
    lv_obj_t *fast_lbl = lv_label_create(fast_row);
    lv_label_set_text(fast_lbl, "Fast scan");
    lv_obj_set_style_text_color(fast_lbl, ui_text_color(), 0);
    lv_obj_set_style_text_font(fast_lbl, &lv_font_montserrat_12, 0);
    lv_obj_t *fast_sw = lv_switch_create(fast_row);
    if (s_cfg.hunter_fast) lv_obj_add_state(fast_sw, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(fast_sw, ui_muted_color(), 0);
    lv_obj_set_style_bg_color(fast_sw, UI_ACCENT_PINK, LV_STATE_CHECKED | LV_PART_INDICATOR);
    lv_obj_add_event_cb(fast_sw, on_fast_changed, LV_EVENT_VALUE_CHANGED, NULL);
    add_setting_desc(fast_block,
        "Faster frequency steps with shorter settle and sticky re-lock.");

    ESP_LOGI(TAG, "Hunter settings screen ready");
}
