#include "wardrive_screen.h"
#include "global_attacks_screen.h"
#include "ui_helpers.h"
#include "uart_handler.h"
#include "bsp/m5stack_core_s3.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "wardrive";

/* ================================================================== */
/*  Network ring buffer                                                */
/* ================================================================== */

#define WD_RING_SIZE 20

typedef struct {
    char ssid[33];
    char bssid[18];
    int  channel;
    int  rssi;
} wd_network_t;

static wd_network_t wd_ring[WD_RING_SIZE];
static int wd_ring_head  = 0;
static int wd_ring_count = 0;
static int wd_total      = 0;

static void wd_ring_push(const wd_network_t *net)
{
    wd_ring[wd_ring_head] = *net;
    wd_ring_head = (wd_ring_head + 1) % WD_RING_SIZE;
    if (wd_ring_count < WD_RING_SIZE) wd_ring_count++;
    wd_total++;
}

/* ================================================================== */
/*  UI state                                                           */
/* ================================================================== */

static bool         wd_running    = false;
static lv_obj_t    *wd_table      = NULL;
static lv_obj_t    *wd_status_lbl = NULL;
static lv_obj_t    *wd_count_lbl  = NULL;
static lv_obj_t    *start_btn     = NULL;
static lv_obj_t    *stop_btn      = NULL;
static lv_obj_t    *gps_overlay   = NULL;

/* ================================================================== */
/*  GPS overlay                                                        */
/* ================================================================== */

static void show_gps_overlay(void)
{
    if (gps_overlay) return;

    lv_obj_t *scr = lv_scr_act();
    gps_overlay = lv_obj_create(scr);
    lv_obj_set_size(gps_overlay, 240, 100);
    lv_obj_center(gps_overlay);
    lv_obj_set_style_bg_color(gps_overlay, UI_BG_CARD, 0);
    lv_obj_set_style_bg_opa(gps_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(gps_overlay, 12, 0);
    lv_obj_set_style_border_color(gps_overlay, UI_ACCENT_TEAL, 0);
    lv_obj_set_style_border_width(gps_overlay, 2, 0);
    lv_obj_set_style_pad_all(gps_overlay, 14, 0);
    lv_obj_set_flex_flow(gps_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(gps_overlay, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(gps_overlay, 8, 0);
    lv_obj_clear_flag(gps_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *spinner = lv_spinner_create(gps_overlay);
    lv_obj_set_size(spinner, 30, 30);

    lv_obj_t *lbl = lv_label_create(gps_overlay);
    lv_label_set_text(lbl, "Acquiring GPS Fix...");
    lv_obj_set_style_text_color(lbl, UI_ACCENT_TEAL, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
}

static void close_gps_overlay(void)
{
    if (gps_overlay) {
        lv_obj_del(gps_overlay);
        gps_overlay = NULL;
    }
}

/* ================================================================== */
/*  Table update                                                       */
/* ================================================================== */

static void update_table(void)
{
    if (!wd_table) return;

    int rows = wd_ring_count < 8 ? wd_ring_count : 8;
    lv_table_set_row_cnt(wd_table, rows + 1);
    lv_table_set_col_cnt(wd_table, 3);
    lv_table_set_col_width(wd_table, 0, 120);
    lv_table_set_col_width(wd_table, 1, 130);
    lv_table_set_col_width(wd_table, 2, 40);

    lv_table_set_cell_value(wd_table, 0, 0, "SSID");
    lv_table_set_cell_value(wd_table, 0, 1, "BSSID");
    lv_table_set_cell_value(wd_table, 0, 2, "CH");

    for (int i = 0; i < rows; i++) {
        int idx = (wd_ring_head - 1 - i + WD_RING_SIZE) % WD_RING_SIZE;
        wd_network_t *n = &wd_ring[idx];

        lv_table_set_cell_value(wd_table, i + 1, 0,
                                n->ssid[0] ? n->ssid : "(hidden)");
        lv_table_set_cell_value(wd_table, i + 1, 1, n->bssid);

        char ch[8];
        snprintf(ch, sizeof(ch), "%d", n->channel);
        lv_table_set_cell_value(wd_table, i + 1, 2, ch);
    }
}

static void update_count_label(void)
{
    if (!wd_count_lbl) return;
    char txt[32];
    snprintf(txt, sizeof(txt), "Networks: %d", wd_total);
    lv_label_set_text(wd_count_lbl, txt);
}

/* ================================================================== */
/*  CSV parser                                                         */
/* ================================================================== */

static bool parse_wardrive_csv(const char *line, wd_network_t *net)
{
    /* BSSID,SSID,[security],timestamp,channel,rssi,lat,lon,alt,acc,WIFI */
    if (strlen(line) < 17) return false;
    if (line[2] != ':' || line[5] != ':') return false;

    char buf[256];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *fields[11] = {NULL};
    int fi = 0;
    char *p = buf;
    while (*p && fi < 11) {
        fields[fi++] = p;
        /* handle brackets in security field */
        if (*p == '[') {
            char *close = strchr(p, ']');
            if (close) p = close + 1;
            if (*p == ',') { *p = '\0'; p++; }
            continue;
        }
        char *comma = strchr(p, ',');
        if (comma) { *comma = '\0'; p = comma + 1; }
        else break;
    }

    if (fi < 6) return false;

    strncpy(net->bssid, fields[0], sizeof(net->bssid) - 1);
    net->bssid[sizeof(net->bssid) - 1] = '\0';
    strncpy(net->ssid, fields[1], sizeof(net->ssid) - 1);
    net->ssid[sizeof(net->ssid) - 1] = '\0';
    net->channel = atoi(fields[4]);
    net->rssi = atoi(fields[5]);

    return true;
}

/* ================================================================== */
/*  UART line callback                                                 */
/* ================================================================== */

static void wd_uart_line_cb(const char *line)
{
    if (!wd_running) return;

    if (strstr(line, "GPS fix obtained")) {
        bsp_display_lock(0);
        close_gps_overlay();
        if (wd_status_lbl)
            lv_label_set_text(wd_status_lbl, "GPS fix OK. Scanning...");
        bsp_display_unlock();
        return;
    }

    if (strstr(line, "GPS fix lost")) {
        bsp_display_lock(0);
        show_gps_overlay();
        if (wd_status_lbl)
            lv_label_set_text(wd_status_lbl, "GPS fix lost!");
        bsp_display_unlock();
        return;
    }

    if (strstr(line, "GPS fix recovered")) {
        bsp_display_lock(0);
        close_gps_overlay();
        if (wd_status_lbl)
            lv_label_set_text(wd_status_lbl, "GPS recovered. Scanning...");
        bsp_display_unlock();
        return;
    }

    if (strstr(line, "No GPS fix obtained")) {
        ESP_LOGW(TAG, "No GPS fix -- stopping wardrive");
        wd_running = false;
        uart_set_line_callback(NULL);
        uart_send_command("stop");
        bsp_display_lock(0);
        close_gps_overlay();
        if (wd_status_lbl)
            lv_label_set_text(wd_status_lbl, "No GPS fix. Stopped.");
        bsp_display_unlock();
        return;
    }

    if (strstr(line, "Still waiting for GPS fix")) {
        return;
    }

    if (strstr(line, "Flushed") || strstr(line, "Logged")) {
        bsp_display_lock(0);
        if (wd_status_lbl)
            lv_label_set_text(wd_status_lbl, line);
        update_count_label();
        bsp_display_unlock();
        return;
    }

    if (strstr(line, "Wardrive promisc")) {
        bsp_display_lock(0);
        if (wd_status_lbl)
            lv_label_set_text(wd_status_lbl, "Wardrive active");
        bsp_display_unlock();
        return;
    }

    /* try CSV network line */
    wd_network_t net;
    memset(&net, 0, sizeof(net));
    if (parse_wardrive_csv(line, &net)) {
        wd_ring_push(&net);
        bsp_display_lock(0);
        update_table();
        update_count_label();
        bsp_display_unlock();
    }
}

/* ================================================================== */
/*  Button callbacks                                                   */
/* ================================================================== */

static void on_start(lv_event_t *e)
{
    (void)e;
    if (wd_running) return;

    wd_ring_head = 0;
    wd_ring_count = 0;
    wd_total = 0;
    wd_running = true;

    uart_set_line_callback(wd_uart_line_cb);
    uart_send_command("start_wardrive_promisc");
    ESP_LOGI(TAG, "Wardrive started");

    bsp_display_lock(0);
    if (wd_status_lbl)
        lv_label_set_text(wd_status_lbl, "Starting...");
    update_count_label();
    update_table();
    show_gps_overlay();
    if (start_btn) lv_obj_add_flag(start_btn, LV_OBJ_FLAG_HIDDEN);
    if (stop_btn)  lv_obj_clear_flag(stop_btn, LV_OBJ_FLAG_HIDDEN);
    bsp_display_unlock();
}

static void on_stop(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Wardrive STOP");
    wd_running = false;
    uart_set_line_callback(NULL);
    uart_send_command("stop");

    bsp_display_lock(0);
    close_gps_overlay();
    if (wd_status_lbl)
        lv_label_set_text(wd_status_lbl, "Stopped");
    if (start_btn) lv_obj_clear_flag(start_btn, LV_OBJ_FLAG_HIDDEN);
    if (stop_btn)  lv_obj_add_flag(stop_btn, LV_OBJ_FLAG_HIDDEN);
    bsp_display_unlock();
}

static void on_back(lv_event_t *e)
{
    (void)e;
    if (wd_running) {
        wd_running = false;
        uart_set_line_callback(NULL);
        uart_send_command("stop");
    }
    close_gps_overlay();
    wd_table = NULL;
    wd_status_lbl = NULL;
    wd_count_lbl = NULL;
    start_btn = NULL;
    stop_btn = NULL;

    bsp_display_lock(0);
    show_global_attacks_screen();
    bsp_display_unlock();
}

/* ================================================================== */
/*  Main page                                                          */
/* ================================================================== */

void show_wardrive_screen(void)
{
    wd_ring_head = 0;
    wd_ring_count = 0;
    wd_total = 0;
    gps_overlay = NULL;

    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Wardrive", on_back, NULL);

    /* status row */
    lv_obj_t *status_row = lv_obj_create(scr);
    lv_obj_set_size(status_row, LV_PCT(100), 24);
    lv_obj_set_pos(status_row, 0, 36);
    lv_obj_set_style_bg_opa(status_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(status_row, 0, 0);
    lv_obj_set_style_pad_hor(status_row, 8, 0);
    lv_obj_set_style_pad_ver(status_row, 2, 0);
    lv_obj_set_flex_flow(status_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(status_row, LV_OBJ_FLAG_SCROLLABLE);

    wd_status_lbl = lv_label_create(status_row);
    lv_label_set_text(wd_status_lbl, "Ready");
    lv_obj_set_style_text_color(wd_status_lbl, UI_ACCENT_TEAL, 0);
    lv_obj_set_style_text_font(wd_status_lbl, &lv_font_montserrat_12, 0);

    wd_count_lbl = lv_label_create(status_row);
    lv_label_set_text(wd_count_lbl, "Networks: 0");
    lv_obj_set_style_text_color(wd_count_lbl, UI_TEXT_COLOR, 0);
    lv_obj_set_style_text_font(wd_count_lbl, &lv_font_montserrat_12, 0);

    /* button row */
    lv_obj_t *btn_row = lv_obj_create(scr);
    lv_obj_set_size(btn_row, LV_PCT(100), 36);
    lv_obj_set_pos(btn_row, 0, 60);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_set_style_pad_column(btn_row, 8, 0);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    start_btn = lv_btn_create(btn_row);
    lv_obj_set_size(start_btn, 120, 30);
    lv_obj_set_style_bg_color(start_btn, UI_ACCENT_TEAL, 0);
    lv_obj_set_style_radius(start_btn, 8, 0);
    lv_obj_add_event_cb(start_btn, on_start, LV_EVENT_CLICKED, NULL);
    lv_obj_t *start_lbl = lv_label_create(start_btn);
    lv_label_set_text(start_lbl, LV_SYMBOL_PLAY " Start");
    lv_obj_set_style_text_color(start_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(start_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(start_lbl);

    stop_btn = lv_btn_create(btn_row);
    lv_obj_set_size(stop_btn, 120, 30);
    lv_obj_set_style_bg_color(stop_btn, UI_ACCENT_RED, 0);
    lv_obj_set_style_radius(stop_btn, 8, 0);
    lv_obj_add_event_cb(stop_btn, on_stop, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(stop_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *stop_lbl = lv_label_create(stop_btn);
    lv_label_set_text(stop_lbl, LV_SYMBOL_CLOSE " Stop");
    lv_obj_set_style_text_color(stop_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(stop_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(stop_lbl);

    /* network table */
    wd_table = lv_table_create(scr);
    lv_obj_set_size(wd_table, 310, 135);
    lv_obj_set_pos(wd_table, 5, 98);
    lv_obj_set_style_bg_color(wd_table, lv_color_hex(0x111122), 0);
    lv_obj_set_style_bg_opa(wd_table, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(wd_table, 6, 0);
    lv_obj_set_style_border_width(wd_table, 1, 0);
    lv_obj_set_style_border_color(wd_table, UI_TEXT_DIM, 0);
    lv_obj_set_style_pad_all(wd_table, 2, 0);
    lv_obj_set_style_text_font(wd_table, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(wd_table, UI_TEXT_COLOR, 0);

    /* header styling */
    lv_obj_set_style_bg_color(wd_table, UI_BAR_COLOR, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(wd_table, UI_TEXT_COLOR, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(wd_table, 0, LV_PART_ITEMS);
    lv_obj_set_style_pad_ver(wd_table, 2, LV_PART_ITEMS);
    lv_obj_set_style_pad_hor(wd_table, 4, LV_PART_ITEMS);

    update_table();
}
