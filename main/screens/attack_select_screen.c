#include "attack_select_screen.h"
#include "wifi_scan_screen.h"
#include "evil_twin_screen.h"
#include "sae_overflow_screen.h"
#include "handshaker_screen.h"
#include "sniffer_screen.h"
#include "home_screen.h"
#include "ui_helpers.h"
#include "uart_handler.h"
#include "bsp/m5stack_core_s3.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "attack_sel";

/* ---- shared helper: send select_networks with 1-based indices ---- */
void send_select_networks(void)
{
    int indices[MAX_NETWORKS];
    int count = wifi_scan_get_selected(indices, MAX_NETWORKS);
    wifi_network_t *nets = wifi_scan_get_networks();
    char cmd[256] = "select_networks";
    int pos = strlen(cmd);
    for (int i = 0; i < count; i++)
        pos += snprintf(cmd + pos, sizeof(cmd) - pos, " %d", nets[indices[i]].index);
    uart_send_command(cmd);
}

/* ---- deauth popup ---- */
static lv_obj_t *deauth_overlay = NULL;

static void on_deauth_stop(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Deauth STOP");
    uart_send_command("stop");

    if (deauth_overlay) {
        lv_obj_del(deauth_overlay);
        deauth_overlay = NULL;
    }
}

static void on_deauth(lv_event_t *e)
{
    (void)e;
    int indices[MAX_NETWORKS];
    int sel_count = wifi_scan_get_selected(indices, MAX_NETWORKS);
    if (sel_count == 0) return;

    wifi_network_t *nets = wifi_scan_get_networks();

    send_select_networks();
    uart_send_command("start_deauth");

    /* build full-screen overlay popup */
    lv_obj_t *scr = lv_scr_act();
    deauth_overlay = lv_obj_create(scr);
    lv_obj_set_size(deauth_overlay, 300, 220);
    lv_obj_center(deauth_overlay);
    lv_obj_set_style_bg_color(deauth_overlay, UI_BG_CARD, 0);
    lv_obj_set_style_bg_opa(deauth_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(deauth_overlay, 12, 0);
    lv_obj_set_style_border_color(deauth_overlay, UI_ACCENT_RED, 0);
    lv_obj_set_style_border_width(deauth_overlay, 2, 0);
    lv_obj_set_style_pad_all(deauth_overlay, 10, 0);
    lv_obj_set_flex_flow(deauth_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(deauth_overlay, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(deauth_overlay);
    lv_label_set_text(title, "Attacking networks:");
    lv_obj_set_style_text_color(title, UI_ACCENT_RED, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);

    /* scrollable list of targets */
    lv_obj_t *list = lv_obj_create(deauth_overlay);
    lv_obj_set_size(list, LV_PCT(100), 120);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 2, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(list, 1);

    for (int i = 0; i < sel_count; i++) {
        wifi_network_t *net = &nets[indices[i]];
        char line[80];
        snprintf(line, sizeof(line), "%s  %s  ch%d",
                 net->ssid[0] ? net->ssid : "(hidden)",
                 net->bssid, net->channel);
        lv_obj_t *lbl = lv_label_create(list);
        lv_label_set_text(lbl, line);
        lv_obj_set_style_text_color(lbl, UI_TEXT_COLOR, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
    }

    /* stop button */
    lv_obj_t *btn = lv_btn_create(deauth_overlay);
    lv_obj_set_size(btn, 120, 34);
    lv_obj_set_style_bg_color(btn, UI_ACCENT_RED, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_add_event_cb(btn, on_deauth_stop, LV_EVENT_CLICKED, NULL);
    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, LV_SYMBOL_CLOSE " Stop");
    lv_obj_set_style_text_color(btn_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(btn_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(btn_lbl);
}

/* ---- other attack placeholders ---- */

static void on_evil_twin(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Evil Twin selected");
    show_evil_twin_screen();
}

static void on_sae_overflow(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "SAE Overflow selected");
    show_sae_overflow_screen();
}

static void on_handshaker(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Handshaker selected");
    show_handshaker_for_selected();
}

static void on_sniffer(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Sniffer selected");
    show_sniffer_screen();
}

/* ---- back ---- */

static void on_back(lv_event_t *e)
{
    (void)e;
    bsp_display_lock(0);
    show_wifi_scan_screen();
    bsp_display_unlock();
}

/* ---- main screen builder ---- */

void show_attack_select_screen(void)
{
    lv_obj_t *scr = ui_screen_clear();

    int sel[MAX_NETWORKS];
    int sel_count = wifi_scan_get_selected(sel, MAX_NETWORKS);
    char title[40];
    snprintf(title, sizeof(title), "Attack (%d nets)", sel_count);
    ui_create_top_bar(scr, title, on_back, NULL);

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
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    ui_create_tile(grid, "Deauth",        UI_ACCENT_RED,    on_deauth,       NULL);
    ui_create_tile(grid, "Evil Twin",     UI_ACCENT_PURPLE, on_evil_twin,    NULL);
    ui_create_tile(grid, "SAE\nOverflow", UI_ACCENT_ORANGE, on_sae_overflow, NULL);
    ui_create_tile(grid, "Handshaker",    UI_ACCENT_CYAN,   on_handshaker,   NULL);
    ui_create_tile(grid, "Sniffer",       UI_ACCENT_GREEN,  on_sniffer,      NULL);
}
