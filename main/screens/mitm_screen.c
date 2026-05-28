#include "mitm_screen.h"
#include "attack_select_screen.h"
#include "wifi_scan_screen.h"
#include "wifi_connect_helper.h"
#include "ui_helpers.h"
#include "uart_handler.h"
#include <stdio.h>
#include <string.h>

static wifi_network_t s_net;
static char s_password[64];
static lv_obj_t *s_status_lbl = NULL;
static bool s_running = false;

static wifi_network_t *get_single_net(wifi_network_t *out)
{
    int idx[MAX_NETWORKS];
    if (wifi_scan_get_selected(idx, MAX_NETWORKS) != 1) return NULL;
    wifi_network_t *nets = wifi_scan_get_networks();
    if (!nets) return NULL;
    *out = nets[idx[0]];
    return out;
}

static void mitm_line_cb(const char *line)
{
    if (!s_running || !line || !s_status_lbl) return;
    if (strstr(line, "PCAP net capture started") || strstr(line, "PCAP saved:")) {
        if (ui_display_lock_wait()) {
            lv_label_set_text(s_status_lbl, line);
            ui_display_unlock_safe();
        }
    }
}

static void on_back(lv_event_t *e)
{
    (void)e;
    s_running = false;
    uart_send_command("stop");
    uart_set_line_callback(NULL);
    show_attack_select_screen();
}

static void on_stop(lv_event_t *e)
{
    (void)e;
    on_back(NULL);
}

static void show_running_screen(void)
{
    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "MITM PCAP", on_back, NULL);

    s_status_lbl = lv_label_create(scr);
    lv_label_set_text(s_status_lbl, "Capturing network traffic...");
    lv_obj_set_style_text_color(s_status_lbl, UI_ACCENT_TEAL, 0);
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(s_status_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_status_lbl, 280);
    lv_obj_align(s_status_lbl, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *btn = lv_btn_create(scr);
    lv_obj_set_size(btn, 120, 34);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_bg_color(btn, UI_ACCENT_RED, 0);
    lv_obj_add_event_cb(btn, on_stop, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, LV_SYMBOL_CLOSE " Stop");
    lv_obj_center(bl);

    s_running = true;
    uart_set_line_callback(mitm_line_cb);
    uart_send_command("start_pcap net");
}

static void on_connect_done(bool success, void *unused)
{
    (void)unused;
    if (!success) {
        if (ui_display_lock_wait()) {
            lv_obj_t *scr = ui_screen_clear();
            ui_create_top_bar(scr, "MITM PCAP", on_back, NULL);
            lv_obj_t *msg = lv_label_create(scr);
            lv_label_set_text(msg, "WiFi connect failed.");
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
    ui_show_text_input_popup("WiFi Password", "", 63, UI_ACCENT_TEAL,
                             on_password_confirm, NULL, NULL);
}

void show_mitm_screen(void)
{
    s_password[0] = '\0';
    s_running = false;

    if (!get_single_net(&s_net)) {
        show_attack_select_screen();
        return;
    }

    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "MITM PCAP", on_back, NULL);
    lv_obj_t *lbl = lv_label_create(scr);
    lv_label_set_text(lbl, "Connecting...");
    lv_obj_center(lbl);
    begin_connect();
}
