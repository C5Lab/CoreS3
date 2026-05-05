#include "sniffer_screen.h"
#include "attack_select_screen.h"
#include "karma_screen.h"
#include "wifi_scan_screen.h"
#include "ui_helpers.h"
#include "uart_handler.h"
#include "bsp/m5stack_core_s3.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static const char *TAG = "sniffer";

/* ---- observer data ---- */

#define MAX_OBSERVER_NETWORKS 32
#define MAX_CLIENTS_PER_NET   10

typedef struct {
    char ssid[33];
    int  channel;
    int  client_count;
    char clients[MAX_CLIENTS_PER_NET][18];
} observer_network_t;

static observer_network_t obs_networks[MAX_OBSERVER_NETWORKS];
static int obs_network_count = 0;
static int obs_current_net   = -1;
static bool obs_collecting   = false;
static esp_timer_handle_t obs_timeout_timer = NULL;

/* ---- sniffer state ---- */

static lv_obj_t *pkt_label    = NULL;
static bool      sniff_running = false;
static int       pkt_count     = 0;

/* ---- forward declarations ---- */
static void show_sniffer_main(void);
static void show_clients_screen(void);

/* ================================================================== */
/*  UART callback -- normal sniffer mode (packet counter)             */
/* ================================================================== */

static void sniff_uart_line_cb(const char *line)
{
    if (!sniff_running) return;
    const char *p = strstr(line, "Sniffer packet count:");
    if (p) {
        pkt_count = atoi(p + strlen("Sniffer packet count:"));
        bsp_display_lock(0);
        if (pkt_label) {
            char txt[48];
            snprintf(txt, sizeof(txt), "Packets: %d", pkt_count);
            lv_label_set_text(pkt_label, txt);
        }
        bsp_display_unlock();
    }
}

/* ================================================================== */
/*  Show Clients -- collect show_sniffer_results                      */
/* ================================================================== */

static bool parse_observer_network_line(const char *line, observer_network_t *net)
{
    if (line[0] == ' ' || line[0] == '\t') return false;
    const char *ch_marker = strstr(line, ", CH");
    if (!ch_marker) return false;

    int ssid_len = ch_marker - line;
    if (ssid_len > 32) ssid_len = 32;
    memcpy(net->ssid, line, ssid_len);
    net->ssid[ssid_len] = '\0';

    int channel = 0, count = 0;
    if (sscanf(ch_marker, ", CH%d: %d", &channel, &count) == 2) {
        net->channel = channel;
        net->client_count = 0;
        memset(net->clients, 0, sizeof(net->clients));
        return true;
    }
    return false;
}

static bool parse_observer_client_line(const char *line, char *mac_out, size_t mac_size)
{
    if (line[0] != ' ') return false;
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (strlen(p) >= 17 && p[2] == ':' && p[5] == ':') {
        strncpy(mac_out, p, mac_size - 1);
        mac_out[mac_size - 1] = '\0';
        int len = strlen(mac_out);
        while (len > 0 && (mac_out[len-1] == ' ' || mac_out[len-1] == '\r' || mac_out[len-1] == '\n'))
            mac_out[--len] = '\0';
        return true;
    }
    return false;
}

static void obs_timeout_cb(void *arg)
{
    (void)arg;
    if (!obs_collecting) return;
    obs_collecting = false;

    uart_set_line_callback(sniff_uart_line_cb);

    ESP_LOGI(TAG, "Clients collected: %d networks", obs_network_count);

    bsp_display_lock(0);
    show_clients_screen();
    bsp_display_unlock();
}

static void obs_collect_line_cb(const char *line)
{
    if (strstr(line, "Sniffer packet count:")) {
        const char *p = strstr(line, "Sniffer packet count:");
        pkt_count = atoi(p + strlen("Sniffer packet count:"));
        return;
    }

    if (strstr(line, "No APs with clients found")) {
        return;
    }

    observer_network_t net;
    memset(&net, 0, sizeof(net));
    if (parse_observer_network_line(line, &net)) {
        if (obs_network_count < MAX_OBSERVER_NETWORKS) {
            obs_networks[obs_network_count] = net;
            obs_current_net = obs_network_count;
            obs_network_count++;
        }
        esp_timer_stop(obs_timeout_timer);
        esp_timer_start_once(obs_timeout_timer, 800000);
        return;
    }

    char mac[18];
    if (parse_observer_client_line(line, mac, sizeof(mac))) {
        if (obs_current_net >= 0 && obs_current_net < obs_network_count) {
            observer_network_t *cur = &obs_networks[obs_current_net];
            if (cur->client_count < MAX_CLIENTS_PER_NET) {
                memcpy(cur->clients[cur->client_count], mac, 17);
                cur->clients[cur->client_count][17] = '\0';
                cur->client_count++;
            }
        }
        esp_timer_stop(obs_timeout_timer);
        esp_timer_start_once(obs_timeout_timer, 800000);
    }
}

static void start_collect_clients(void)
{
    obs_network_count = 0;
    obs_current_net = -1;
    obs_collecting = true;

    if (!obs_timeout_timer) {
        esp_timer_create_args_t args = {
            .callback = obs_timeout_cb,
            .name = "obs_timeout",
        };
        esp_timer_create(&args, &obs_timeout_timer);
    }

    uart_set_line_callback(obs_collect_line_cb);
    esp_timer_start_once(obs_timeout_timer, 2000000);
    uart_send_command("show_sniffer_results");
}

/* ---- Clients screen UI ---- */

static void on_clients_back(lv_event_t *e)
{
    (void)e;
    bsp_display_lock(0);
    show_sniffer_main();
    bsp_display_unlock();
}

static void show_clients_screen(void)
{
    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Clients", on_clients_back, NULL);

    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_set_size(list, LV_PCT(100), 240 - 36);
    lv_obj_set_pos(list, 0, 36);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 6, 0);
    lv_obj_set_style_pad_row(list, 2, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    if (obs_network_count == 0) {
        lv_obj_t *lbl = lv_label_create(list);
        lv_label_set_text(lbl, "No clients found yet.\nLet sniffer run longer.");
        lv_obj_set_style_text_color(lbl, ui_muted_color(), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        return;
    }

    for (int i = 0; i < obs_network_count; i++) {
        observer_network_t *net = &obs_networks[i];

        lv_obj_t *net_lbl = lv_label_create(list);
        char net_txt[64];
        snprintf(net_txt, sizeof(net_txt), "%.32s (%d client%s)",
                 net->ssid[0] ? net->ssid : "(hidden)",
                 net->client_count,
                 net->client_count == 1 ? "" : "s");
        lv_label_set_text(net_lbl, net_txt);
        lv_obj_set_style_text_color(net_lbl, UI_ACCENT_CYAN, 0);
        lv_obj_set_style_text_font(net_lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_pad_top(net_lbl, 4, 0);

        for (int j = 0; j < net->client_count; j++) {
            lv_obj_t *c_lbl = lv_label_create(list);
            char c_txt[24];
            snprintf(c_txt, sizeof(c_txt), "  %s", net->clients[j]);
            lv_label_set_text(c_lbl, c_txt);
            lv_obj_set_style_text_color(c_lbl, UI_ACCENT_TEAL, 0);
            lv_obj_set_style_text_font(c_lbl, &lv_font_montserrat_10, 0);
        }
    }
}

/* ================================================================== */
/*  Main sniffer screen                                               */
/* ================================================================== */

static void on_stop(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Sniffer STOP");
    sniff_running = false;
    uart_set_line_callback(NULL);
    uart_send_command("stop");
    bsp_display_lock(0);
    show_attack_select_screen();
    bsp_display_unlock();
}

static void on_clients(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Show Clients");
    start_collect_clients();
}

static void on_karma(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Karma from sniffer");
    sniff_running = false;
    uart_set_line_callback(NULL);
    bsp_display_lock(0);
    show_karma_screen();
    bsp_display_unlock();
}

static void on_back(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Sniffer back");
    sniff_running = false;
    uart_set_line_callback(NULL);
    uart_send_command("stop");
    bsp_display_lock(0);
    show_attack_select_screen();
    bsp_display_unlock();
}

static void show_sniffer_main(void)
{
    sniff_running = true;
    uart_set_line_callback(sniff_uart_line_cb);

    int indices[MAX_NETWORKS];
    int sel_count = wifi_scan_get_selected(indices, MAX_NETWORKS);
    wifi_network_t *nets = wifi_scan_get_networks();

    lv_obj_t *scr = ui_screen_clear();

    char title[40];
    snprintf(title, sizeof(title), "Sniffer (%d net%s)", sel_count, sel_count == 1 ? "" : "s");
    ui_create_top_bar(scr, title, on_back, NULL);

    /* content area */
    lv_obj_t *content = lv_obj_create(scr);
    lv_obj_set_size(content, LV_PCT(100), 240 - 36 - 44);
    lv_obj_set_pos(content, 0, 36);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 6, 0);
    lv_obj_set_style_pad_row(content, 2, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);

    /* selected networks list */
    lv_obj_t *hdr = lv_label_create(content);
    lv_label_set_text(hdr, "Sniffing:");
    lv_obj_set_style_text_color(hdr, ui_muted_color(), 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_12, 0);

    for (int i = 0; i < sel_count && i < 6; i++) {
        wifi_network_t *net = &nets[indices[i]];
        lv_obj_t *lbl = lv_label_create(content);
        char txt[64];
        snprintf(txt, sizeof(txt), " " LV_SYMBOL_RIGHT " %s  ch%d  %ddBm",
                 net->ssid[0] ? net->ssid : "(hidden)",
                 net->channel, net->rssi);
        lv_label_set_text(lbl, txt);
        lv_obj_set_style_text_color(lbl, UI_ACCENT_GREEN, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
    }
    if (sel_count > 6) {
        lv_obj_t *more = lv_label_create(content);
        char mtxt[24];
        snprintf(mtxt, sizeof(mtxt), "  +%d more...", sel_count - 6);
        lv_label_set_text(more, mtxt);
        lv_obj_set_style_text_color(more, ui_muted_color(), 0);
        lv_obj_set_style_text_font(more, &lv_font_montserrat_10, 0);
    }

    /* packet counter */
    pkt_label = lv_label_create(content);
    char pkt_txt[48];
    snprintf(pkt_txt, sizeof(pkt_txt), "Packets: %d", pkt_count);
    lv_label_set_text(pkt_label, pkt_txt);
    lv_obj_set_style_text_color(pkt_label, ui_text_color(), 0);
    lv_obj_set_style_text_font(pkt_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_pad_top(pkt_label, 8, 0);

    /* bottom button row */
    lv_obj_t *btn_row = lv_obj_create(scr);
    lv_obj_set_size(btn_row, LV_PCT(100), 40);
    lv_obj_align(btn_row, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(btn_row, ui_panel_color(), 0);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_radius(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 4, 0);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    /* Karma button */
    lv_obj_t *karma_btn = lv_btn_create(btn_row);
    lv_obj_set_size(karma_btn, 90, 32);
    lv_obj_set_style_bg_color(karma_btn, UI_ACCENT_ORANGE, 0);
    lv_obj_set_style_radius(karma_btn, 8, 0);
    lv_obj_add_event_cb(karma_btn, on_karma, LV_EVENT_CLICKED, NULL);
    lv_obj_t *k_lbl = lv_label_create(karma_btn);
    lv_label_set_text(k_lbl, "Karma");
    lv_obj_set_style_text_color(k_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(k_lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(k_lbl);

    /* Clients button */
    lv_obj_t *clients_btn = lv_btn_create(btn_row);
    lv_obj_set_size(clients_btn, 90, 32);
    lv_obj_set_style_bg_color(clients_btn, UI_ACCENT_CYAN, 0);
    lv_obj_set_style_radius(clients_btn, 8, 0);
    lv_obj_add_event_cb(clients_btn, on_clients, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl_lbl = lv_label_create(clients_btn);
    lv_label_set_text(cl_lbl, "Clients");
    lv_obj_set_style_text_color(cl_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(cl_lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(cl_lbl);

    /* Stop button */
    lv_obj_t *stop_btn = lv_btn_create(btn_row);
    lv_obj_set_size(stop_btn, 90, 32);
    lv_obj_set_style_bg_color(stop_btn, UI_ACCENT_RED, 0);
    lv_obj_set_style_radius(stop_btn, 8, 0);
    lv_obj_add_event_cb(stop_btn, on_stop, LV_EVENT_CLICKED, NULL);
    lv_obj_t *s_lbl = lv_label_create(stop_btn);
    lv_label_set_text(s_lbl, LV_SYMBOL_CLOSE " Stop");
    lv_obj_set_style_text_color(s_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(s_lbl);
}

/* ================================================================== */
/*  Entry point -- called from attack_select_screen                   */
/* ================================================================== */

void show_sniffer_screen(void)
{
    send_select_networks();
    uart_send_command("start_sniffer");
    ESP_LOGI(TAG, "Sniffer started");

    pkt_count = 0;
    sniff_running = true;

    show_sniffer_main();
}
