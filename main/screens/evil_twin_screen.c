#include "evil_twin_screen.h"
#include "attack_select_screen.h"
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

static const char *TAG = "evil_twin";

/* ---- SD file list state ---- */
#define MAX_SD_FILES 32

typedef struct {
    int  number;
    char filename[64];
} sd_file_t;

static sd_file_t sd_files[MAX_SD_FILES];
static int sd_file_count = 0;
static bool sd_collecting = false;
static esp_timer_handle_t sd_timeout_timer = NULL;

/* ---- Evil Twin running state ---- */
static char et_ssid[33]     = {0};
static char et_password[64] = {0};
static bool et_password_ok  = false;
static bool et_shutdown     = false;

/* array position (in wifi scan list) of the network chosen as the Evil Twin AP */
static int et_primary_pos = -1;

static lv_obj_t *et_status_lbl = NULL;

/* ---- forward declarations ---- */
static void show_et_network_picker(void);
static void on_et_network_selected(lv_event_t *e);
static void send_select_networks_with_primary(int primary_pos);
static void show_sd_loading_screen(void);
static void show_html_picker(void);
static void show_running_screen(void);
static void show_success_screen(void);
static void et_uart_line_cb(const char *line);

/* ================================================================== */
/*  Phase 1: list_sd -- collect SD file list                          */
/* ================================================================== */

static bool parse_sd_line(const char *line, sd_file_t *f)
{
    if (!isdigit((unsigned char)line[0])) return false;
    int num = 0;
    const char *p = line;
    while (isdigit((unsigned char)*p)) {
        num = num * 10 + (*p - '0');
        p++;
    }
    while (*p == ' ') p++;
    if (*p == '\0') return false;

    f->number = num;
    strncpy(f->filename, p, sizeof(f->filename) - 1);
    f->filename[sizeof(f->filename) - 1] = '\0';
    /* trim trailing whitespace */
    int len = strlen(f->filename);
    while (len > 0 && (f->filename[len-1] == ' ' || f->filename[len-1] == '\r'))
        f->filename[--len] = '\0';
    return len > 0;
}

static void sd_timeout_cb(void *arg)
{
    (void)arg;
    if (!sd_collecting) return;
    sd_collecting = false;
    uart_set_line_callback(NULL);

    ESP_LOGI(TAG, "SD list complete, %d files", sd_file_count);

    bsp_display_lock(0);
    show_html_picker();
    bsp_display_unlock();
}

static void sd_line_cb(const char *line)
{
    if (!sd_collecting) return;

    sd_file_t f;
    if (parse_sd_line(line, &f) && sd_file_count < MAX_SD_FILES) {
        sd_files[sd_file_count++] = f;
        /* restart timeout -- more lines may follow */
        esp_timer_stop(sd_timeout_timer);
        esp_timer_start_once(sd_timeout_timer, 500000); /* 500 ms */
    }
}

static void start_list_sd(void)
{
    sd_file_count = 0;
    sd_collecting = true;

    if (!sd_timeout_timer) {
        esp_timer_create_args_t args = {
            .callback = sd_timeout_cb,
            .name = "sd_timeout",
        };
        esp_timer_create(&args, &sd_timeout_timer);
    }

    uart_set_line_callback(sd_line_cb);
    /* start timeout in case no lines arrive at all */
    esp_timer_start_once(sd_timeout_timer, 2000000); /* 2 s */
    uart_send_command("list_sd");
}

/* ================================================================== */
/*  Phase 2: HTML portal picker screen                                */
/* ================================================================== */

static int selected_html_idx = -1;

static void on_html_selected(lv_event_t *e)
{
    selected_html_idx = (int)(intptr_t)lv_event_get_user_data(e);
    ESP_LOGI(TAG, "HTML selected: %d -> %s",
             sd_files[selected_html_idx].number,
             sd_files[selected_html_idx].filename);

    /* send select_html command */
    char cmd[80];
    snprintf(cmd, sizeof(cmd), "select_html %d", sd_files[selected_html_idx].number);
    uart_send_command(cmd);

    /* start evil twin attack */
    uart_send_command("start_evil_twin");

    /* set up UART monitoring */
    et_ssid[0] = '\0';
    et_password[0] = '\0';
    et_password_ok = false;
    et_shutdown = false;
    uart_set_line_callback(et_uart_line_cb);

    bsp_display_lock(0);
    show_running_screen();
    bsp_display_unlock();
}

static void on_picker_back(lv_event_t *e)
{
    (void)e;
    sd_collecting = false;
    uart_set_line_callback(NULL);
    if (sd_timeout_timer)
        esp_timer_stop(sd_timeout_timer);
    bsp_display_lock(0);
    show_attack_select_screen();
    bsp_display_unlock();
}

static void show_html_picker(void)
{
    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Select HTML Portal", on_picker_back, NULL);

    if (sd_file_count == 0) {
        lv_obj_t *lbl = lv_label_create(scr);
        lv_label_set_text(lbl, "No HTML files found on SD card.");
        lv_obj_set_style_text_color(lbl, ui_muted_color(), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_center(lbl);
        return;
    }

    /* scrollable list of HTML files */
    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_set_size(list, LV_PCT(100), 240 - 36);
    lv_obj_set_pos(list, 0, 36);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 6, 0);
    lv_obj_set_style_pad_row(list, 4, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    for (int i = 0; i < sd_file_count; i++) {
        lv_obj_t *btn = lv_btn_create(list);
        lv_obj_set_size(btn, LV_PCT(100), 34);
        lv_obj_set_style_bg_color(btn, ui_card_color(), 0);
        lv_obj_set_style_bg_color(btn, UI_ACCENT_PURPLE, LV_STATE_PRESSED);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_pad_hor(btn, 8, 0);
        lv_obj_add_event_cb(btn, on_html_selected, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);

        lv_obj_t *lbl = lv_label_create(btn);
        char display[72];
        snprintf(display, sizeof(display), "%d. %s",
                 sd_files[i].number, sd_files[i].filename);
        lv_label_set_text(lbl, display);
        lv_obj_set_style_text_color(lbl, ui_text_color(), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
    }
}

/* ================================================================== */
/*  Phase 3: Running screen -- monitor UART                           */
/* ================================================================== */

static void on_running_stop(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Evil Twin manual stop");
    uart_send_command("stop");
    uart_set_line_callback(NULL);
    bsp_display_lock(0);
    show_attack_select_screen();
    bsp_display_unlock();
}

static void show_running_screen(void)
{
    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Evil Twin Running", NULL, NULL);

    /* show targeted networks */
    int indices[MAX_NETWORKS];
    int sel_count = wifi_scan_get_selected(indices, MAX_NETWORKS);
    wifi_network_t *nets = wifi_scan_get_networks();

    lv_obj_t *info_area = lv_obj_create(scr);
    lv_obj_set_size(info_area, LV_PCT(100), 130);
    lv_obj_set_pos(info_area, 0, 36);
    lv_obj_set_style_bg_opa(info_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(info_area, 0, 0);
    lv_obj_set_style_pad_all(info_area, 8, 0);
    lv_obj_set_style_pad_row(info_area, 2, 0);
    lv_obj_set_flex_flow(info_area, LV_FLEX_FLOW_COLUMN);

    if (sel_count > 0) {
        /* primary SSID (first selected) */
        wifi_network_t *primary = &nets[indices[0]];
        lv_obj_t *lbl_main = lv_label_create(info_area);
        char main_txt[64];
        snprintf(main_txt, sizeof(main_txt), "AP: %s",
                 primary->ssid[0] ? primary->ssid : "(hidden)");
        lv_label_set_text(lbl_main, main_txt);
        lv_obj_set_style_text_color(lbl_main, UI_ACCENT_PURPLE, 0);
        lv_obj_set_style_text_font(lbl_main, &lv_font_montserrat_14, 0);

        /* other SSIDs */
        for (int i = 1; i < sel_count; i++) {
            wifi_network_t *net = &nets[indices[i]];
            lv_obj_t *lbl = lv_label_create(info_area);
            char txt[64];
            snprintf(txt, sizeof(txt), "  + %s  ch%d",
                     net->ssid[0] ? net->ssid : "(hidden)", net->channel);
            lv_label_set_text(lbl, txt);
            lv_obj_set_style_text_color(lbl, ui_muted_color(), 0);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
        }
    }

    /* status label -- updated by UART callback */
    et_status_lbl = lv_label_create(info_area);
    lv_label_set_text(et_status_lbl, "Waiting for client...");
    lv_obj_set_style_text_color(et_status_lbl, UI_ACCENT_CYAN, 0);
    lv_obj_set_style_text_font(et_status_lbl, &lv_font_montserrat_12, 0);

    /* stop button */
    lv_obj_t *btn = lv_btn_create(scr);
    lv_obj_set_size(btn, 140, 36);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(btn, UI_ACCENT_RED, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_add_event_cb(btn, on_running_stop, LV_EVENT_CLICKED, NULL);
    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, LV_SYMBOL_CLOSE " Stop");
    lv_obj_set_style_text_color(btn_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(btn_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(btn_lbl);
}

/* ---- UART line callback during Evil Twin attack ---- */
static void et_uart_line_cb(const char *line)
{
    /* Client connected */
    if (strstr(line, "Client connected to portal")) {
        bsp_display_lock(0);
        if (et_status_lbl)
            lv_label_set_text(et_status_lbl, "Client connected!");
        bsp_display_unlock();
    }

    /* Password received: Wi-Fi: connected to SSID='X' with password='Y' */
    const char *pw_marker = "Wi-Fi: connected to SSID='";
    const char *pw_pos = strstr(line, pw_marker);
    if (pw_pos) {
        pw_pos += strlen(pw_marker);
        const char *ssid_end = strchr(pw_pos, '\'');
        if (ssid_end) {
            int ssid_len = ssid_end - pw_pos;
            if (ssid_len > (int)sizeof(et_ssid) - 1) ssid_len = sizeof(et_ssid) - 1;
            memcpy(et_ssid, pw_pos, ssid_len);
            et_ssid[ssid_len] = '\0';

            const char *pass_marker = "with password='";
            const char *pass_pos = strstr(ssid_end, pass_marker);
            if (pass_pos) {
                pass_pos += strlen(pass_marker);
                const char *pass_end = strchr(pass_pos, '\'');
                if (pass_end) {
                    int pass_len = pass_end - pass_pos;
                    if (pass_len > (int)sizeof(et_password) - 1)
                        pass_len = sizeof(et_password) - 1;
                    memcpy(et_password, pass_pos, pass_len);
                    et_password[pass_len] = '\0';
                    ESP_LOGI(TAG, "Captured: SSID='%s' pass='%s'", et_ssid, et_password);
                }
            }
        }

        bsp_display_lock(0);
        if (et_status_lbl) {
            char txt[96];
            snprintf(txt, sizeof(txt), "Password: %s", et_password);
            lv_label_set_text(et_status_lbl, txt);
        }
        bsp_display_unlock();
    }

    /* Password verified */
    if (strstr(line, "Password verified")) {
        et_password_ok = true;
        bsp_display_lock(0);
        if (et_status_lbl)
            lv_label_set_text(et_status_lbl, "Password verified!");
        bsp_display_unlock();
    }

    /* Evil Twin shut down -- attack finished */
    if (strstr(line, "Evil Twin portal shut down")) {
        et_shutdown = true;
        uart_set_line_callback(NULL);
        ESP_LOGI(TAG, "Evil Twin finished. SSID=%s pass=%s ok=%d",
                 et_ssid, et_password, et_password_ok);
        bsp_display_lock(0);
        show_success_screen();
        bsp_display_unlock();
    }
}

/* ================================================================== */
/*  Phase 4: Success screen                                           */
/* ================================================================== */

static void on_success_close(lv_event_t *e)
{
    (void)e;
    bsp_display_lock(0);
    show_attack_select_screen();
    bsp_display_unlock();
}

static void show_success_screen(void)
{
    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Evil Twin Result", NULL, NULL);

    lv_obj_t *center = lv_obj_create(scr);
    lv_obj_set_size(center, LV_PCT(90), LV_SIZE_CONTENT);
    lv_obj_center(center);
    lv_obj_set_style_bg_color(center, ui_card_color(), 0);
    lv_obj_set_style_bg_opa(center, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(center, 12, 0);
    lv_obj_set_style_border_color(center, UI_ACCENT_GREEN, 0);
    lv_obj_set_style_border_width(center, 2, 0);
    lv_obj_set_style_pad_all(center, 14, 0);
    lv_obj_set_flex_flow(center, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(center, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(center, 6, 0);
    lv_obj_clear_flag(center, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = lv_label_create(center);
    lv_label_set_text(icon, LV_SYMBOL_OK);
    lv_obj_set_style_text_color(icon, UI_ACCENT_GREEN, 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, 0);

    lv_obj_t *title = lv_label_create(center);
    lv_label_set_text(title, et_password_ok ? "SUCCESS" : "Attack Finished");
    lv_obj_set_style_text_color(title, UI_ACCENT_GREEN, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);

    lv_obj_t *ssid_lbl = lv_label_create(center);
    char ssid_txt[64];
    snprintf(ssid_txt, sizeof(ssid_txt), "SSID: %s", et_ssid);
    lv_label_set_text(ssid_lbl, ssid_txt);
    lv_obj_set_style_text_color(ssid_lbl, ui_text_color(), 0);
    lv_obj_set_style_text_font(ssid_lbl, &lv_font_montserrat_14, 0);

    if (et_password[0]) {
        lv_obj_t *pass_lbl = lv_label_create(center);
        char pass_txt[80];
        snprintf(pass_txt, sizeof(pass_txt), "Password: %s", et_password);
        lv_label_set_text(pass_lbl, pass_txt);
        lv_obj_set_style_text_color(pass_lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(pass_lbl, &lv_font_montserrat_16, 0);
    }

    lv_obj_t *btn = lv_btn_create(center);
    lv_obj_set_size(btn, 120, 34);
    lv_obj_set_style_bg_color(btn, UI_ACCENT_BLUE, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_add_event_cb(btn, on_success_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, "Close");
    lv_obj_set_style_text_color(btn_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(btn_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(btn_lbl);
}

/* ================================================================== */
/*  Phase 0: Evil Twin Network picker                                 */
/*  Pick exactly one of the previously checkbox-selected networks as  */
/*  the cloned AP. Its index is sent first in select_networks; the     */
/*  rest of the selected networks follow (deauth targets).             */
/* ================================================================== */

/* Build select_networks with `primary_pos` first, then the remaining
 * checkbox-selected networks, skipping the primary so indices are not
 * duplicated. Indices are 1-based (nets[].index), same as elsewhere. */
static void send_select_networks_with_primary(int primary_pos)
{
    int indices[MAX_NETWORKS];
    int count = wifi_scan_get_selected(indices, MAX_NETWORKS);
    wifi_network_t *nets = wifi_scan_get_networks();
    if (!nets || primary_pos < 0) return;

    char cmd[256] = "select_networks";
    int pos = strlen(cmd);
    pos += snprintf(cmd + pos, sizeof(cmd) - pos, " %d", nets[primary_pos].index);
    for (int i = 0; i < count; i++) {
        if (indices[i] == primary_pos) continue;   /* no duplicate */
        pos += snprintf(cmd + pos, sizeof(cmd) - pos, " %d", nets[indices[i]].index);
    }
    uart_send_command(cmd);
}

/* Loading screen shown while list_sd runs (shared with entry point). */
static void show_sd_loading_screen(void)
{
    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Evil Twin", on_picker_back, NULL);

    lv_obj_t *spinner = lv_spinner_create(scr);
    lv_obj_set_size(spinner, 40, 40);
    lv_obj_center(spinner);
    lv_obj_set_y(spinner, 90);

    lv_obj_t *lbl = lv_label_create(scr);
    lv_label_set_text(lbl, "Loading SD card files...");
    lv_obj_set_style_text_color(lbl, ui_text_color(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 45);
}

static void on_et_network_selected(lv_event_t *e)
{
    et_primary_pos = (int)(intptr_t)lv_event_get_user_data(e);
    wifi_network_t *nets = wifi_scan_get_networks();
    if (nets)
        ESP_LOGI(TAG, "Evil Twin AP: pos=%d index=%d ssid=%s",
                 et_primary_pos, nets[et_primary_pos].index,
                 nets[et_primary_pos].ssid);

    /* primary first, others follow */
    send_select_networks_with_primary(et_primary_pos);

    /* proceed to the HTML portal picker (via list_sd) */
    show_sd_loading_screen();
    start_list_sd();
}

static void show_et_network_picker(void)
{
    int indices[MAX_NETWORKS];
    int count = wifi_scan_get_selected(indices, MAX_NETWORKS);
    wifi_network_t *nets = wifi_scan_get_networks();

    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Evil Twin Network", on_picker_back, NULL);

    if (count == 0 || !nets) {
        lv_obj_t *lbl = lv_label_create(scr);
        lv_label_set_text(lbl, "No networks selected.");
        lv_obj_set_style_text_color(lbl, ui_muted_color(), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_center(lbl);
        return;
    }

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "Pick the network to clone:");
    lv_obj_set_style_text_color(hint, ui_muted_color(), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_10, 0);
    lv_obj_set_pos(hint, 8, 38);

    /* scrollable list of selected networks */
    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_set_size(list, LV_PCT(100), 240 - 54);
    lv_obj_set_pos(list, 0, 54);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 6, 0);
    lv_obj_set_style_pad_row(list, 4, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    for (int i = 0; i < count; i++) {
        int net_pos = indices[i];
        wifi_network_t *net = &nets[net_pos];

        lv_obj_t *btn = lv_btn_create(list);
        lv_obj_set_size(btn, LV_PCT(100), 34);
        lv_obj_set_style_bg_color(btn, ui_card_color(), 0);
        lv_obj_set_style_bg_color(btn, UI_ACCENT_PURPLE, LV_STATE_PRESSED);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_pad_hor(btn, 8, 0);
        lv_obj_add_event_cb(btn, on_et_network_selected, LV_EVENT_CLICKED,
                            (void *)(intptr_t)net_pos);

        lv_obj_t *lbl = lv_label_create(btn);
        char display[80];
        snprintf(display, sizeof(display), "%s  ch%d",
                 net->ssid[0] ? net->ssid : "(hidden)", net->channel);
        lv_label_set_text(lbl, display);
        lv_obj_set_style_text_color(lbl, ui_text_color(), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
    }
}

/* ================================================================== */
/*  Entry point: called from attack_select_screen                     */
/* ================================================================== */

void show_evil_twin_screen(void)
{
    et_primary_pos = -1;
    /* Step 1: let the user choose which selected network to clone.
     * select_networks is sent once that choice is made (with the chosen
     * AP first), then the HTML portal picker follows. */
    show_et_network_picker();
}
