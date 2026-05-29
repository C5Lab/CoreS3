#include "arp_poison_screen.h"
#include "attack_select_screen.h"
#include "wifi_scan_screen.h"
#include "wifi_connect_helper.h"
#include "ui_helpers.h"
#include "uart_handler.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "arp";

static wifi_network_t s_net;
static char s_password[64];
static lan_host_t s_hosts[WIFI_MAX_HOSTS];
static int s_host_count;
static lv_obj_t *s_status_lbl = NULL;
static lv_obj_t *s_list = NULL;
static bool s_attacking = false;

static wifi_network_t *get_single_net(wifi_network_t *out)
{
    int idx[MAX_NETWORKS];
    if (wifi_scan_get_selected(idx, MAX_NETWORKS) != 1) return NULL;
    wifi_network_t *nets = wifi_scan_get_networks();
    if (!nets) return NULL;
    *out = nets[idx[0]];
    return out;
}

static void show_host_list(void);

static void on_back(lv_event_t *e)
{
    (void)e;
    uart_send_command("stop");
    uart_set_line_callback(NULL);
    s_attacking = false;
    show_attack_select_screen();
}

static void start_arp_on_host(int host_idx)
{
    if (!ui_red_team_enabled()) {
        ESP_LOGW(TAG, "ARP blocked - Red Team off");
        return;
    }
    if (host_idx < 0 || host_idx >= s_host_count) return;

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "arp_ban %s %s",
             s_hosts[host_idx].mac, s_hosts[host_idx].ip);
    uart_send_command(cmd);
    s_attacking = true;

    if (s_status_lbl) {
        char buf[80];
        snprintf(buf, sizeof(buf), "ARP active: %s", s_hosts[host_idx].ip);
        lv_label_set_text(s_status_lbl, buf);
        lv_obj_set_style_text_color(s_status_lbl, UI_ACCENT_PURPLE, 0);
    }
}

static void on_host_click(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    start_arp_on_host(idx);
}

static void show_host_list(void)
{
    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "ARP Poison", on_back, NULL);

    s_status_lbl = lv_label_create(scr);
    lv_label_set_text(s_status_lbl, "Tap a host to poison:");
    lv_obj_set_style_text_color(s_status_lbl, ui_muted_color(), 0);
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(s_status_lbl, 8, 40);

    s_list = lv_obj_create(scr);
    lv_obj_set_size(s_list, LV_PCT(100), 240 - 40 - 8);
    lv_obj_set_pos(s_list, 0, 56);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 4, 0);
    lv_obj_set_style_pad_row(s_list, 3, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);

    for (int i = 0; i < s_host_count; i++) {
        lv_obj_t *btn = lv_btn_create(s_list);
        lv_obj_set_size(btn, LV_PCT(100), 32);
        lv_obj_set_style_bg_color(btn, ui_card_color(), 0);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_add_event_cb(btn, on_host_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *lbl = lv_label_create(btn);
        char line[40];
        snprintf(line, sizeof(line), "%.15s  %.17s",
                 s_hosts[i].ip, s_hosts[i].mac);
        lv_label_set_text(lbl, line);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(lbl, ui_text_color(), 0);
    }
}

static void hosts_done_lvgl_cb(void *arg)
{
    (void)arg;
    if (s_host_count > 0) {
        show_host_list();
    } else {
        lv_obj_t *scr = ui_screen_clear();
        s_status_lbl = NULL;
        ui_create_top_bar(scr, "ARP Poison", on_back, NULL);
        lv_obj_t *msg = lv_label_create(scr);
        lv_label_set_text(msg, "No hosts found.");
        lv_obj_center(msg);
    }
}

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
            ui_create_top_bar(scr, "ARP Poison", on_back, NULL);
            lv_obj_t *msg = lv_label_create(scr);
            lv_label_set_text(msg, "WiFi connect failed.");
            lv_obj_set_style_text_color(msg, UI_ACCENT_RED, 0);
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

    xTaskCreate(hosts_collect_task, "arp_hosts", 4096, NULL, 5, NULL);
}

static void on_password_confirm(const char *text, void *unused)
{
    (void)unused;
    if (text && text[0])
        snprintf(s_password, sizeof(s_password), "%s", text);
    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "ARP Poison", on_back, NULL);
    s_status_lbl = lv_label_create(scr);
    lv_label_set_text(s_status_lbl, "Connecting...");
    lv_obj_center(s_status_lbl);
    wifi_connect_async(&s_net, s_password[0] ? s_password : NULL,
                       on_connect_done, NULL);
}

static void begin_connect(void)
{
    if (wifi_network_is_open(&s_net)) {
        lv_obj_t *scr = ui_screen_clear();
        ui_create_top_bar(scr, "ARP Poison", on_back, NULL);
        s_status_lbl = lv_label_create(scr);
        lv_label_set_text(s_status_lbl, "Connecting...");
        lv_obj_center(s_status_lbl);
        wifi_connect_async(&s_net, NULL, on_connect_done, NULL);
        return;
    }

    char evil_pass[64] = {0};
    if (wifi_lookup_evil_password(s_net.ssid, evil_pass, sizeof(evil_pass))) {
        snprintf(s_password, sizeof(s_password), "%s", evil_pass);
        lv_obj_t *scr = ui_screen_clear();
        ui_create_top_bar(scr, "ARP Poison", on_back, NULL);
        s_status_lbl = lv_label_create(scr);
        lv_label_set_text(s_status_lbl, "Connecting...");
        lv_obj_center(s_status_lbl);
        wifi_connect_async(&s_net, s_password, on_connect_done, NULL);
        return;
    }

    ui_show_text_input_popup("WiFi Password", "", 63, UI_ACCENT_PURPLE,
                             on_password_confirm, NULL, NULL);
}

void show_arp_poison_screen(void)
{
    s_password[0] = '\0';
    s_host_count = 0;
    s_attacking = false;

    if (!get_single_net(&s_net)) {
        ESP_LOGW(TAG, "Need exactly 1 network");
        show_attack_select_screen();
        return;
    }

    begin_connect();
}
