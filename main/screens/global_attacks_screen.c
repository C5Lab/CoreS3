#include "global_attacks_screen.h"
#include "home_screen.h"
#include "handshaker_screen.h"
#include "portal_screen.h"
#include "wardrive_screen.h"
#include "ui_helpers.h"
#include "uart_handler.h"
#include "bsp/m5stack_core_s3.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "global_atk";

/* ================================================================== */
/*  Blackout                                                           */
/* ================================================================== */

static lv_obj_t *blackout_overlay = NULL;

static void close_blackout_overlay(void)
{
    if (blackout_overlay) {
        lv_obj_del(blackout_overlay);
        blackout_overlay = NULL;
    }
}

static void blackout_stop_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Blackout STOP");
    uart_send_command("stop");
    bsp_display_lock(0);
    close_blackout_overlay();
    bsp_display_unlock();
}

static void show_blackout_active(void)
{
    close_blackout_overlay();

    uart_send_command("start_blackout");
    ESP_LOGI(TAG, "Blackout started");

    lv_obj_t *scr = lv_scr_act();
    blackout_overlay = lv_obj_create(scr);
    lv_obj_set_size(blackout_overlay, 280, 180);
    lv_obj_center(blackout_overlay);
    lv_obj_set_style_bg_color(blackout_overlay, ui_card_color(), 0);
    lv_obj_set_style_bg_opa(blackout_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(blackout_overlay, 12, 0);
    lv_obj_set_style_border_color(blackout_overlay, UI_ACCENT_RED, 0);
    lv_obj_set_style_border_width(blackout_overlay, 2, 0);
    lv_obj_set_style_pad_all(blackout_overlay, 14, 0);
    lv_obj_set_flex_flow(blackout_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(blackout_overlay, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(blackout_overlay, 10, 0);
    lv_obj_clear_flag(blackout_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = lv_label_create(blackout_overlay);
    lv_label_set_text(icon, LV_SYMBOL_WARNING);
    lv_obj_set_style_text_color(icon, UI_ACCENT_RED, 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, 0);

    lv_obj_t *title = lv_label_create(blackout_overlay);
    lv_label_set_text(title, "BLACKOUT ACTIVE");
    lv_obj_set_style_text_color(title, UI_ACCENT_RED, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);

    lv_obj_t *desc = lv_label_create(blackout_overlay);
    lv_label_set_text(desc, "Deauthing all networks...");
    lv_obj_set_style_text_color(desc, ui_text_color(), 0);
    lv_obj_set_style_text_font(desc, &lv_font_montserrat_12, 0);

    lv_obj_t *btn = lv_btn_create(blackout_overlay);
    lv_obj_set_size(btn, 120, 34);
    lv_obj_set_style_bg_color(btn, UI_ACCENT_RED, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_add_event_cb(btn, blackout_stop_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, LV_SYMBOL_CLOSE " Stop");
    lv_obj_set_style_text_color(btn_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(btn_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(btn_lbl);
}

static void blackout_yes_cb(lv_event_t *e)
{
    (void)e;
    show_blackout_active();
}

static void blackout_no_cb(lv_event_t *e)
{
    (void)e;
    close_blackout_overlay();
}

static void show_blackout_confirm(void)
{
    lv_obj_t *scr = lv_scr_act();
    blackout_overlay = lv_obj_create(scr);
    lv_obj_set_size(blackout_overlay, 280, 180);
    lv_obj_center(blackout_overlay);
    lv_obj_set_style_bg_color(blackout_overlay, ui_card_color(), 0);
    lv_obj_set_style_bg_opa(blackout_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(blackout_overlay, 12, 0);
    lv_obj_set_style_border_color(blackout_overlay, UI_ACCENT_RED, 0);
    lv_obj_set_style_border_width(blackout_overlay, 2, 0);
    lv_obj_set_style_pad_all(blackout_overlay, 14, 0);
    lv_obj_set_flex_flow(blackout_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(blackout_overlay, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(blackout_overlay, 8, 0);
    lv_obj_clear_flag(blackout_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = lv_label_create(blackout_overlay);
    lv_label_set_text(icon, LV_SYMBOL_WARNING);
    lv_obj_set_style_text_color(icon, UI_ACCENT_RED, 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, 0);

    lv_obj_t *title = lv_label_create(blackout_overlay);
    lv_label_set_text(title, "BLACKOUT");
    lv_obj_set_style_text_color(title, UI_ACCENT_RED, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);

    lv_obj_t *desc = lv_label_create(blackout_overlay);
    lv_label_set_text(desc, "Deauth all networks\naround you?");
    lv_obj_set_style_text_color(desc, ui_text_color(), 0);
    lv_obj_set_style_text_font(desc, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(desc, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *btn_row = lv_obj_create(blackout_overlay);
    lv_obj_set_size(btn_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *no_btn = lv_btn_create(btn_row);
    lv_obj_set_size(no_btn, 100, 32);
    lv_obj_set_style_bg_color(no_btn, ui_muted_color(), 0);
    lv_obj_set_style_radius(no_btn, 8, 0);
    lv_obj_add_event_cb(no_btn, blackout_no_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *no_lbl = lv_label_create(no_btn);
    lv_label_set_text(no_lbl, "No");
    lv_obj_set_style_text_color(no_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(no_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(no_lbl);

    lv_obj_t *yes_btn = lv_btn_create(btn_row);
    lv_obj_set_size(yes_btn, 100, 32);
    lv_obj_set_style_bg_color(yes_btn, UI_ACCENT_RED, 0);
    lv_obj_set_style_radius(yes_btn, 8, 0);
    lv_obj_add_event_cb(yes_btn, blackout_yes_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *yes_lbl = lv_label_create(yes_btn);
    lv_label_set_text(yes_lbl, "Yes");
    lv_obj_set_style_text_color(yes_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(yes_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(yes_lbl);
}

/* ================================================================== */
/*  Sniffer Dog                                                        */
/* ================================================================== */

static lv_obj_t *snifferdog_overlay = NULL;

static void close_snifferdog_overlay(void)
{
    if (snifferdog_overlay) {
        lv_obj_del(snifferdog_overlay);
        snifferdog_overlay = NULL;
    }
}

static void snifferdog_stop_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Sniffer Dog STOP");
    uart_send_command("stop");
    bsp_display_lock(0);
    close_snifferdog_overlay();
    bsp_display_unlock();
}

static void show_snifferdog_active(void)
{
    close_snifferdog_overlay();

    uart_send_command("start_sniffer_dog");
    ESP_LOGI(TAG, "Sniffer Dog started");

    lv_obj_t *scr = lv_scr_act();
    snifferdog_overlay = lv_obj_create(scr);
    lv_obj_set_size(snifferdog_overlay, 280, 180);
    lv_obj_center(snifferdog_overlay);
    lv_obj_set_style_bg_color(snifferdog_overlay, ui_card_color(), 0);
    lv_obj_set_style_bg_opa(snifferdog_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(snifferdog_overlay, 12, 0);
    lv_obj_set_style_border_color(snifferdog_overlay, UI_ACCENT_CYAN, 0);
    lv_obj_set_style_border_width(snifferdog_overlay, 2, 0);
    lv_obj_set_style_pad_all(snifferdog_overlay, 14, 0);
    lv_obj_set_flex_flow(snifferdog_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(snifferdog_overlay, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(snifferdog_overlay, 10, 0);
    lv_obj_clear_flag(snifferdog_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = lv_label_create(snifferdog_overlay);
    lv_label_set_text(icon, LV_SYMBOL_EYE_OPEN);
    lv_obj_set_style_text_color(icon, UI_ACCENT_CYAN, 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, 0);

    lv_obj_t *title = lv_label_create(snifferdog_overlay);
    lv_label_set_text(title, "SNIFFER DOG ACTIVE");
    lv_obj_set_style_text_color(title, UI_ACCENT_CYAN, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);

    lv_obj_t *desc = lv_label_create(snifferdog_overlay);
    lv_label_set_text(desc, "Deauthing all clients...");
    lv_obj_set_style_text_color(desc, ui_text_color(), 0);
    lv_obj_set_style_text_font(desc, &lv_font_montserrat_12, 0);

    lv_obj_t *btn = lv_btn_create(snifferdog_overlay);
    lv_obj_set_size(btn, 120, 34);
    lv_obj_set_style_bg_color(btn, UI_ACCENT_RED, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_add_event_cb(btn, snifferdog_stop_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, LV_SYMBOL_CLOSE " Stop");
    lv_obj_set_style_text_color(btn_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(btn_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(btn_lbl);
}

static void snifferdog_yes_cb(lv_event_t *e)
{
    (void)e;
    show_snifferdog_active();
}

static void snifferdog_no_cb(lv_event_t *e)
{
    (void)e;
    close_snifferdog_overlay();
}

static void show_snifferdog_confirm(void)
{
    lv_obj_t *scr = lv_scr_act();
    snifferdog_overlay = lv_obj_create(scr);
    lv_obj_set_size(snifferdog_overlay, 280, 180);
    lv_obj_center(snifferdog_overlay);
    lv_obj_set_style_bg_color(snifferdog_overlay, ui_card_color(), 0);
    lv_obj_set_style_bg_opa(snifferdog_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(snifferdog_overlay, 12, 0);
    lv_obj_set_style_border_color(snifferdog_overlay, UI_ACCENT_CYAN, 0);
    lv_obj_set_style_border_width(snifferdog_overlay, 2, 0);
    lv_obj_set_style_pad_all(snifferdog_overlay, 14, 0);
    lv_obj_set_flex_flow(snifferdog_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(snifferdog_overlay, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(snifferdog_overlay, 8, 0);
    lv_obj_clear_flag(snifferdog_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = lv_label_create(snifferdog_overlay);
    lv_label_set_text(icon, LV_SYMBOL_EYE_OPEN);
    lv_obj_set_style_text_color(icon, UI_ACCENT_CYAN, 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, 0);

    lv_obj_t *title = lv_label_create(snifferdog_overlay);
    lv_label_set_text(title, "SNIFFER DOG");
    lv_obj_set_style_text_color(title, UI_ACCENT_CYAN, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);

    lv_obj_t *desc = lv_label_create(snifferdog_overlay);
    lv_label_set_text(desc, "Deauth all clients\naround you?");
    lv_obj_set_style_text_color(desc, ui_text_color(), 0);
    lv_obj_set_style_text_font(desc, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(desc, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *btn_row = lv_obj_create(snifferdog_overlay);
    lv_obj_set_size(btn_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *no_btn = lv_btn_create(btn_row);
    lv_obj_set_size(no_btn, 100, 32);
    lv_obj_set_style_bg_color(no_btn, ui_muted_color(), 0);
    lv_obj_set_style_radius(no_btn, 8, 0);
    lv_obj_add_event_cb(no_btn, snifferdog_no_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *no_lbl = lv_label_create(no_btn);
    lv_label_set_text(no_lbl, "No");
    lv_obj_set_style_text_color(no_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(no_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(no_lbl);

    lv_obj_t *yes_btn = lv_btn_create(btn_row);
    lv_obj_set_size(yes_btn, 100, 32);
    lv_obj_set_style_bg_color(yes_btn, UI_ACCENT_CYAN, 0);
    lv_obj_set_style_radius(yes_btn, 8, 0);
    lv_obj_add_event_cb(yes_btn, snifferdog_yes_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *yes_lbl = lv_label_create(yes_btn);
    lv_label_set_text(yes_lbl, "Yes");
    lv_obj_set_style_text_color(yes_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(yes_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(yes_lbl);
}

/* ================================================================== */
/*  Tile callbacks                                                     */
/* ================================================================== */

static void on_blackout(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Blackout selected");
    show_blackout_confirm();
}

static void on_handshaker(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Handshaker selected");
    show_handshaker_screen();
}

static void on_portal(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Portal selected");
    show_portal_screen();
}

static void on_snifferdog(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Sniffer Dog selected");
    show_snifferdog_confirm();
}

static void on_wardrive(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Wardrive selected");
    show_wardrive_screen();
}

/* ================================================================== */
/*  Menu page                                                          */
/* ================================================================== */

static void on_back(lv_event_t *e)
{
    (void)e;
    bsp_display_lock(0);
    show_home_screen();
    bsp_display_unlock();
}

void show_global_attacks_screen(void)
{
    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Global WiFi Attacks", on_back, NULL);

    lv_obj_t *grid = lv_obj_create(scr);
    lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(grid, 10, 0);
    lv_obj_set_style_pad_column(grid, 8, 0);
    lv_obj_set_style_pad_top(grid, 10, 0);
    lv_obj_set_style_pad_bottom(grid, 10, 0);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_y(grid, 36);

    ui_create_tile(grid, LV_SYMBOL_POWER,    "Blackout",     UI_ACCENT_RED,    on_blackout,   NULL);
    ui_create_tile(grid, LV_SYMBOL_REFRESH,  "Handshaker",   UI_ACCENT_ORANGE, on_handshaker, NULL);
    ui_create_tile(grid, LV_SYMBOL_HOME,     "Portal",       UI_ACCENT_PURPLE, on_portal,     NULL);
    ui_create_tile(grid, LV_SYMBOL_EYE_OPEN, "Sniffer Dog",  UI_ACCENT_CYAN,   on_snifferdog, NULL);
    ui_create_tile(grid, LV_SYMBOL_GPS,      "Wardrive",     UI_ACCENT_TEAL,   on_wardrive,   NULL);
}
