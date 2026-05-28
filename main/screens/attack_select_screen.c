#include "attack_select_screen.h"
#include "wifi_scan_screen.h"
#include "inspect_network.h"
#include "evil_twin_screen.h"
#include "sae_overflow_screen.h"
#include "handshaker_screen.h"
#include "sniffer_screen.h"
#include "arp_poison_screen.h"
#include "rogue_ap_screen.h"
#include "mitm_screen.h"
#include "nmap_screen.h"
#include "ap_radar_screen.h"
#include "ui_helpers.h"
#include "uart_handler.h"
#include <stdio.h>
#include <string.h>

static lv_obj_t *s_error_overlay = NULL;

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

static int get_selected_count(void)
{
    int sel[MAX_NETWORKS];
    return wifi_scan_get_selected(sel, MAX_NETWORKS);
}

static void dismiss_error(lv_event_t *e)
{
    (void)e;
    if (s_error_overlay) {
        lv_obj_del(s_error_overlay);
        s_error_overlay = NULL;
    }
}

static void show_error_popup(const char *msg)
{
    if (s_error_overlay) {
        lv_obj_del(s_error_overlay);
        s_error_overlay = NULL;
    }
    lv_obj_t *scr = lv_scr_act();
    s_error_overlay = lv_obj_create(scr);
    lv_obj_set_size(s_error_overlay, 260, 100);
    lv_obj_center(s_error_overlay);
    lv_obj_set_style_bg_color(s_error_overlay, ui_card_color(), 0);
    lv_obj_set_style_radius(s_error_overlay, 10, 0);
    lv_obj_set_style_border_color(s_error_overlay, UI_ACCENT_RED, 0);
    lv_obj_set_style_border_width(s_error_overlay, 2, 0);
    lv_obj_set_style_pad_all(s_error_overlay, 10, 0);
    lv_obj_add_event_cb(s_error_overlay, dismiss_error, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(s_error_overlay);
    lv_label_set_text(lbl, msg);
    lv_obj_set_style_text_color(lbl, UI_ACCENT_RED, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl, 230);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_center(lbl);
}

static bool require_single(const char *attack_name)
{
    int n = get_selected_count();
    if (n == 1) return true;
    char msg[64];
    snprintf(msg, sizeof(msg), "Select exactly 1 network\nfor %s", attack_name);
    show_error_popup(msg);
    return false;
}

static bool require_any(void)
{
    if (get_selected_count() > 0) return true;
    show_error_popup("Select at least 1 network");
    return false;
}

/* ---- deauth popup ---- */
static lv_obj_t *deauth_overlay = NULL;

static void on_deauth_stop(lv_event_t *e)
{
    (void)e;
    uart_send_command("stop");
    if (deauth_overlay) {
        lv_obj_del(deauth_overlay);
        deauth_overlay = NULL;
    }
}

static void on_deauth(lv_event_t *e)
{
    (void)e;
    wifi_inspect_cancel();
    if (!require_any()) return;

    int indices[MAX_NETWORKS];
    int sel_count = wifi_scan_get_selected(indices, MAX_NETWORKS);
    wifi_network_t *nets = wifi_scan_get_networks();

    send_select_networks();
    uart_send_command("start_deauth");

    lv_obj_t *scr = lv_scr_act();
    deauth_overlay = lv_obj_create(scr);
    lv_obj_set_size(deauth_overlay, 300, 220);
    lv_obj_center(deauth_overlay);
    lv_obj_set_style_bg_color(deauth_overlay, ui_card_color(), 0);
    lv_obj_set_style_bg_opa(deauth_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(deauth_overlay, 12, 0);
    lv_obj_set_style_border_color(deauth_overlay, UI_ACCENT_RED, 0);
    lv_obj_set_style_border_width(deauth_overlay, 2, 0);
    lv_obj_set_style_pad_all(deauth_overlay, 10, 0);
    lv_obj_set_flex_flow(deauth_overlay, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *title = lv_label_create(deauth_overlay);
    lv_label_set_text(title, "Attacking networks:");
    lv_obj_set_style_text_color(title, UI_ACCENT_RED, 0);

    lv_obj_t *list = lv_obj_create(deauth_overlay);
    lv_obj_set_size(list, LV_PCT(100), 120);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
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
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
    }

    lv_obj_t *btn = lv_btn_create(deauth_overlay);
    lv_obj_set_size(btn, 120, 34);
    lv_obj_set_style_bg_color(btn, UI_ACCENT_RED, 0);
    lv_obj_add_event_cb(btn, on_deauth_stop, LV_EVENT_CLICKED, NULL);
    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, LV_SYMBOL_CLOSE " Stop");
    lv_obj_center(btn_lbl);
}

static void on_evil_twin(lv_event_t *e)
{
    (void)e;
    wifi_inspect_cancel();
    if (!require_any()) return;
    show_evil_twin_screen();
}

static void on_sae_overflow(lv_event_t *e)
{
    (void)e;
    wifi_inspect_cancel();
    if (!require_single("SAE Overflow")) return;
    show_sae_overflow_screen();
}

static void on_handshaker(lv_event_t *e)
{
    (void)e;
    wifi_inspect_cancel();
    if (!require_any()) return;
    show_handshaker_for_selected();
}

static void on_sniffer(lv_event_t *e)
{
    (void)e;
    wifi_inspect_cancel();
    if (!require_any()) return;
    show_sniffer_screen();
}

static void on_arp(lv_event_t *e)
{
    (void)e;
    wifi_inspect_cancel();
    if (!require_single("ARP Poison")) return;
    show_arp_poison_screen();
}

static void on_rogue_ap(lv_event_t *e)
{
    (void)e;
    wifi_inspect_cancel();
    if (!require_single("Rogue AP")) return;
    show_rogue_ap_screen();
}

static void on_mitm(lv_event_t *e)
{
    (void)e;
    wifi_inspect_cancel();
    if (!require_single("MITM")) return;
    show_mitm_screen();
}

static void on_nmap(lv_event_t *e)
{
    (void)e;
    wifi_inspect_cancel();
    if (!require_single("Nmap")) return;
    show_nmap_screen();
}

static void on_radar(lv_event_t *e)
{
    (void)e;
    wifi_inspect_cancel();
    if (!require_single("Radar")) return;
    send_select_networks();
    uart_send_command("start_ap_locator");
    show_ap_radar_screen();
}

static void on_back(lv_event_t *e)
{
    (void)e;
    wifi_inspect_cancel();
    show_wifi_scan_screen();
}

void show_attack_select_screen(void)
{
    lv_obj_t *scr = ui_screen_clear();

    int sel_count = get_selected_count();
    char title[40];
    snprintf(title, sizeof(title), "Attack (%d nets)", sel_count);
    ui_create_top_bar(scr, title, on_back, NULL);

    lv_obj_t *grid = lv_obj_create(scr);
    lv_obj_set_size(grid, LV_PCT(100), 204);
    lv_obj_set_pos(grid, 0, 36);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(grid, 8, 0);
    lv_obj_set_style_pad_column(grid, 6, 0);
    lv_obj_set_style_pad_all(grid, 6, 0);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_add_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    if (ui_red_team_enabled()) {
        ui_create_tile(grid, LV_SYMBOL_WARNING,  "Deauth",        UI_ACCENT_RED,    on_deauth,       NULL);
        ui_create_tile(grid, LV_SYMBOL_CLOSE,    "Evil Twin",     UI_ACCENT_PURPLE, on_evil_twin,    NULL);
        ui_create_tile(grid, LV_SYMBOL_CHARGE,   "SAE\nOverflow", UI_ACCENT_ORANGE, on_sae_overflow, NULL);
        ui_create_tile(grid, LV_SYMBOL_REFRESH,  "Handshaker",    UI_ACCENT_CYAN,   on_handshaker,   NULL);
        ui_create_tile(grid, LV_SYMBOL_EYE_OPEN, "Sniffer",       UI_ACCENT_GREEN,  on_sniffer,      NULL);
        ui_create_tile(grid, LV_SYMBOL_SHUFFLE,  "ARP",           UI_ACCENT_PURPLE, on_arp,          NULL);
        ui_create_tile(grid, LV_SYMBOL_WIFI,     "Rogue AP",      UI_ACCENT_CYAN,   on_rogue_ap,     NULL);
        ui_create_tile(grid, LV_SYMBOL_COPY,     "MITM",          UI_ACCENT_TEAL,   on_mitm,         NULL);
        ui_create_tile(grid, LV_SYMBOL_LIST,     "Nmap",          UI_ACCENT_GREEN,  on_nmap,         NULL);
        ui_create_tile(grid, LV_SYMBOL_GPS,      "Radar",         UI_ACCENT_BLUE,   on_radar,        NULL);
    } else {
        ui_create_tile(grid, LV_SYMBOL_SHUFFLE,  "ARP",           UI_ACCENT_PURPLE, on_arp,          NULL);
        ui_create_tile(grid, LV_SYMBOL_LIST,     "Nmap",          UI_ACCENT_GREEN,  on_nmap,         NULL);
    }
}
