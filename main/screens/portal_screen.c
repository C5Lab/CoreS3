#include "portal_screen.h"
#include "global_attacks_screen.h"
#include "ui_helpers.h"
#include "uart_handler.h"
#include "cardkb.h"
#include "bsp/m5stack_core_s3.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static const char *TAG = "portal";

/* ================================================================== */
/*  SD file list state                                                 */
/* ================================================================== */

#define MAX_SD_FILES  32

typedef struct {
    int  number;
    char filename[64];
} portal_sd_file_t;

static portal_sd_file_t sd_files[MAX_SD_FILES];
static int  sd_file_count   = 0;
static bool sd_collecting   = false;
static esp_timer_handle_t sd_timeout_timer = NULL;

static int  selected_html   = -1;
static char portal_ssid[33] = {0};

/* ---- forward declarations ---- */
static void show_html_picker(void);
static void show_ssid_input(void);
static void show_running_screen(void);

/* ================================================================== */
/*  Phase 1: list_sd                                                   */
/* ================================================================== */

static bool parse_sd_line(const char *line, portal_sd_file_t *f)
{
    if (!isdigit((unsigned char)line[0])) return false;
    int num = 0;
    const char *p = line;
    while (isdigit((unsigned char)*p)) { num = num * 10 + (*p - '0'); p++; }
    while (*p == ' ') p++;
    if (*p == '\0') return false;

    f->number = num;
    strncpy(f->filename, p, sizeof(f->filename) - 1);
    f->filename[sizeof(f->filename) - 1] = '\0';
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
    portal_sd_file_t f;
    if (parse_sd_line(line, &f) && sd_file_count < MAX_SD_FILES) {
        sd_files[sd_file_count++] = f;
        esp_timer_stop(sd_timeout_timer);
        esp_timer_start_once(sd_timeout_timer, 500000);
    }
}

static void on_loading_back(lv_event_t *e)
{
    (void)e;
    sd_collecting = false;
    uart_set_line_callback(NULL);
    if (sd_timeout_timer) esp_timer_stop(sd_timeout_timer);
    bsp_display_lock(0);
    show_global_attacks_screen();
    bsp_display_unlock();
}

static void start_list_sd(void)
{
    sd_file_count = 0;
    sd_collecting = true;

    if (!sd_timeout_timer) {
        esp_timer_create_args_t args = {
            .callback = sd_timeout_cb,
            .name = "portal_sd_to",
        };
        esp_timer_create(&args, &sd_timeout_timer);
    }

    uart_set_line_callback(sd_line_cb);
    esp_timer_start_once(sd_timeout_timer, 2000000);
    uart_send_command("list_sd");
}

/* ================================================================== */
/*  Phase 2: HTML picker                                               */
/* ================================================================== */

static void on_html_selected(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    selected_html = idx;
    ESP_LOGI(TAG, "HTML selected: %d -> %s",
             sd_files[idx].number, sd_files[idx].filename);

    bsp_display_lock(0);
    show_ssid_input();
    bsp_display_unlock();
}

static void on_picker_back(lv_event_t *e)
{
    (void)e;
    bsp_display_lock(0);
    show_global_attacks_screen();
    bsp_display_unlock();
}

static void show_html_picker(void)
{
    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Select HTML Portal", on_picker_back, NULL);

    if (sd_file_count == 0) {
        lv_obj_t *lbl = lv_label_create(scr);
        lv_label_set_text(lbl, "No HTML files on SD.");
        lv_obj_set_style_text_color(lbl, ui_muted_color(), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_center(lbl);
        return;
    }

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
/*  Phase 3: SSID input                                                */
/* ================================================================== */

static lv_obj_t  *ssid_textarea = NULL;
static lv_timer_t *cardkb_ssid_timer = NULL;

static void stop_cardkb_timer(void)
{
    if (cardkb_ssid_timer) {
        lv_timer_del(cardkb_ssid_timer);
        cardkb_ssid_timer = NULL;
    }
}

static void on_ssid_cancel(lv_event_t *e)
{
    (void)e;
    stop_cardkb_timer();
    bsp_display_lock(0);
    show_html_picker();
    bsp_display_unlock();
}

static void on_ssid_start(lv_event_t *e)
{
    (void)e;
    const char *text = lv_textarea_get_text(ssid_textarea);
    if (!text || strlen(text) == 0) return;

    stop_cardkb_timer();

    strncpy(portal_ssid, text, sizeof(portal_ssid) - 1);
    portal_ssid[sizeof(portal_ssid) - 1] = '\0';

    char cmd[80];
    snprintf(cmd, sizeof(cmd), "select_html %d", sd_files[selected_html].number);
    uart_send_command(cmd);

    char cmd2[80];
    snprintf(cmd2, sizeof(cmd2), "start_portal %s", portal_ssid);
    uart_send_command(cmd2);

    ESP_LOGI(TAG, "Portal started: SSID=%s html=%s",
             portal_ssid, sd_files[selected_html].filename);

    bsp_display_lock(0);
    show_running_screen();
    bsp_display_unlock();
}

static void poll_cardkb_ssid(lv_timer_t *t)
{
    (void)t;
    if (!ssid_textarea) return;
    uint8_t key = cardkb_read_key();
    if (key == 0) return;

    if (key == 0x0D || key == 0x0A) {
        on_ssid_start(NULL);
        return;
    }
    if (key == 0x08 || key == 0x7F) {
        lv_textarea_delete_char(ssid_textarea);
        return;
    }
    if (key >= 0x20 && key < 0x7F) {
        lv_textarea_add_char(ssid_textarea, key);
    }
}

static void show_ssid_input(void)
{
    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Portal SSID", on_ssid_cancel, NULL);

    lv_obj_t *cont = lv_obj_create(scr);
    lv_obj_set_size(cont, 300, 160);
    lv_obj_center(cont);
    lv_obj_set_style_bg_color(cont, ui_card_color(), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(cont, 12, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 12, 0);
    lv_obj_set_style_pad_row(cont, 8, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(cont);
    lv_label_set_text(lbl, "Enter AP name:");
    lv_obj_set_style_text_color(lbl, ui_text_color(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);

    ssid_textarea = lv_textarea_create(cont);
    lv_obj_set_size(ssid_textarea, 260, 36);
    lv_textarea_set_one_line(ssid_textarea, true);
    lv_textarea_set_max_length(ssid_textarea, 32);
    lv_textarea_set_placeholder_text(ssid_textarea, "FreeWiFi");
    lv_obj_set_style_text_font(ssid_textarea, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(ssid_textarea, ui_card_color(), 0);
    lv_obj_set_style_text_color(ssid_textarea, lv_color_white(), 0);
    lv_obj_set_style_border_color(ssid_textarea, UI_ACCENT_PURPLE, LV_STATE_FOCUSED);

    lv_obj_t *html_lbl = lv_label_create(cont);
    char htxt[80];
    snprintf(htxt, sizeof(htxt), "HTML: %s", sd_files[selected_html].filename);
    lv_label_set_text(html_lbl, htxt);
    lv_obj_set_style_text_color(html_lbl, ui_muted_color(), 0);
    lv_obj_set_style_text_font(html_lbl, &lv_font_montserrat_10, 0);

    lv_obj_t *btn_row = lv_obj_create(cont);
    lv_obj_set_size(btn_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cancel_btn = lv_btn_create(btn_row);
    lv_obj_set_size(cancel_btn, 100, 32);
    lv_obj_set_style_bg_color(cancel_btn, ui_muted_color(), 0);
    lv_obj_set_style_radius(cancel_btn, 8, 0);
    lv_obj_add_event_cb(cancel_btn, on_ssid_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t *c_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(c_lbl, "Cancel");
    lv_obj_set_style_text_color(c_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(c_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(c_lbl);

    lv_obj_t *start_btn = lv_btn_create(btn_row);
    lv_obj_set_size(start_btn, 100, 32);
    lv_obj_set_style_bg_color(start_btn, UI_ACCENT_PURPLE, 0);
    lv_obj_set_style_radius(start_btn, 8, 0);
    lv_obj_add_event_cb(start_btn, on_ssid_start, LV_EVENT_CLICKED, NULL);
    lv_obj_t *s_lbl = lv_label_create(start_btn);
    lv_label_set_text(s_lbl, "Start");
    lv_obj_set_style_text_color(s_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(s_lbl);

    cardkb_ssid_timer = lv_timer_create(poll_cardkb_ssid, 50, NULL);
}

/* ================================================================== */
/*  Phase 4: Running -- UART monitor                                   */
/* ================================================================== */

static lv_obj_t *portal_status_lbl  = NULL;
static lv_obj_t *portal_capture_lbl = NULL;
static bool portal_running = false;

static void portal_uart_line_cb(const char *line)
{
    if (!portal_running) return;

    const char *p;

    if ((p = strstr(line, "Password:")) != NULL) {
        p += strlen("Password:");
        while (*p == ' ') p++;
        bsp_display_lock(0);
        if (portal_capture_lbl) {
            char txt[128];
            snprintf(txt, sizeof(txt), "Data: %s", p);
            lv_label_set_text(portal_capture_lbl, txt);
        }
        bsp_display_unlock();
    }

    if (strstr(line, "Received POST data:")) {
        bsp_display_lock(0);
        if (portal_capture_lbl)
            lv_label_set_text(portal_capture_lbl, "POST data received!");
        bsp_display_unlock();
    }

    if (strstr(line, "Client connected")) {
        bsp_display_lock(0);
        if (portal_status_lbl)
            lv_label_set_text(portal_status_lbl, "Client connected!");
        bsp_display_unlock();
    }

    if (strstr(line, "Portal data saved")) {
        bsp_display_lock(0);
        if (portal_status_lbl)
            lv_label_set_text(portal_status_lbl, "Data saved to SD");
        bsp_display_unlock();
    }

    if ((p = strstr(line, "Client count")) != NULL) {
        bsp_display_lock(0);
        if (portal_status_lbl)
            lv_label_set_text(portal_status_lbl, line);
        bsp_display_unlock();
    }
}

static void on_running_stop(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Portal STOP");
    portal_running = false;
    uart_set_line_callback(NULL);
    uart_send_command("stop");
    bsp_display_lock(0);
    show_global_attacks_screen();
    bsp_display_unlock();
}

static void show_running_screen(void)
{
    portal_running = true;

    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Portal Active", NULL, NULL);

    lv_obj_t *info = lv_obj_create(scr);
    lv_obj_set_size(info, LV_PCT(100), 140);
    lv_obj_set_pos(info, 0, 36);
    lv_obj_set_style_bg_opa(info, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(info, 0, 0);
    lv_obj_set_style_pad_all(info, 10, 0);
    lv_obj_set_style_pad_row(info, 6, 0);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);

    char ssid_txt[48];
    snprintf(ssid_txt, sizeof(ssid_txt), "AP: %s", portal_ssid);
    lv_obj_t *ssid_lbl = lv_label_create(info);
    lv_label_set_text(ssid_lbl, ssid_txt);
    lv_obj_set_style_text_color(ssid_lbl, UI_ACCENT_PURPLE, 0);
    lv_obj_set_style_text_font(ssid_lbl, &lv_font_montserrat_14, 0);

    char html_txt[80];
    snprintf(html_txt, sizeof(html_txt), "HTML: %s",
             sd_files[selected_html].filename);
    lv_obj_t *html_lbl = lv_label_create(info);
    lv_label_set_text(html_lbl, html_txt);
    lv_obj_set_style_text_color(html_lbl, ui_muted_color(), 0);
    lv_obj_set_style_text_font(html_lbl, &lv_font_montserrat_10, 0);

    portal_status_lbl = lv_label_create(info);
    lv_label_set_text(portal_status_lbl, "Waiting for clients...");
    lv_obj_set_style_text_color(portal_status_lbl, UI_ACCENT_CYAN, 0);
    lv_obj_set_style_text_font(portal_status_lbl, &lv_font_montserrat_12, 0);

    portal_capture_lbl = lv_label_create(info);
    lv_label_set_text(portal_capture_lbl, "");
    lv_obj_set_width(portal_capture_lbl, 300);
    lv_label_set_long_mode(portal_capture_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(portal_capture_lbl, UI_ACCENT_GREEN, 0);
    lv_obj_set_style_text_font(portal_capture_lbl, &lv_font_montserrat_12, 0);

    lv_obj_t *btn = lv_btn_create(scr);
    lv_obj_set_size(btn, 140, 34);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(btn, UI_ACCENT_RED, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_add_event_cb(btn, on_running_stop, LV_EVENT_CLICKED, NULL);
    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, LV_SYMBOL_CLOSE " Stop");
    lv_obj_set_style_text_color(btn_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(btn_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(btn_lbl);

    uart_set_line_callback(portal_uart_line_cb);
}

/* ================================================================== */
/*  Entry point                                                        */
/* ================================================================== */

void show_portal_screen(void)
{
    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Portal", on_loading_back, NULL);

    lv_obj_t *spinner = lv_spinner_create(scr);
    lv_obj_set_size(spinner, 40, 40);
    lv_obj_center(spinner);
    lv_obj_set_y(spinner, 90);

    lv_obj_t *lbl = lv_label_create(scr);
    lv_label_set_text(lbl, "Loading SD card files...");
    lv_obj_set_style_text_color(lbl, ui_text_color(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 45);

    start_list_sd();
}
