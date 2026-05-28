#include "rogue_ap_screen.h"
#include "attack_select_screen.h"
#include "wifi_scan_screen.h"
#include "wifi_connect_helper.h"
#include "ui_helpers.h"
#include "uart_handler.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#define MAX_SD_FILES 32

typedef struct {
    int  number;
    char filename[64];
} sd_file_t;

static sd_file_t s_sd_files[MAX_SD_FILES];
static int s_sd_count = 0;
static bool s_sd_collecting = false;
static esp_timer_handle_t s_sd_timer = NULL;

static wifi_network_t s_net;
static char s_ap_password[64] = "password123";
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

static bool parse_sd_line(const char *line, sd_file_t *f)
{
    if (!line || !isdigit((unsigned char)line[0])) return false;
    int num = 0;
    const char *p = line;
    while (isdigit((unsigned char)*p)) { num = num * 10 + (*p - '0'); p++; }
    while (*p == ' ') p++;
    if (*p == '\0') return false;
    f->number = num;
    strncpy(f->filename, p, sizeof(f->filename) - 1);
    return f->filename[0] != '\0';
}

static void show_html_picker(void);

static void on_back(lv_event_t *e)
{
    (void)e;
    s_running = false;
    s_sd_collecting = false;
    uart_send_command("stop");
    uart_set_line_callback(NULL);
    if (s_sd_timer) esp_timer_stop(s_sd_timer);
    show_attack_select_screen();
}

static void rogue_line_cb(const char *line)
{
    if (!s_running || !line || !s_status_lbl) return;
    if (strstr(line, "Client connected") || strstr(line, "Portal password") ||
        strstr(line, "Password verified")) {
        if (ui_display_lock_wait()) {
            lv_label_set_text(s_status_lbl, line);
            ui_display_unlock_safe();
        }
    }
}

static void sd_timeout_cb(void *arg)
{
    (void)arg;
    if (!s_sd_collecting) return;
    s_sd_collecting = false;
    uart_set_line_callback(NULL);
    if (ui_display_lock_wait()) {
        show_html_picker();
        ui_display_unlock_safe();
    }
}

static void sd_line_cb(const char *line)
{
    if (!s_sd_collecting) return;
    sd_file_t f;
    if (parse_sd_line(line, &f) && s_sd_count < MAX_SD_FILES) {
        s_sd_files[s_sd_count++] = f;
        if (s_sd_timer) {
            esp_timer_stop(s_sd_timer);
            esp_timer_start_once(s_sd_timer, 500000);
        }
    }
}

static void start_list_sd(void)
{
    s_sd_count = 0;
    s_sd_collecting = true;
    if (!s_sd_timer) {
        esp_timer_create_args_t args = { .callback = sd_timeout_cb, .name = "rogue_sd" };
        esp_timer_create(&args, &s_sd_timer);
    }
    uart_set_line_callback(sd_line_cb);
    esp_timer_start_once(s_sd_timer, 2000000);
    uart_send_command("list_sd");
}

static void show_running_screen(void)
{
    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Rogue AP", on_back, NULL);

    s_status_lbl = lv_label_create(scr);
    lv_label_set_text(s_status_lbl, "Rogue AP running...");
    lv_obj_set_style_text_color(s_status_lbl, UI_ACCENT_CYAN, 0);
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_width(s_status_lbl, 280);
    lv_label_set_long_mode(s_status_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_status_lbl, LV_ALIGN_CENTER, 0, 0);

    s_running = true;
    uart_set_line_callback(rogue_line_cb);
}

static void on_html_selected(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_sd_count) return;

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "select_html %d", s_sd_files[idx].number);
    uart_send_command(cmd);

    send_select_networks();

    char esc_ssid[67];
    char esc_pass[131];
    wifi_escape_quoted_arg(s_net.ssid, esc_ssid, sizeof(esc_ssid));
    wifi_escape_quoted_arg(s_ap_password, esc_pass, sizeof(esc_pass));
    snprintf(cmd, sizeof(cmd), "start_rogueap \"%.65s\" \"%.129s\"",
             esc_ssid, esc_pass);
    uart_send_command(cmd);

    show_running_screen();
}

static void show_html_picker(void)
{
    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Rogue AP HTML", on_back, NULL);

    if (s_sd_count == 0) {
        lv_obj_t *lbl = lv_label_create(scr);
        lv_label_set_text(lbl, "No HTML files on SD card.");
        lv_obj_center(lbl);
        return;
    }

    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_set_size(list, LV_PCT(100), 240 - 36);
    lv_obj_set_pos(list, 0, 36);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 3, 0);

    for (int i = 0; i < s_sd_count; i++) {
        lv_obj_t *btn = lv_btn_create(list);
        lv_obj_set_size(btn, LV_PCT(100), 32);
        lv_obj_add_event_cb(btn, on_html_selected, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *lbl = lv_label_create(btn);
        char txt[72];
        snprintf(txt, sizeof(txt), "%d. %s", s_sd_files[i].number, s_sd_files[i].filename);
        lv_label_set_text(lbl, txt);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
    }
}

static void on_pass_confirm(const char *text, void *unused)
{
    (void)unused;
    if (text && text[0])
        strncpy(s_ap_password, text, sizeof(s_ap_password) - 1);
    start_list_sd();
}

void show_rogue_ap_screen(void)
{
    s_running = false;
    if (!get_single_net(&s_net)) {
        show_attack_select_screen();
        return;
    }

    ui_show_text_input_popup("AP Password", s_ap_password, 63, UI_ACCENT_CYAN,
                             on_pass_confirm, NULL, NULL);
}
