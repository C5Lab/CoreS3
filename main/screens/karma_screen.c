#include "karma_screen.h"
#include "attack_select_screen.h"
#include "sniffer_screen.h"
#include "ui_helpers.h"
#include "uart_handler.h"
#include "psram_dynarr.h"
#include "bsp/m5stack_core_s3.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static const char *TAG = "karma";

/* ================================================================== */
/*  Probe list state                                                   */
/* ================================================================== */

#define KARMA_PROBE_HARD_CAP 1024

typedef struct {
    int  index;
    char ssid[33];
} karma_probe_t;

static karma_probe_t *probes = NULL;
static int  probes_cap         = 0;
static int  probe_count        = 0;
static bool probe_collecting   = false;
static int  selected_probe_idx = -1;
static esp_timer_handle_t probe_timeout_timer = NULL;

/* ================================================================== */
/*  SD / HTML file list state                                          */
/* ================================================================== */

#define MAX_SD_FILES 32

typedef struct {
    int  number;
    char filename[64];
} karma_sd_file_t;

static karma_sd_file_t sd_files[MAX_SD_FILES];
static int  sd_file_count = 0;
static bool sd_collecting = false;
static esp_timer_handle_t sd_timeout_timer = NULL;

/* ================================================================== */
/*  Karma running state                                                */
/* ================================================================== */

static lv_obj_t *karma_status_lbl  = NULL;
static lv_obj_t *karma_capture_lbl = NULL;
static bool karma_running = false;

/* ---- forward declarations ---- */
static void show_probe_picker(void);
static void show_html_picker(void);
static void show_running_screen(void);
static void start_list_sd(void);

/* ================================================================== */
/*  Phase 1: list_probes                                               */
/* ================================================================== */

static bool parse_probe_line(const char *line, karma_probe_t *probe)
{
    const char *p = line;
    while (*p == ' ') p++;
    if (!isdigit((unsigned char)*p)) return false;

    int idx = 0;
    while (isdigit((unsigned char)*p)) {
        idx = idx * 10 + (*p - '0');
        p++;
    }
    while (*p == ' ') p++;
    if (*p == '\0') return false;

    probe->index = idx;
    strncpy(probe->ssid, p, sizeof(probe->ssid) - 1);
    probe->ssid[sizeof(probe->ssid) - 1] = '\0';

    int len = strlen(probe->ssid);
    while (len > 0 && (probe->ssid[len-1] == ' ' ||
                       probe->ssid[len-1] == '\r' ||
                       probe->ssid[len-1] == '\n'))
        probe->ssid[--len] = '\0';
    return len > 0;
}

static void probe_timeout_cb(void *arg)
{
    (void)arg;
    if (!probe_collecting) return;
    probe_collecting = false;
    uart_set_line_callback(NULL);

    ESP_LOGI(TAG, "Probe list complete, %d probes", probe_count);

    bsp_display_lock(0);
    show_probe_picker();
    bsp_display_unlock();
}

static void probe_line_cb(const char *line)
{
    if (!probe_collecting) return;

    karma_probe_t p;
    if (parse_probe_line(line, &p) &&
        psram_dynarr_ensure((void **)&probes, &probes_cap,
                            probe_count + 1, sizeof(*probes),
                            KARMA_PROBE_HARD_CAP)) {
        probes[probe_count++] = p;
        esp_timer_stop(probe_timeout_timer);
        esp_timer_start_once(probe_timeout_timer, 500000);
    }
}

/* ================================================================== */
/*  Phase 2: list_sd                                                   */
/* ================================================================== */

static bool parse_sd_line(const char *line, karma_sd_file_t *f)
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
    karma_sd_file_t f;
    if (parse_sd_line(line, &f) && sd_file_count < MAX_SD_FILES) {
        sd_files[sd_file_count++] = f;
        esp_timer_stop(sd_timeout_timer);
        esp_timer_start_once(sd_timeout_timer, 500000);
    }
}

static void start_list_sd(void)
{
    sd_file_count = 0;
    sd_collecting = true;

    if (!sd_timeout_timer) {
        esp_timer_create_args_t args = {
            .callback = sd_timeout_cb,
            .name = "karma_sd_to",
        };
        esp_timer_create(&args, &sd_timeout_timer);
    }

    uart_set_line_callback(sd_line_cb);
    esp_timer_start_once(sd_timeout_timer, 2000000);
    uart_send_command("list_sd");
}

/* ================================================================== */
/*  Phase 1 UI: Probe picker                                           */
/* ================================================================== */

static void on_probe_back(lv_event_t *e)
{
    (void)e;
    probe_collecting = false;
    uart_set_line_callback(NULL);
    if (probe_timeout_timer)
        esp_timer_stop(probe_timeout_timer);

    bsp_display_lock(0);
    show_sniffer_screen();
    bsp_display_unlock();
}

static void on_probe_selected(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    selected_probe_idx = idx;
    ESP_LOGI(TAG, "Probe selected: %d -> %s", probes[idx].index, probes[idx].ssid);

    bsp_display_lock(0);
    {
        lv_obj_t *scr = ui_screen_clear();
        ui_create_top_bar(scr, "Karma", NULL, NULL);

        lv_obj_t *spinner = lv_spinner_create(scr);
        lv_obj_set_size(spinner, 40, 40);
        lv_obj_center(spinner);
        lv_obj_set_y(spinner, 90);

        lv_obj_t *lbl = lv_label_create(scr);
        lv_label_set_text(lbl, "Loading HTML files...");
        lv_obj_set_style_text_color(lbl, ui_text_color(), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 45);
    }
    bsp_display_unlock();

    start_list_sd();
}

static void show_probe_picker(void)
{
    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Karma - Probes", on_probe_back, NULL);

    if (probe_count == 0) {
        lv_obj_t *lbl = lv_label_create(scr);
        lv_label_set_text(lbl, "No probe requests captured.\nLet sniffer run longer.");
        lv_obj_set_style_text_color(lbl, ui_muted_color(), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_width(lbl, LV_PCT(90));
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
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

    for (int i = 0; i < probe_count; i++) {
        lv_obj_t *btn = lv_btn_create(list);
        lv_obj_set_size(btn, LV_PCT(100), 34);
        lv_obj_set_style_bg_color(btn, ui_card_color(), 0);
        lv_obj_set_style_bg_color(btn, UI_ACCENT_ORANGE, LV_STATE_PRESSED);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_pad_hor(btn, 8, 0);
        lv_obj_add_event_cb(btn, on_probe_selected, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);

        lv_obj_t *lbl = lv_label_create(btn);
        char display[48];
        snprintf(display, sizeof(display), "%d. %s",
                 probes[i].index, probes[i].ssid);
        lv_label_set_text(lbl, display);
        lv_obj_set_style_text_color(lbl, ui_text_color(), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
    }
}

/* ================================================================== */
/*  Phase 2 UI: HTML picker                                            */
/* ================================================================== */

static void on_html_back(lv_event_t *e)
{
    (void)e;
    sd_collecting = false;
    uart_set_line_callback(NULL);
    if (sd_timeout_timer)
        esp_timer_stop(sd_timeout_timer);
    bsp_display_lock(0);
    show_probe_picker();
    bsp_display_unlock();
}

static void on_html_selected(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    ESP_LOGI(TAG, "HTML selected: %d -> %s",
             sd_files[idx].number, sd_files[idx].filename);

    char cmd[80];
    snprintf(cmd, sizeof(cmd), "select_html %d", sd_files[idx].number);
    uart_send_command(cmd);

    char cmd2[80];
    snprintf(cmd2, sizeof(cmd2), "start_karma %d", probes[selected_probe_idx].index);
    uart_send_command(cmd2);

    karma_running = true;
    karma_status_lbl = NULL;
    karma_capture_lbl = NULL;

    bsp_display_lock(0);
    show_running_screen();
    bsp_display_unlock();
}

static void show_html_picker(void)
{
    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Karma - HTML", on_html_back, NULL);

    if (sd_file_count == 0) {
        lv_obj_t *lbl = lv_label_create(scr);
        lv_label_set_text(lbl, "No HTML files found on SD card.");
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
        lv_obj_set_style_bg_color(btn, UI_ACCENT_ORANGE, LV_STATE_PRESSED);
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
/*  Phase 3: Karma running -- monitor UART                             */
/* ================================================================== */

static void karma_uart_line_cb(const char *line)
{
    if (!karma_running) return;

    if (strstr(line, "Captive portal started")) {
        bsp_display_lock(0);
        if (karma_status_lbl)
            lv_label_set_text(karma_status_lbl, "Portal active!");
        bsp_display_unlock();
    }

    if (strstr(line, "Client connected")) {
        bsp_display_lock(0);
        if (karma_status_lbl)
            lv_label_set_text(karma_status_lbl, "Client connected!");
        bsp_display_unlock();
    }

    const char *pw;
    if ((pw = strstr(line, "Password:")) != NULL) {
        pw += strlen("Password:");
        while (*pw == ' ') pw++;
        bsp_display_lock(0);
        if (karma_capture_lbl) {
            char txt[128];
            snprintf(txt, sizeof(txt), "Captured: %s", pw);
            lv_label_set_text(karma_capture_lbl, txt);
        }
        bsp_display_unlock();
    }

    if (strstr(line, "Portal data saved")) {
        bsp_display_lock(0);
        if (karma_status_lbl)
            lv_label_set_text(karma_status_lbl, "Data saved to SD!");
        bsp_display_unlock();
    }
}

static void on_running_stop(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Karma STOP");
    karma_running = false;
    uart_set_line_callback(NULL);
    uart_send_command("stop");
    bsp_display_lock(0);
    show_attack_select_screen();
    bsp_display_unlock();
}

static void show_running_screen(void)
{
    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Karma Running", NULL, NULL);

    lv_obj_t *info = lv_obj_create(scr);
    lv_obj_set_size(info, LV_PCT(100), 140);
    lv_obj_set_pos(info, 0, 36);
    lv_obj_set_style_bg_opa(info, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(info, 0, 0);
    lv_obj_set_style_pad_all(info, 10, 0);
    lv_obj_set_style_pad_row(info, 6, 0);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *ssid_lbl = lv_label_create(info);
    char stxt[48];
    snprintf(stxt, sizeof(stxt), "SSID: %s", probes[selected_probe_idx].ssid);
    lv_label_set_text(ssid_lbl, stxt);
    lv_obj_set_style_text_color(ssid_lbl, UI_ACCENT_ORANGE, 0);
    lv_obj_set_style_text_font(ssid_lbl, &lv_font_montserrat_14, 0);

    karma_status_lbl = lv_label_create(info);
    lv_label_set_text(karma_status_lbl, "Starting portal...");
    lv_obj_set_style_text_color(karma_status_lbl, UI_ACCENT_CYAN, 0);
    lv_obj_set_style_text_font(karma_status_lbl, &lv_font_montserrat_12, 0);

    karma_capture_lbl = lv_label_create(info);
    lv_label_set_text(karma_capture_lbl, "");
    lv_obj_set_width(karma_capture_lbl, 300);
    lv_label_set_long_mode(karma_capture_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(karma_capture_lbl, UI_ACCENT_GREEN, 0);
    lv_obj_set_style_text_font(karma_capture_lbl, &lv_font_montserrat_12, 0);

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

    uart_set_line_callback(karma_uart_line_cb);
}

/* ================================================================== */
/*  Entry point -- called from sniffer screen                          */
/* ================================================================== */

void show_karma_screen(void)
{
    uart_send_command("stop");

    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Karma", on_probe_back, NULL);

    lv_obj_t *spinner = lv_spinner_create(scr);
    lv_obj_set_size(spinner, 40, 40);
    lv_obj_center(spinner);
    lv_obj_set_y(spinner, 90);

    lv_obj_t *lbl = lv_label_create(scr);
    lv_label_set_text(lbl, "Loading probe requests...");
    lv_obj_set_style_text_color(lbl, ui_text_color(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 45);

    probe_count = 0;
    probe_collecting = true;

    if (!probe_timeout_timer) {
        esp_timer_create_args_t args = {
            .callback = probe_timeout_cb,
            .name = "probe_timeout",
        };
        esp_timer_create(&args, &probe_timeout_timer);
    }

    uart_set_line_callback(probe_line_cb);
    esp_timer_start_once(probe_timeout_timer, 2000000);
    uart_send_command("list_probes");
}
