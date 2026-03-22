#include "sae_overflow_screen.h"
#include "attack_select_screen.h"
#include "ui_helpers.h"
#include "uart_handler.h"
#include "bsp/m5stack_core_s3.h"
#include "esp_log.h"

static const char *TAG = "sae_overflow";

static void on_stop(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "SAE Overflow STOP");
    uart_send_command("stop");
    bsp_display_lock(0);
    show_attack_select_screen();
    bsp_display_unlock();
}

void show_sae_overflow_screen(void)
{
    send_select_networks();
    uart_send_command("sae_overflow");
    ESP_LOGI(TAG, "SAE Overflow started");

    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "SAE Overflow", NULL, NULL);

    lv_obj_t *center = lv_obj_create(scr);
    lv_obj_set_size(center, LV_PCT(80), LV_SIZE_CONTENT);
    lv_obj_center(center);
    lv_obj_set_style_bg_color(center, ui_card_color(), 0);
    lv_obj_set_style_bg_opa(center, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(center, 12, 0);
    lv_obj_set_style_border_color(center, UI_ACCENT_ORANGE, 0);
    lv_obj_set_style_border_width(center, 2, 0);
    lv_obj_set_style_pad_all(center, 16, 0);
    lv_obj_set_style_pad_row(center, 10, 0);
    lv_obj_set_flex_flow(center, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(center, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(center, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = lv_label_create(center);
    lv_label_set_text(icon, LV_SYMBOL_WARNING);
    lv_obj_set_style_text_color(icon, UI_ACCENT_ORANGE, 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, 0);

    lv_obj_t *title = lv_label_create(center);
    lv_label_set_text(title, "SAE Overflow");
    lv_obj_set_style_text_color(title, UI_ACCENT_ORANGE, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);

    lv_obj_t *status = lv_label_create(center);
    lv_label_set_text(status, "Running...");
    lv_obj_set_style_text_color(status, ui_text_color(), 0);
    lv_obj_set_style_text_font(status, &lv_font_montserrat_14, 0);

    lv_obj_t *btn = lv_btn_create(scr);
    lv_obj_set_size(btn, 140, 36);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(btn, UI_ACCENT_RED, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_add_event_cb(btn, on_stop, LV_EVENT_CLICKED, NULL);
    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, LV_SYMBOL_CLOSE " Stop");
    lv_obj_set_style_text_color(btn_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(btn_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(btn_lbl);
}
