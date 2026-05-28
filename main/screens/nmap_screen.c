#include "nmap_screen.h"
#include "attack_select_screen.h"
#include "wifi_scan_screen.h"
#include "wifi_connect_helper.h"
#include "ui_helpers.h"
#include "uart_handler.h"
#include <stdio.h>
#include <string.h>

static wifi_network_t s_net;
static char s_password[64];
static char s_target_ip[16];
static lan_host_t s_hosts[WIFI_MAX_HOSTS];
static int s_host_count;
static lv_obj_t *s_log_lbl = NULL;
static volatile bool s_scanning = false;

static wifi_network_t *get_single_net(wifi_network_t *out)
{
    int idx[MAX_NETWORKS];
    if (wifi_scan_get_selected(idx, MAX_NETWORKS) != 1) return NULL;
    wifi_network_t *nets = wifi_scan_get_networks();
    if (!nets) return NULL;
    *out = nets[idx[0]];
    return out;
}

static void on_back(lv_event_t *e);
static void show_nmap_mode_picker(void);
static void show_host_picker(void);

static void on_back(lv_event_t *e)
{
    (void)e;
    s_scanning = false;
    uart_send_command("stop");
    uart_set_line_callback(NULL);
    show_attack_select_screen();
}

static void nmap_line_cb(const char *line)
{
    if (!s_scanning || !line) return;

    bool update = false;
    if (strstr(line, "Scanning") || strstr(line, "/tcp") ||
        strstr(line, "Scanned") || strstr(line, "Host:") ||
        strstr(line, "open ports")) {
        update = true;
    }

    if (update && s_log_lbl && ui_display_lock_wait()) {
        lv_label_set_text(s_log_lbl, line);
        ui_display_unlock_safe();
    }
}

static void start_nmap_scan(const char *mode, const char *ip)
{
    char cmd[64];
    if (ip && ip[0])
        snprintf(cmd, sizeof(cmd), "start_nmap %s %s", mode, ip);
    else
        snprintf(cmd, sizeof(cmd), "start_nmap %s", mode);

    s_scanning = true;
    uart_set_line_callback(nmap_line_cb);
    uart_send_command(cmd);
}

static void on_mode_click(lv_event_t *e)
{
    const char *mode = (const char *)lv_event_get_user_data(e);
    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Nmap Scan", on_back, NULL);

    s_log_lbl = lv_label_create(scr);
    lv_label_set_text(s_log_lbl, "Scanning...");
    lv_obj_set_style_text_color(s_log_lbl, ui_text_color(), 0);
    lv_obj_set_style_text_font(s_log_lbl, &lv_font_montserrat_10, 0);
    lv_obj_set_width(s_log_lbl, 300);
    lv_label_set_long_mode(s_log_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_log_lbl, LV_ALIGN_CENTER, 0, 0);

    start_nmap_scan(mode, s_target_ip[0] ? s_target_ip : NULL);
}

static void on_host_click(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_host_count) return;
    strncpy(s_target_ip, s_hosts[idx].ip, sizeof(s_target_ip) - 1);
    s_target_ip[sizeof(s_target_ip) - 1] = '\0';
    show_nmap_mode_picker();
}

static void show_host_picker(void)
{
    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Nmap Hosts", on_back, NULL);

    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_set_size(list, LV_PCT(100), 240 - 36);
    lv_obj_set_pos(list, 0, 36);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 3, 0);

    lv_obj_t *all_btn = lv_btn_create(list);
    lv_obj_set_size(all_btn, LV_PCT(100), 32);
    lv_obj_add_event_cb(all_btn, on_mode_click, LV_EVENT_CLICKED, (void *)"medium");
    lv_obj_t *all_lbl = lv_label_create(all_btn);
    lv_label_set_text(all_lbl, "Scan all hosts (medium)");
    lv_obj_center(all_lbl);
    s_target_ip[0] = '\0';

    for (int i = 0; i < s_host_count; i++) {
        lv_obj_t *btn = lv_btn_create(list);
        lv_obj_set_size(btn, LV_PCT(100), 30);
        lv_obj_add_event_cb(btn, on_host_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *lbl = lv_label_create(btn);
        char line[40];
        snprintf(line, sizeof(line), "%.15s  %.17s",
                 s_hosts[i].ip, s_hosts[i].mac);
        lv_label_set_text(lbl, line);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
    }
}

static void show_nmap_mode_picker(void)
{
    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Nmap Mode", on_back, NULL);

    lv_obj_t *col = lv_obj_create(scr);
    lv_obj_set_size(col, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_align(col, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 6, 0);

    const char *modes[] = { "quick", "medium", "heavy" };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *btn = lv_btn_create(col);
        lv_obj_set_size(btn, 160, 34);
        lv_obj_add_event_cb(btn, on_mode_click, LV_EVENT_CLICKED, (void *)modes[i]);
        lv_obj_t *lbl = lv_label_create(btn);
        char txt[24];
        snprintf(txt, sizeof(txt), "%s scan", modes[i]);
        lv_label_set_text(lbl, txt);
        lv_obj_center(lbl);
    }
}

static void on_connect_done(bool success, void *unused)
{
    (void)unused;
    if (!success) {
        if (ui_display_lock_wait()) {
            lv_obj_t *scr = ui_screen_clear();
            ui_create_top_bar(scr, "Nmap", on_back, NULL);
            lv_obj_t *msg = lv_label_create(scr);
            lv_label_set_text(msg, "WiFi connect failed.");
            lv_obj_center(msg);
            ui_display_unlock_safe();
        }
        return;
    }

    s_host_count = wifi_collect_list_hosts(s_hosts, WIFI_MAX_HOSTS, 800);
    if (ui_display_lock_wait()) {
        if (s_host_count > 0)
            show_host_picker();
        else {
            s_target_ip[0] = '\0';
            show_nmap_mode_picker();
        }
        ui_display_unlock_safe();
    }
}

static void on_password_confirm(const char *text, void *unused)
{
    (void)unused;
    if (text && text[0])
        snprintf(s_password, sizeof(s_password), "%s", text);
    wifi_connect_async(&s_net, s_password[0] ? s_password : NULL,
                       on_connect_done, NULL);
}

static void begin_connect(void)
{
    if (wifi_network_is_open(&s_net)) {
        wifi_connect_async(&s_net, NULL, on_connect_done, NULL);
        return;
    }
    char evil_pass[64] = {0};
    if (wifi_lookup_evil_password(s_net.ssid, evil_pass, sizeof(evil_pass))) {
        snprintf(s_password, sizeof(s_password), "%s", evil_pass);
        wifi_connect_async(&s_net, s_password, on_connect_done, NULL);
        return;
    }
    ui_show_text_input_popup("WiFi Password", "", 63, UI_ACCENT_GREEN,
                             on_password_confirm, NULL, NULL);
}

void show_nmap_screen(void)
{
    s_password[0] = '\0';
    s_target_ip[0] = '\0';
    s_scanning = false;

    if (!get_single_net(&s_net)) {
        show_attack_select_screen();
        return;
    }

    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Nmap", on_back, NULL);
    lv_obj_t *lbl = lv_label_create(scr);
    lv_label_set_text(lbl, "Connecting...");
    lv_obj_center(lbl);
    begin_connect();
}
