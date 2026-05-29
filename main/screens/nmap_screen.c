#include "nmap_screen.h"
#include "attack_select_screen.h"
#include "wifi_scan_screen.h"
#include "wifi_connect_helper.h"
#include "ui_helpers.h"
#include "uart_handler.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static wifi_network_t s_net;
static char s_password[64];
static char s_target_ip[16];
static lan_host_t s_hosts[WIFI_MAX_HOSTS];
static int s_host_count;
static volatile bool s_scanning = false;
static lv_obj_t *s_status_lbl = NULL;

/* Results table refs */
static lv_obj_t *s_results_container = NULL;
static lv_obj_t *s_progress_lbl = NULL;
static lv_obj_t *s_progress_bar = NULL;

/* Stream parse state */
static int  s_total_ports;
static int  s_hosts_expected;
static int  s_hosts_completed;
static char s_cur_host_ip[20];
static bool s_cur_had_port;

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
    s_status_lbl = NULL;
    s_results_container = NULL;
    s_progress_lbl = NULL;
    s_progress_bar = NULL;
    show_attack_select_screen();
}

/* ---- results table row builders (call with display lock held) ---- */
static void append_port_row(const char *ip, int port, const char *service)
{
    if (!s_results_container || !lv_obj_is_valid(s_results_container)) return;

    lv_obj_t *row = lv_obj_create(s_results_container);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, ui_card_color(), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 4, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 4, 0);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ip_lbl = lv_label_create(row);
    lv_label_set_text(ip_lbl, ip);
    lv_obj_set_style_text_font(ip_lbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(ip_lbl, ui_muted_color(), 0);
    lv_label_set_long_mode(ip_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(ip_lbl, 110);

    lv_obj_t *port_lbl = lv_label_create(row);
    char pbuf[16];
    snprintf(pbuf, sizeof(pbuf), "%d/tcp", port);
    lv_label_set_text(port_lbl, pbuf);
    lv_obj_set_style_text_font(port_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(port_lbl, UI_ACCENT_GREEN, 0);
    lv_obj_set_width(port_lbl, 58);

    lv_obj_t *svc_lbl = lv_label_create(row);
    lv_label_set_text(svc_lbl, service[0] ? service : "?");
    lv_obj_set_style_text_font(svc_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(svc_lbl, ui_text_color(), 0);
    lv_label_set_long_mode(svc_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(svc_lbl, 1);

    lv_obj_scroll_to_view(row, LV_ANIM_OFF);
}

static void append_no_ports_row(const char *ip)
{
    if (!s_results_container || !lv_obj_is_valid(s_results_container)) return;

    lv_obj_t *row = lv_obj_create(s_results_container);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 4, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(row);
    char buf[48];
    snprintf(buf, sizeof(buf), "%.16s  no open ports", ip);
    lv_label_set_text(lbl, buf);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(lbl, ui_muted_color(), 0);
}

static void nmap_results_line_cb(const char *line)
{
    if (!s_scanning || !line) return;

    const char *t = line;
    while (*t == ' ' || *t == '\t') t++;

    /* Scan level: total ports per host, e.g. "Scan level: quick (20)" */
    if (strstr(line, "Scan level:")) {
        const char *paren = strchr(line, '(');
        if (paren) s_total_ports = atoi(paren + 1);
        return;
    }
    /* "Scanning N host(s), ..." */
    if (strstr(line, "host(s),")) {
        sscanf(line, "Scanning %d", &s_hosts_expected);
        return;
    }
    if (strstr(line, "Single-host mode")) {
        s_hosts_expected = 1;
        return;
    }
    /* New host block: "Host: IP (MAC)" */
    if (strncmp(t, "Host:", 5) == 0) {
        if (s_cur_host_ip[0] && !s_cur_had_port) {
            if (ui_display_lock_wait()) {
                append_no_ports_row(s_cur_host_ip);
                ui_display_unlock_safe();
            }
        }
        if (s_cur_host_ip[0]) s_hosts_completed++;

        char ip[20] = {0};
        if (sscanf(t, "Host: %19s", ip) == 1) {
            snprintf(s_cur_host_ip, sizeof(s_cur_host_ip), "%s", ip);
        }
        s_cur_had_port = false;
        return;
    }
    /* Progress: "Scanning X.X.X.X ports N-M [cur/total]" */
    if (strstr(t, "Scanning") && strstr(t, "ports") && strchr(t, '[')) {
        const char *br = strchr(t, '[');
        int cur = 0, tot = 0;
        if (br && sscanf(br, "[%d/%d]", &cur, &tot) == 2 && tot > 0) {
            int pct;
            if (s_hosts_expected > 0 && s_total_ports > 0) {
                int done = s_hosts_completed * s_total_ports + cur;
                int all  = s_hosts_expected * s_total_ports;
                pct = all > 0 ? (done * 100) / all : (cur * 100) / tot;
            } else {
                pct = (cur * 100) / tot;
            }
            if (pct > 100) pct = 100;
            if (ui_display_lock_wait()) {
                if (s_progress_bar && lv_obj_is_valid(s_progress_bar))
                    lv_bar_set_value(s_progress_bar, pct, LV_ANIM_ON);
                if (s_progress_lbl && lv_obj_is_valid(s_progress_lbl)) {
                    char b[16];
                    snprintf(b, sizeof(b), "%d%%", pct);
                    lv_label_set_text(s_progress_lbl, b);
                }
                ui_display_unlock_safe();
            }
        }
        return;
    }
    /* Open port: "135/tcp  open  MSRPC" */
    if (strstr(t, "/tcp") && strstr(t, "open")) {
        int port = 0;
        char service[32] = {0};
        if (sscanf(t, "%d/tcp %*s %31s", &port, service) >= 1 && port > 0) {
            s_cur_had_port = true;
            if (ui_display_lock_wait()) {
                append_port_row(s_cur_host_ip[0] ? s_cur_host_ip : "?",
                                port, service);
                ui_display_unlock_safe();
            }
        }
        return;
    }
    /* Host with no open ports */
    if (strstr(t, "(no open ports)")) {
        if (s_cur_host_ip[0] && !s_cur_had_port) {
            if (ui_display_lock_wait()) {
                append_no_ports_row(s_cur_host_ip);
                ui_display_unlock_safe();
            }
        }
        s_cur_had_port = true;
        return;
    }
    /* Completion: "Scanned N hosts, found M open ports" */
    if (strstr(line, "Scanned") && strstr(line, "open ports")) {
        int hosts = 0, ports = 0;
        sscanf(line, "Scanned %d hosts, found %d open ports", &hosts, &ports);
        s_scanning = false;
        uart_set_line_callback(NULL);
        if (ui_display_lock_wait()) {
            if (s_progress_bar && lv_obj_is_valid(s_progress_bar))
                lv_bar_set_value(s_progress_bar, 100, LV_ANIM_ON);
            if (s_progress_lbl && lv_obj_is_valid(s_progress_lbl)) {
                char b[32];
                snprintf(b, sizeof(b), "Done: %dh %dp", hosts, ports);
                lv_label_set_text(s_progress_lbl, b);
                lv_obj_set_style_text_color(s_progress_lbl, UI_ACCENT_GREEN, 0);
            }
            ui_display_unlock_safe();
        }
        return;
    }
}

static void start_nmap_scan(const char *mode, const char *ip)
{
    char cmd[64];
    if (ip && ip[0])
        snprintf(cmd, sizeof(cmd), "start_nmap %s %s", mode, ip);
    else
        snprintf(cmd, sizeof(cmd), "start_nmap %s", mode);

    s_total_ports     = 0;
    s_hosts_expected  = 0;
    s_hosts_completed = 0;
    s_cur_host_ip[0]  = '\0';
    s_cur_had_port    = false;

    s_scanning = true;
    uart_set_line_callback(nmap_results_line_cb);
    uart_send_command(cmd);
}

static void on_mode_click(lv_event_t *e)
{
    const char *mode = (const char *)lv_event_get_user_data(e);

    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Nmap Results", on_back, NULL);

    /* Progress row */
    s_progress_bar = lv_bar_create(scr);
    lv_obj_set_size(s_progress_bar, 120, 12);
    lv_obj_set_pos(s_progress_bar, 8, 44);
    lv_bar_set_range(s_progress_bar, 0, 100);
    lv_bar_set_value(s_progress_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_progress_bar, ui_card_color(), 0);
    lv_obj_set_style_bg_color(s_progress_bar, UI_ACCENT_GREEN, LV_PART_INDICATOR);

    s_progress_lbl = lv_label_create(scr);
    lv_label_set_text(s_progress_lbl, "Scanning...");
    lv_obj_set_style_text_color(s_progress_lbl, UI_ACCENT_ORANGE, 0);
    lv_obj_set_style_text_font(s_progress_lbl, &lv_font_montserrat_10, 0);
    lv_label_set_long_mode(s_progress_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_progress_lbl, 320 - 138);
    lv_obj_set_pos(s_progress_lbl, 134, 42);

    /* Column header */
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_set_size(hdr, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_pos(hdr, 0, 60);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 4, 0);
    lv_obj_set_style_pad_column(hdr, 6, 0);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *h_host = lv_label_create(hdr);
    lv_label_set_text(h_host, "HOST");
    lv_obj_set_style_text_font(h_host, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(h_host, ui_muted_color(), 0);
    lv_obj_set_width(h_host, 110);

    lv_obj_t *h_port = lv_label_create(hdr);
    lv_label_set_text(h_port, "PORT");
    lv_obj_set_style_text_font(h_port, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(h_port, ui_muted_color(), 0);
    lv_obj_set_width(h_port, 58);

    lv_obj_t *h_svc = lv_label_create(hdr);
    lv_label_set_text(h_svc, "SERVICE");
    lv_obj_set_style_text_font(h_svc, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(h_svc, ui_muted_color(), 0);

    /* Scrollable results container */
    s_results_container = lv_obj_create(scr);
    lv_obj_set_size(s_results_container, LV_PCT(100), 240 - 84);
    lv_obj_set_pos(s_results_container, 0, 84);
    lv_obj_set_style_bg_opa(s_results_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_results_container, 0, 0);
    lv_obj_set_style_pad_all(s_results_container, 4, 0);
    lv_obj_set_style_pad_row(s_results_container, 2, 0);
    lv_obj_set_flex_flow(s_results_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_results_container, LV_DIR_VER);

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

static void on_all_hosts_click(lv_event_t *e)
{
    (void)e;
    s_target_ip[0] = '\0';
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
    lv_obj_add_event_cb(all_btn, on_all_hosts_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *all_lbl = lv_label_create(all_btn);
    lv_label_set_text(all_lbl, "Scan all hosts");
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

/* Runs on the LVGL task (via ui_lvgl_async_call) once list_hosts finished. */
static void hosts_done_lvgl_cb(void *arg)
{
    (void)arg;
    if (s_host_count > 0) {
        show_host_picker();
    } else {
        s_target_ip[0] = '\0';
        show_nmap_mode_picker();
    }
}

/* list_hosts blocks for up to a few seconds; keep it off the LVGL task. */
static void hosts_collect_task(void *arg)
{
    (void)arg;
    s_host_count = wifi_collect_list_hosts(s_hosts, WIFI_MAX_HOSTS, 1500);
    if (!ui_lvgl_async_call(hosts_done_lvgl_cb, NULL))
        hosts_done_lvgl_cb(NULL);
    vTaskDelete(NULL);
}

static void on_connect_done(bool success, void *unused)
{
    (void)unused;
    if (!success) {
        if (ui_display_lock_wait()) {
            lv_obj_t *scr = ui_screen_clear();
            s_status_lbl = NULL;
            ui_create_top_bar(scr, "Nmap", on_back, NULL);
            lv_obj_t *msg = lv_label_create(scr);
            lv_label_set_text(msg, "WiFi connect failed.");
            lv_obj_center(msg);
            ui_display_unlock_safe();
        }
        return;
    }

    if (ui_display_lock_wait()) {
        if (s_status_lbl && lv_obj_is_valid(s_status_lbl))
            lv_label_set_text(s_status_lbl, "Connected. Listing hosts...");
        ui_display_unlock_safe();
    }

    xTaskCreate(hosts_collect_task, "nmap_hosts", 4096, NULL, 5, NULL);
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
    s_status_lbl = NULL;
    s_results_container = NULL;
    s_progress_lbl = NULL;
    s_progress_bar = NULL;
    s_total_ports = 0;
    s_hosts_expected = 0;
    s_hosts_completed = 0;
    s_cur_host_ip[0] = '\0';
    s_cur_had_port = false;

    if (!get_single_net(&s_net)) {
        show_attack_select_screen();
        return;
    }

    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Nmap", on_back, NULL);
    s_status_lbl = lv_label_create(scr);
    lv_label_set_text(s_status_lbl, "Connecting...");
    lv_obj_set_style_text_color(s_status_lbl, ui_text_color(), 0);
    lv_obj_center(s_status_lbl);
    begin_connect();
}
