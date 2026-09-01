#include "rogue_gitm_screen.h"
#include "attack_select_screen.h"
#include "wifi_scan_screen.h"
#include "wifi_connect_helper.h"
#include "ui_helpers.h"
#include "uart_handler.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "rogue_gitm";

#define GITM_MAX_CLIENTS 4
#define GITM_STATUS_MS   3000

static wifi_network_t s_victim;
static wifi_network_t s_home;
static char s_home_pass[64];
static char s_mirror_pass[64];
static bool s_session_alive;
static bool s_running;
static bool s_started;
static bool s_status_pending;
static bool s_refused;

static lv_obj_t *s_info_lbl;
static lv_obj_t *s_status_lbl;
static lv_obj_t *s_clients_lbl;
static lv_timer_t *s_status_timer;

static unsigned long s_clients;
static unsigned long s_packets;
static unsigned long s_drops;
static int s_upstream; /* -1 unknown, 0 down, 1 up */
static char s_client_lines[GITM_MAX_CLIENTS][48];
static int s_client_count;
static char s_error[96];

static void show_home_picker(void);
static void begin_home_password(void);
static void begin_mirror_password(void);
static void show_connecting_screen(void);
static void show_running_screen(void);
static void start_gateway(void);
static void refresh_status_labels(void);

static void stop_status_timer(void)
{
    if (s_status_timer) {
        lv_timer_delete(s_status_timer);
        s_status_timer = NULL;
    }
}

static void cleanup_session(bool send_stop)
{
    s_session_alive = false;
    s_running = false;
    s_started = false;
    s_status_pending = false;
    stop_status_timer();
    uart_set_line_callback(NULL);
    if (send_stop)
        uart_send_command("stop");
    s_info_lbl = NULL;
    s_status_lbl = NULL;
    s_clients_lbl = NULL;
}

static void on_back_to_attacks(lv_event_t *e)
{
    (void)e;
    cleanup_session(true);
    show_attack_select_screen();
}

static void on_picker_back(lv_event_t *e)
{
    (void)e;
    cleanup_session(false);
    show_attack_select_screen();
}

static bool get_single_victim(wifi_network_t *out)
{
    int idx[MAX_NETWORKS];
    if (wifi_scan_get_selected(idx, MAX_NETWORKS) != 1) return false;
    wifi_network_t *nets = wifi_scan_get_networks();
    if (!nets) return false;
    *out = nets[idx[0]];
    return true;
}

static bool parse_kv_u32(const char *line, const char *key, unsigned long *out)
{
    const char *p = strstr(line, key);
    if (!p) return false;
    p += strlen(key);
    if (*p != '=') return false;
    *out = strtoul(p + 1, NULL, 10);
    return true;
}

static void refresh_status_labels(void)
{
    if (s_status_lbl) {
        if (s_error[0]) {
            lv_label_set_text(s_status_lbl, s_error);
            lv_obj_set_style_text_color(s_status_lbl, UI_ACCENT_RED, 0);
        } else {
            char buf[128];
            snprintf(buf, sizeof(buf), "clients=%lu  up=%s\npkts=%lu  drops=%lu",
                     s_clients,
                     s_upstream < 0 ? "?" : (s_upstream ? "1" : "0"),
                     s_packets, s_drops);
            lv_label_set_text(s_status_lbl, buf);
            lv_obj_set_style_text_color(s_status_lbl, UI_ACCENT_ORANGE, 0);
        }
    }
    if (s_clients_lbl) {
        if (s_client_count == 0) {
            lv_label_set_text(s_clients_lbl, "No SoftAP clients");
        } else {
            char buf[200] = {0};
            int pos = 0;
            for (int i = 0; i < s_client_count && pos < (int)sizeof(buf) - 1; i++) {
                pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "%s%s",
                                i ? "\n" : "", s_client_lines[i]);
            }
            lv_label_set_text(s_clients_lbl, buf);
        }
    }
}

static void gitm_line_cb(const char *line)
{
    if (!s_running || !line) return;

    if (strstr(line, "Rogue GITM refused") ||
        strstr(line, "No upstream IPv4") ||
        strstr(line, "Password length must be") ||
        strstr(line, "SSID length must be")) {
        s_refused = true;
        snprintf(s_error, sizeof(s_error), "%s", line);
        ESP_LOGW(TAG, "%s", line);
        if (ui_display_lock_wait()) {
            refresh_status_labels();
            ui_display_unlock_safe();
        }
        return;
    }

    if (strstr(line, "Rogue GITM started successfully")) {
        s_started = true;
        s_error[0] = '\0';
        if (ui_display_lock_wait()) {
            if (s_status_lbl) {
                lv_label_set_text(s_status_lbl, "Rogue GITM started. Waiting for clients...");
                lv_obj_set_style_text_color(s_status_lbl, UI_ACCENT_ORANGE, 0);
            }
            ui_display_unlock_safe();
        }
        return;
    }

    if (strncmp(line, "[CGW] status", 12) == 0) {
        unsigned long clients = 0, upstream = 0;
        parse_kv_u32(line, "clients", &clients);
        parse_kv_u32(line, "upstream", &upstream);
        s_clients = clients;
        s_upstream = (int)upstream;
        s_client_count = 0;
        return;
    }

    if (strncmp(line, "[CGW] capture=", 14) == 0) {
        parse_kv_u32(line, "packets", &s_packets);
        parse_kv_u32(line, "drops", &s_drops);
        return;
    }

    if (strncmp(line, "[CGW_CLIENT]", 12) == 0) {
        const char *mac = strstr(line, "mac=");
        const char *ip = strstr(line, "ip=");
        if (mac && ip && s_client_count < GITM_MAX_CLIENTS) {
            mac += 4;
            ip += 3;
            char mac_buf[18] = {0};
            char ip_buf[16] = {0};
            sscanf(mac, "%17s", mac_buf);
            sscanf(ip, "%15s", ip_buf);
            snprintf(s_client_lines[s_client_count], sizeof(s_client_lines[0]),
                     "%s  %s", ip_buf, mac_buf);
            s_client_count++;
        }
        return;
    }

    if (strcmp(line, "[CGW] END") == 0) {
        s_status_pending = false;
        if (ui_display_lock_wait()) {
            refresh_status_labels();
            ui_display_unlock_safe();
        }
    }
}

static void status_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_running || s_refused || s_status_pending) return;
    s_status_pending = true;
    uart_send_command("capture_gateway status");
}

static void start_gateway(void)
{
    send_select_networks();

    char esc_ssid[67];
    char esc_pass[131];
    char cmd[256];
    wifi_escape_quoted_arg(s_victim.ssid, esc_ssid, sizeof(esc_ssid));
    wifi_escape_quoted_arg(s_mirror_pass, esc_pass, sizeof(esc_pass));
    snprintf(cmd, sizeof(cmd), "start_rogue_gitm \"%.65s\" \"%.129s\"",
             esc_ssid, esc_pass);
    ESP_LOGI(TAG, "starting GITM mirror=%s uplink=%s ch=%u",
             s_victim.ssid, s_home.ssid, s_victim.channel);
    uart_send_command(cmd);
}

static void show_running_screen(void)
{
    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Rogue GITM", on_back_to_attacks, NULL);

    lv_obj_t *col = lv_obj_create(scr);
    lv_obj_set_size(col, LV_PCT(100), 240 - 36 - 50);
    lv_obj_set_pos(col, 0, 36);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 8, 0);
    lv_obj_set_style_pad_row(col, 4, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    char info[128];
    snprintf(info, sizeof(info), "Mirror: %s\nUplink: %s  ch%u",
             s_victim.ssid[0] ? s_victim.ssid : "(hidden)",
             s_home.ssid[0] ? s_home.ssid : "(hidden)",
             s_victim.channel);
    s_info_lbl = lv_label_create(col);
    lv_label_set_text(s_info_lbl, info);
    lv_obj_set_style_text_color(s_info_lbl, ui_text_color(), 0);
    lv_obj_set_style_text_font(s_info_lbl, &lv_font_montserrat_12, 0);
    lv_label_set_long_mode(s_info_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_info_lbl, 300);

    s_status_lbl = lv_label_create(col);
    lv_label_set_text(s_status_lbl, "Starting Rogue GITM...");
    lv_obj_set_style_text_color(s_status_lbl, UI_ACCENT_ORANGE, 0);
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_12, 0);
    lv_label_set_long_mode(s_status_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_status_lbl, 300);

    s_clients_lbl = lv_label_create(col);
    lv_label_set_text(s_clients_lbl, "");
    lv_obj_set_style_text_color(s_clients_lbl, ui_muted_color(), 0);
    lv_obj_set_style_text_font(s_clients_lbl, &lv_font_montserrat_10, 0);
    lv_label_set_long_mode(s_clients_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_clients_lbl, 300);

    lv_obj_t *btn = lv_btn_create(scr);
    lv_obj_set_size(btn, 140, 34);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(btn, UI_ACCENT_RED, 0);
    lv_obj_add_event_cb(btn, on_back_to_attacks, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, LV_SYMBOL_CLOSE " Stop");
    lv_obj_center(bl);

    s_running = true;
    s_started = false;
    s_refused = false;
    s_status_pending = false;
    s_clients = 0;
    s_packets = 0;
    s_drops = 0;
    s_upstream = -1;
    s_client_count = 0;
    s_error[0] = '\0';

    uart_set_line_callback(gitm_line_cb);
    start_gateway();

    stop_status_timer();
    s_status_timer = lv_timer_create(status_timer_cb, GITM_STATUS_MS, NULL);
}

static void on_connect_done(bool success, void *unused)
{
    (void)unused;
    if (!s_session_alive) return;
    if (!success) {
        if (ui_display_lock_wait()) {
            cleanup_session(true);
            lv_obj_t *scr = ui_screen_clear();
            ui_create_top_bar(scr, "Rogue GITM", on_picker_back, NULL);
            lv_obj_t *msg = lv_label_create(scr);
            lv_label_set_text(msg, "Home WiFi connect failed.");
            lv_obj_set_style_text_color(msg, UI_ACCENT_RED, 0);
            lv_obj_center(msg);
            ui_display_unlock_safe();
        }
        return;
    }
    if (ui_display_lock_wait()) {
        show_running_screen();
        ui_display_unlock_safe();
    }
}

static void show_connecting_screen(void)
{
    s_session_alive = true;
    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Rogue GITM", on_back_to_attacks, NULL);

    lv_obj_t *lbl = lv_label_create(scr);
    char buf[80];
    snprintf(buf, sizeof(buf), "Connecting to %s...",
             s_home.ssid[0] ? s_home.ssid : "(hidden)");
    lv_label_set_text(lbl, buf);
    lv_obj_set_style_text_color(lbl, UI_ACCENT_ORANGE, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(lbl);

    wifi_connect_async(&s_home, s_home_pass[0] ? s_home_pass : NULL,
                       on_connect_done, NULL);
}

static void on_mirror_pass_cancel(void *unused)
{
    (void)unused;
    show_home_picker();
}

static void on_mirror_pass_confirm(const char *text, void *unused)
{
    (void)unused;
    size_t len = text ? strlen(text) : 0;
    if (len < 8 || len > 63) {
        ui_show_text_input_popup("Mirror password (8-63)",
                                 text ? text : "", 63, UI_ACCENT_ORANGE,
                                 on_mirror_pass_confirm, on_mirror_pass_cancel,
                                 NULL);
        return;
    }
    snprintf(s_mirror_pass, sizeof(s_mirror_pass), "%s", text);
    show_connecting_screen();
}

static void begin_mirror_password(void)
{
    char prefill[64] = {0};
    if (s_victim.ssid[0])
        wifi_lookup_evil_password(s_victim.ssid, prefill, sizeof(prefill));
    ui_show_text_input_popup("Mirror password", prefill, 63, UI_ACCENT_ORANGE,
                             on_mirror_pass_confirm, on_mirror_pass_cancel, NULL);
}

static void on_home_pass_cancel(void *unused)
{
    (void)unused;
    show_home_picker();
}

static void on_home_pass_confirm(const char *text, void *unused)
{
    (void)unused;
    if (text && text[0])
        snprintf(s_home_pass, sizeof(s_home_pass), "%s", text);
    begin_mirror_password();
}

static void begin_home_password(void)
{
    s_home_pass[0] = '\0';
    if (wifi_network_is_open(&s_home)) {
        begin_mirror_password();
        return;
    }
    char evil_pass[64] = {0};
    if (wifi_lookup_evil_password(s_home.ssid, evil_pass, sizeof(evil_pass))) {
        snprintf(s_home_pass, sizeof(s_home_pass), "%s", evil_pass);
        begin_mirror_password();
        return;
    }
    ui_show_text_input_popup("Home WiFi password", "", 63, UI_ACCENT_ORANGE,
                             on_home_pass_confirm, on_home_pass_cancel, NULL);
}

static void on_home_selected(lv_event_t *e)
{
    int pos = (int)(intptr_t)lv_event_get_user_data(e);
    wifi_network_t *nets = wifi_scan_get_networks();
    int count = wifi_scan_get_network_count();
    if (!nets || pos < 0 || pos >= count) return;
    s_home = nets[pos];
    ESP_LOGI(TAG, "uplink pos=%d ssid=%s ch=%u",
             pos, s_home.ssid, s_home.channel);
    begin_home_password();
}

static void show_home_picker(void)
{
    wifi_network_t *nets = wifi_scan_get_networks();
    int count = wifi_scan_get_network_count();

    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Rogue GITM", on_picker_back, NULL);

    if (!nets || count == 0) {
        lv_obj_t *lbl = lv_label_create(scr);
        lv_label_set_text(lbl, "No scan results.");
        lv_obj_set_style_text_color(lbl, ui_muted_color(), 0);
        lv_obj_center(lbl);
        return;
    }

    char hint[72];
    snprintf(hint, sizeof(hint), "Home WiFi on ch%u (not the target):",
             s_victim.channel);
    lv_obj_t *hint_lbl = lv_label_create(scr);
    lv_label_set_text(hint_lbl, hint);
    lv_obj_set_style_text_color(hint_lbl, ui_muted_color(), 0);
    lv_obj_set_style_text_font(hint_lbl, &lv_font_montserrat_10, 0);
    lv_obj_set_pos(hint_lbl, 8, 38);

    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_set_size(list, LV_PCT(100), 240 - 54);
    lv_obj_set_pos(list, 0, 54);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 6, 0);
    lv_obj_set_style_pad_row(list, 4, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    int shown = 0;
    for (int i = 0; i < count; i++) {
        if (s_victim.bssid[0] && strcmp(nets[i].bssid, s_victim.bssid) == 0)
            continue;
        if (nets[i].channel != s_victim.channel)
            continue;

        lv_obj_t *btn = lv_btn_create(list);
        lv_obj_set_size(btn, LV_PCT(100), 34);
        lv_obj_set_style_bg_color(btn, ui_card_color(), 0);
        lv_obj_set_style_bg_color(btn, UI_ACCENT_ORANGE, LV_STATE_PRESSED);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_pad_hor(btn, 8, 0);
        lv_obj_add_event_cb(btn, on_home_selected, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);

        lv_obj_t *lbl = lv_label_create(btn);
        char display[80];
        snprintf(display, sizeof(display), "%s  ch%u  %s",
                 nets[i].ssid[0] ? nets[i].ssid : "(hidden)",
                 nets[i].channel,
                 nets[i].security[0] ? nets[i].security : "OPEN");
        lv_label_set_text(lbl, display);
        lv_obj_set_style_text_color(lbl, ui_text_color(), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
        shown++;
    }

    if (shown == 0) {
        lv_obj_t *lbl = lv_label_create(list);
        char msg[72];
        snprintf(msg, sizeof(msg), "No other AP on ch%u", s_victim.channel);
        lv_label_set_text(lbl, msg);
        lv_obj_set_style_text_color(lbl, UI_ACCENT_RED, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    }
}

void show_rogue_gitm_screen(void)
{
    s_home_pass[0] = '\0';
    s_mirror_pass[0] = '\0';
    s_session_alive = true;
    s_running = false;
    s_started = false;
    s_refused = false;
    s_error[0] = '\0';
    stop_status_timer();
    uart_set_line_callback(NULL);

    if (!get_single_victim(&s_victim)) {
        show_attack_select_screen();
        return;
    }
    if (!s_victim.ssid[0]) {
        lv_obj_t *scr = ui_screen_clear();
        ui_create_top_bar(scr, "Rogue GITM", on_picker_back, NULL);
        lv_obj_t *lbl = lv_label_create(scr);
        lv_label_set_text(lbl, "Hidden SSID cannot be mirrored.");
        lv_obj_set_style_text_color(lbl, UI_ACCENT_RED, 0);
        lv_obj_center(lbl);
        return;
    }

    show_home_picker();
}
