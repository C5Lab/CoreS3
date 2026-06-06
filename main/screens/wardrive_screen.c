#include "wardrive_screen.h"
#include "global_attacks_screen.h"
#include "wifi_scan_screen.h"
#include "wifi_connect_helper.h"
#include "ui_helpers.h"
#include "uart_handler.h"
#include "gps_module.h"
#include "psram_dynarr.h"
#include "bsp/m5stack_core_s3.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

static const char *TAG = "wardrive";

/* ================================================================== */
/*  Network ring buffer                                                */
/* ================================================================== */

#define WD_RING_SIZE      100
#define WD_DISPLAY_ROWS    30

typedef struct {
    char ssid[33];
    char bssid[18];
    char security[28];
    char lat[14];
    char lon[14];
    int  channel;
    int  rssi;
    bool is_ble;
} wd_network_t;

static wd_network_t wd_ring[WD_RING_SIZE];
static int wd_ring_head  = 0;
static int wd_ring_count = 0;

/* Cumulative counters (mirror firmware stats) */
static int    wd_wifi_count  = 0;
static int    wd_bt_count    = 0;
static int    wd_sat_count   = 0;
static double wd_distance_m  = 0.0;

static void wd_ring_push(const wd_network_t *net)
{
    wd_ring[wd_ring_head] = *net;
    wd_ring_head = (wd_ring_head + 1) % WD_RING_SIZE;
    if (wd_ring_count < WD_RING_SIZE) wd_ring_count++;
    if (net->is_ble) wd_bt_count++;
    else             wd_wifi_count++;
}

/* ================================================================== */
/*  UI / runtime state                                                 */
/* ================================================================== */

typedef enum {
    WD_GPS_M5 = 0,
    WD_GPS_ATGM,
    WD_GPS_EXTERNAL,
} wd_gps_type_t;

static bool          wd_running        = false;
/* True once the *current* session's "Promiscuous wardrive started" was seen.
 * Starting a new wardrive makes the firmware tear down the prior run first,
 * which emits a "Wardrive promisc stopped" line; we must ignore that until
 * the new session has actually started. */
static bool          wd_session_started = false;
static bool          wd_trace_enabled  = true;   /* matches Tab5 default */
static wd_gps_type_t wd_gps_type       = WD_GPS_M5;
static bool          wd_use_external   = false;

static lv_obj_t *wd_list        = NULL;   /* scrollable network card list */
static lv_obj_t *wd_status_lbl  = NULL;
static lv_obj_t *wd_stats_lbl   = NULL;
static lv_obj_t *start_btn      = NULL;
static lv_obj_t *stop_btn       = NULL;
static lv_obj_t *trace_btn      = NULL;
static lv_obj_t *trace_lbl      = NULL;
static lv_obj_t *gps_overlay    = NULL;
static lv_obj_t *gps_overlay_lbl = NULL;
static lv_obj_t *wd_local_gps_lbl = NULL;

static lv_timer_t *wd_gps_push_timer = NULL;
static lv_timer_t *wd_local_gps_timer = NULL;

/* forward decls */
static void show_gps_type_popup(void);
static void wardrive_upload_btn_cb(lv_event_t *e);

/* ================================================================== */
/*  GPS fix overlay (full-screen dimmed modal)                         */
/* ================================================================== */

static void show_gps_overlay(const char *msg)
{
    lv_obj_t *scr = lv_scr_act();

    if (!gps_overlay) {
        gps_overlay = lv_obj_create(scr);
        lv_obj_set_size(gps_overlay, LV_PCT(100), LV_PCT(100));
        lv_obj_set_pos(gps_overlay, 0, 0);
        lv_obj_set_style_bg_color(gps_overlay, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(gps_overlay, LV_OPA_70, 0);
        lv_obj_set_style_border_width(gps_overlay, 0, 0);
        lv_obj_set_style_radius(gps_overlay, 0, 0);
        lv_obj_set_style_pad_all(gps_overlay, 0, 0);
        lv_obj_clear_flag(gps_overlay, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *card = lv_obj_create(gps_overlay);
        lv_obj_set_size(card, 280, 150);
        lv_obj_center(card);
        lv_obj_set_style_bg_color(card, ui_card_color(), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 14, 0);
        lv_obj_set_style_border_color(card, UI_ACCENT_TEAL, 0);
        lv_obj_set_style_border_width(card, 2, 0);
        lv_obj_set_style_pad_all(card, 12, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(card, 8, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *icon = lv_label_create(card);
        lv_label_set_text(icon, LV_SYMBOL_GPS);
        lv_obj_set_style_text_color(icon, UI_ACCENT_TEAL, 0);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, 0);

        gps_overlay_lbl = lv_label_create(card);
        lv_obj_set_style_text_color(gps_overlay_lbl, UI_ACCENT_ORANGE, 0);
        lv_obj_set_style_text_font(gps_overlay_lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_align(gps_overlay_lbl, LV_TEXT_ALIGN_CENTER, 0);

        lv_obj_t *sub = lv_label_create(card);
        lv_label_set_text(sub, "Need clear view of the sky");
        lv_obj_set_style_text_color(sub, ui_muted_color(), 0);
        lv_obj_set_style_text_font(sub, &lv_font_montserrat_12, 0);
    }

    if (gps_overlay_lbl)
        lv_label_set_text(gps_overlay_lbl, msg ? msg : "Acquiring GPS Fix...");
    lv_obj_move_foreground(gps_overlay);
}

static void close_gps_overlay(void)
{
    if (gps_overlay) {
        lv_obj_del(gps_overlay);
        gps_overlay = NULL;
        gps_overlay_lbl = NULL;
    }
}

/* ================================================================== */
/*  Network list / stats                                               */
/* ================================================================== */

static lv_color_t security_color(const char *sec)
{
    if (strstr(sec, "WPA3"))            return UI_ACCENT_GREEN;
    if (strstr(sec, "WPA2") || strstr(sec, "WPA")) return UI_ACCENT_ORANGE;
    if (strstr(sec, "WEP"))             return UI_ACCENT_ORANGE;
    if (sec[0] == '\0' || strstr(sec, "OPEN") || strstr(sec, "ESS"))
        return UI_ACCENT_RED;
    return ui_muted_color();
}

static void add_network_card(const wd_network_t *n)
{
    lv_obj_t *card = lv_obj_create(wd_list);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, ui_panel_color(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 6, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 4, 0);
    lv_obj_set_style_pad_row(card, 1, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* Top line: SSID (grow) + TYPE + RSSI */
    lv_obj_t *top = lv_obj_create(card);
    lv_obj_set_size(top, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_set_style_pad_all(top, 0, 0);
    lv_obj_set_style_pad_column(top, 6, 0);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ssid = lv_label_create(top);
    lv_label_set_text(ssid, n->ssid[0] ? n->ssid : "(hidden)");
    lv_obj_set_style_text_color(ssid, n->ssid[0] ? ui_text_color() : ui_muted_color(), 0);
    lv_obj_set_style_text_font(ssid, &lv_font_montserrat_12, 0);
    lv_label_set_long_mode(ssid, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(ssid, 1);

    lv_obj_t *type = lv_label_create(top);
    lv_label_set_text(type, n->is_ble ? "BLE" : "WIFI");
    lv_obj_set_style_text_color(type, n->is_ble ? UI_ACCENT_BLUE : UI_ACCENT_GREEN, 0);
    lv_obj_set_style_text_font(type, &lv_font_montserrat_10, 0);

    lv_obj_t *rssi = lv_label_create(top);
    char rbuf[12];
    snprintf(rbuf, sizeof(rbuf), "%ddBm", n->rssi);
    lv_label_set_text(rssi, rbuf);
    lv_obj_set_style_text_color(rssi, ui_muted_color(), 0);
    lv_obj_set_style_text_font(rssi, &lv_font_montserrat_10, 0);

    /* Bottom line: BSSID + SEC + coords */
    lv_obj_t *bot = lv_obj_create(card);
    lv_obj_set_size(bot, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(bot, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bot, 0, 0);
    lv_obj_set_style_pad_all(bot, 0, 0);
    lv_obj_set_style_pad_column(bot, 6, 0);
    lv_obj_set_flex_flow(bot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bot, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bot, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bssid = lv_label_create(bot);
    lv_label_set_text(bssid, n->bssid);
    lv_obj_set_style_text_color(bssid, ui_muted_color(), 0);
    lv_obj_set_style_text_font(bssid, &lv_font_montserrat_10, 0);

    if (n->security[0] && !n->is_ble) {
        lv_obj_t *sec = lv_label_create(bot);
        char sbuf[16];
        snprintf(sbuf, sizeof(sbuf), "%.12s", n->security);
        lv_label_set_text(sec, sbuf);
        lv_obj_set_style_text_color(sec, security_color(n->security), 0);
        lv_obj_set_style_text_font(sec, &lv_font_montserrat_10, 0);
    }

    lv_obj_t *coord = lv_label_create(bot);
    char cbuf[32];
    if (n->lat[0] && n->lon[0])
        snprintf(cbuf, sizeof(cbuf), "%s,%s", n->lat, n->lon);
    else
        snprintf(cbuf, sizeof(cbuf), "--");
    lv_label_set_text(coord, cbuf);
    lv_obj_set_style_text_color(coord, UI_ACCENT_TEAL, 0);
    lv_obj_set_style_text_font(coord, &lv_font_montserrat_10, 0);
    lv_label_set_long_mode(coord, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(coord, 1);
    lv_obj_set_style_text_align(coord, LV_TEXT_ALIGN_RIGHT, 0);
}

static void update_list(void)
{
    if (!wd_list || !lv_obj_is_valid(wd_list)) return;

    lv_obj_clean(wd_list);

    int rows = wd_ring_count < WD_DISPLAY_ROWS ? wd_ring_count : WD_DISPLAY_ROWS;
    for (int i = 0; i < rows; i++) {
        int idx = (wd_ring_head - 1 - i + WD_RING_SIZE) % WD_RING_SIZE;
        add_network_card(&wd_ring[idx]);
    }
}

static void update_stats(void)
{
    if (!wd_stats_lbl || !lv_obj_is_valid(wd_stats_lbl)) return;
    char buf[64];
    snprintf(buf, sizeof(buf), "WiFi:%d  BT:%d  SAT:%d  %.2fkm",
             wd_wifi_count, wd_bt_count, wd_sat_count, wd_distance_m / 1000.0);
    lv_label_set_text(wd_stats_lbl, buf);
}

static void set_status(const char *txt, lv_color_t color)
{
    if (!wd_status_lbl || !lv_obj_is_valid(wd_status_lbl)) return;
    lv_label_set_text(wd_status_lbl, txt);
    lv_obj_set_style_text_color(wd_status_lbl, color, 0);
}

/* ================================================================== */
/*  CSV parser (bracket-aware)                                         */
/* ================================================================== */

/* BSSID,SSID,[security],timestamp,channel,rssi,lat,lon,alt,acc,WIFI|BLE */
static bool parse_wardrive_csv(const char *line, wd_network_t *net)
{
    if (strlen(line) < 17) return false;
    if (line[2] != ':' || line[5] != ':') return false;

    bool is_ble  = strstr(line, ",BLE")  != NULL;
    bool is_wifi = strstr(line, ",WIFI") != NULL;
    if (!is_ble && !is_wifi) return false;

    char buf[256];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    /* split on commas that are not inside [] */
    char *fields[12] = {0};
    int fi = 0;
    int depth = 0;
    char *start = buf;
    for (char *p = buf; *p && fi < 12; p++) {
        if (*p == '[') depth++;
        else if (*p == ']') { if (depth > 0) depth--; }
        else if (*p == ',' && depth == 0) {
            *p = '\0';
            fields[fi++] = start;
            start = p + 1;
        }
    }
    if (fi < 12) fields[fi++] = start;

    if (fi < 6) return false;

    memset(net, 0, sizeof(*net));
    net->is_ble = is_ble;
    strncpy(net->bssid, fields[0], sizeof(net->bssid) - 1);
    strncpy(net->ssid,  fields[1], sizeof(net->ssid)  - 1);
    if (fields[2]) {
        strncpy(net->security, fields[2], sizeof(net->security) - 1);
    }
    if (fields[4]) net->channel = atoi(fields[4]);
    if (fields[5]) net->rssi    = atoi(fields[5]);
    if (fields[6]) { strncpy(net->lat, fields[6], sizeof(net->lat) - 1); }
    if (fields[7]) { strncpy(net->lon, fields[7], sizeof(net->lon) - 1); }
    return true;
}

/* ================================================================== */
/*  UART monitor callback (runs in uart_rx task)                       */
/* ================================================================== */

static void reply_tab_gps_read(void)
{
    double lat = 0, lon = 0;
    char resp[64];
    if (gps_module_get_fix(&lat, &lon, NULL, NULL))
        snprintf(resp, sizeof(resp), "%.6f,%.6f", lat, lon);
    else
        snprintf(resp, sizeof(resp), "No GPS fix");
    uart_send_command(resp);
}

static void wd_uart_line_cb(const char *line)
{
    if (!wd_running) return;

    if (strstr(line, "tab_gps_read")) {
        reply_tab_gps_read();
        return;
    }

    if (strstr(line, "GPS fix obtained")) {
        bsp_display_lock(0);
        close_gps_overlay();
        set_status("GPS fix OK. Scanning...", UI_ACCENT_GREEN);
        bsp_display_unlock();
        return;
    }
    if (strstr(line, "GPS fix lost")) {
        bsp_display_lock(0);
        show_gps_overlay("GPS Fix Lost - Pausing...");
        set_status("GPS lost - paused", UI_ACCENT_RED);
        bsp_display_unlock();
        return;
    }
    if (strstr(line, "GPS fix recovered")) {
        bsp_display_lock(0);
        close_gps_overlay();
        set_status("GPS recovered. Scanning...", UI_ACCENT_GREEN);
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
        set_status("No GPS fix. Stopped.", UI_ACCENT_RED);
        if (start_btn) lv_obj_clear_flag(start_btn, LV_OBJ_FLAG_HIDDEN);
        if (stop_btn)  lv_obj_add_flag(stop_btn, LV_OBJ_FLAG_HIDDEN);
        bsp_display_unlock();
        return;
    }
    if (strstr(line, "Still waiting for GPS fix")) {
        int elapsed = 0, total = 0;
        const char *paren = strchr(line, '(');
        if (paren) sscanf(paren, "(%d/%d", &elapsed, &total);
        bsp_display_lock(0);
        if (gps_overlay_lbl) {
            char b[40];
            if (total > 0) snprintf(b, sizeof(b), "Acquiring GPS Fix... (%d/%ds)", elapsed, total);
            else           snprintf(b, sizeof(b), "Acquiring GPS Fix...");
            lv_label_set_text(gps_overlay_lbl, b);
        }
        bsp_display_unlock();
        return;
    }

    if (strstr(line, "Wardrive promisc:")) {
        const char *sats_ptr = strstr(line, "sats:");
        if (sats_ptr) { sats_ptr += 5; while (*sats_ptr == ' ') sats_ptr++; wd_sat_count = atoi(sats_ptr); }
        const char *dist_ptr = strstr(line, "dist:");
        if (dist_ptr) { dist_ptr += 5; while (*dist_ptr == ' ') dist_ptr++; wd_distance_m = atof(dist_ptr); }
        bsp_display_lock(0);
        set_status("Scanning...", UI_ACCENT_GREEN);
        update_stats();
        bsp_display_unlock();
        return;
    }

    if (strstr(line, "Promiscuous wardrive started")) {
        wd_session_started = true;
        bsp_display_lock(0);
        set_status(wd_trace_enabled ? "Trace active - Scanning..." : "Active - Scanning...",
                   UI_ACCENT_GREEN);
        bsp_display_unlock();
        return;
    }

    if (strstr(line, "Flushed") || strstr(line, "Logged")) {
        bsp_display_lock(0);
        set_status("Scanning...", UI_ACCENT_GREEN);
        update_stats();
        bsp_display_unlock();
        return;
    }

    if (strstr(line, "Wardrive promisc stopped")) {
        /* Ignore the teardown of the previous run that the firmware emits
         * while (re)starting; only honor a stop for the live session. */
        if (!wd_session_started) return;
        wd_running = false;
        uart_set_line_callback(NULL);
        bsp_display_lock(0);
        close_gps_overlay();
        set_status("Wardrive stopped", ui_muted_color());
        if (start_btn) lv_obj_clear_flag(start_btn, LV_OBJ_FLAG_HIDDEN);
        if (stop_btn)  lv_obj_add_flag(stop_btn, LV_OBJ_FLAG_HIDDEN);
        update_stats();
        bsp_display_unlock();
        return;
    }

    wd_network_t net;
    if (parse_wardrive_csv(line, &net)) {
        wd_ring_push(&net);
        bsp_display_lock(0);
        update_list();
        update_stats();
        bsp_display_unlock();
    }
}

/* ================================================================== */
/*  External GPS push timer (runs on LVGL thread)                      */
/* ================================================================== */

/* Push state (reset on each Start). */
static bool     wd_push_init      = false;
static bool     wd_push_last_valid = false;
static double   wd_push_last_lat  = 0, wd_push_last_lon = 0;
static uint32_t wd_push_last_tick = 0;

/* Resend at least this often even when stationary, so the firmware wardrive
 * loop always has a fresh fix regardless of when it started listening. */
#define WD_GPS_HEARTBEAT_MS 2000

static void gps_push_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!wd_running || !wd_use_external) return;

    double lat = 0, lon = 0;
    bool has_fix = gps_module_get_fix(&lat, &lon, NULL, NULL);

    char cmd[64];
    if (has_fix) {
        bool moved = !wd_push_last_valid ||
                     fabs(lat - wd_push_last_lat) >= 0.00001 ||
                     fabs(lon - wd_push_last_lon) >= 0.00001;
        bool heartbeat = wd_push_init &&
                         lv_tick_elaps(wd_push_last_tick) >= WD_GPS_HEARTBEAT_MS;
        if (!wd_push_init || moved || heartbeat) {
            snprintf(cmd, sizeof(cmd), "set_gps_position %.7f %.7f", lat, lon);
            uart_send_command(cmd);
            wd_push_last_lat = lat;
            wd_push_last_lon = lon;
            wd_push_last_tick = lv_tick_get();
        }
        wd_push_last_valid = true;
        wd_push_init = true;
    } else if (!wd_push_init || wd_push_last_valid) {
        uart_send_command("set_gps_position");
        wd_push_last_valid = false;
        wd_push_init = true;
        wd_push_last_tick = lv_tick_get();
    }
}

static void start_gps_push_timer(void)
{
    if (wd_gps_push_timer) return;
    /* Fresh handshake every session. */
    wd_push_init = false;
    wd_push_last_valid = false;
    wd_push_last_lat = 0;
    wd_push_last_lon = 0;
    wd_push_last_tick = 0;
    wd_gps_push_timer = lv_timer_create(gps_push_timer_cb, 300, NULL);
}

/* Local-module status line (independent of firmware), refreshed ~1 Hz. */
static void local_gps_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!wd_local_gps_lbl || !lv_obj_is_valid(wd_local_gps_lbl)) return;

    char buf[80];
    if (!gps_module_is_running()) {
        snprintf(buf, sizeof(buf), "GPS(local): off (enable in Settings)");
        lv_obj_set_style_text_color(wd_local_gps_lbl, ui_muted_color(), 0);
    } else {
        double lat = 0, lon = 0;
        int sats = 0;
        if (gps_module_get_fix(&lat, &lon, NULL, &sats)) {
            snprintf(buf, sizeof(buf), "GPS(local): %.5f,%.5f sat:%d @%d",
                     lat, lon, sats, gps_module_get_baud());
            lv_obj_set_style_text_color(wd_local_gps_lbl, UI_ACCENT_GREEN, 0);
        } else {
            snprintf(buf, sizeof(buf), "GPS(local): no fix (@%d, %u lines)",
                     gps_module_get_baud(), gps_module_nmea_count());
            lv_obj_set_style_text_color(wd_local_gps_lbl, UI_ACCENT_ORANGE, 0);
        }
    }
    lv_label_set_text(wd_local_gps_lbl, buf);
}

static void start_local_gps_timer(void)
{
    if (wd_local_gps_timer) return;
    wd_local_gps_timer = lv_timer_create(local_gps_timer_cb, 1000, NULL);
}

static void stop_local_gps_timer(void)
{
    if (wd_local_gps_timer) {
        lv_timer_del(wd_local_gps_timer);
        wd_local_gps_timer = NULL;
    }
}

static void stop_gps_push_timer(void)
{
    if (wd_gps_push_timer) {
        lv_timer_del(wd_gps_push_timer);
        wd_gps_push_timer = NULL;
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
    wd_wifi_count = 0;
    wd_bt_count = 0;
    wd_sat_count = 0;
    wd_distance_m = 0.0;
    wd_running = true;
    wd_session_started = false;

    uart_set_line_callback(wd_uart_line_cb);
    uart_send_command("unselect_networks");
    uart_send_command(wd_trace_enabled ? "start_wardrive_promisc_trace"
                                       : "start_wardrive_promisc");
    ESP_LOGI(TAG, "Wardrive started (trace=%d, gps=%d)", wd_trace_enabled, wd_gps_type);

    if (wd_use_external) start_gps_push_timer();

    bsp_display_lock(0);
    set_status("Starting...", UI_ACCENT_TEAL);
    update_stats();
    update_list();
    show_gps_overlay("Acquiring GPS Fix...");
    if (start_btn) lv_obj_add_flag(start_btn, LV_OBJ_FLAG_HIDDEN);
    if (stop_btn)  lv_obj_clear_flag(stop_btn, LV_OBJ_FLAG_HIDDEN);
    bsp_display_unlock();
}

static void wardrive_stop(void)
{
    wd_running = false;
    stop_gps_push_timer();
    uart_set_line_callback(NULL);
    uart_send_command("stop");
}

static void on_stop(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Wardrive STOP");
    wardrive_stop();

    bsp_display_lock(0);
    close_gps_overlay();
    set_status("Stopped", ui_muted_color());
    if (start_btn) lv_obj_clear_flag(start_btn, LV_OBJ_FLAG_HIDDEN);
    if (stop_btn)  lv_obj_add_flag(stop_btn, LV_OBJ_FLAG_HIDDEN);
    bsp_display_unlock();
}

static void update_trace_btn(void)
{
    if (!trace_btn || !lv_obj_is_valid(trace_btn)) return;
    lv_obj_set_style_bg_color(trace_btn,
                              wd_trace_enabled ? UI_ACCENT_ORANGE : ui_card_color(), 0);
    if (trace_lbl)
        lv_label_set_text(trace_lbl, wd_trace_enabled ? "Trace:ON" : "Trace:OFF");
}

static void on_trace(lv_event_t *e)
{
    (void)e;
    if (wd_running) return;  /* can't change mid-run */
    wd_trace_enabled = !wd_trace_enabled;
    update_trace_btn();
}

static void on_gps_btn(lv_event_t *e)
{
    (void)e;
    show_gps_type_popup();
}

static void on_back(lv_event_t *e)
{
    (void)e;
    if (wd_running) wardrive_stop();
    stop_gps_push_timer();
    stop_local_gps_timer();
    close_gps_overlay();
    wd_list = NULL;
    wd_status_lbl = NULL;
    wd_stats_lbl = NULL;
    wd_local_gps_lbl = NULL;
    start_btn = NULL;
    stop_btn = NULL;
    trace_btn = NULL;
    trace_lbl = NULL;

    bsp_display_lock(0);
    show_global_attacks_screen();
    bsp_display_unlock();
}

/* ================================================================== */
/*  GPS type picker popup                                              */
/* ================================================================== */

static lv_obj_t *gps_popup = NULL;

static void close_gps_popup(void)
{
    if (gps_popup) { lv_obj_del(gps_popup); gps_popup = NULL; }
}

static void on_gps_type_pick(lv_event_t *e)
{
    wd_gps_type_t type = (wd_gps_type_t)(intptr_t)lv_event_get_user_data(e);
    wd_gps_type = type;

    switch (type) {
    case WD_GPS_M5:       uart_send_command("gps_set m5");       wd_use_external = false; break;
    case WD_GPS_ATGM:     uart_send_command("gps_set atgm");     wd_use_external = false; break;
    case WD_GPS_EXTERNAL: uart_send_command("gps_set external"); wd_use_external = true;  break;
    }

    if (wd_use_external) {
        if (!gps_module_is_running()) {
            set_status("Enable External GPS in Settings!", UI_ACCENT_ORANGE);
        } else if (wd_running) {
            start_gps_push_timer();
        }
    } else {
        stop_gps_push_timer();
    }

    close_gps_popup();
}

static void on_gps_popup_close(lv_event_t *e)
{
    (void)e;
    close_gps_popup();
}

static void show_gps_type_popup(void)
{
    if (gps_popup) return;
    lv_obj_t *scr = lv_scr_act();

    gps_popup = lv_obj_create(scr);
    lv_obj_set_size(gps_popup, 260, LV_SIZE_CONTENT);
    lv_obj_center(gps_popup);
    style_popup_card(gps_popup, 12, UI_ACCENT_TEAL);
    lv_obj_set_style_pad_all(gps_popup, 14, 0);
    lv_obj_set_flex_flow(gps_popup, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(gps_popup, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(gps_popup, 8, 0);
    lv_obj_clear_flag(gps_popup, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(gps_popup);
    lv_label_set_text(title, "GPS Source");
    lv_obj_set_style_text_color(title, ui_text_color(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);

    const char *labels[] = { "M5 GPS", "ATGM336H", "External (M5 v2.1)" };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *btn = lv_btn_create(gps_popup);
        lv_obj_set_size(btn, 220, 34);
        lv_obj_set_style_bg_color(btn, ((int)wd_gps_type == i) ? UI_ACCENT_TEAL : ui_card_color(), 0);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_add_event_cb(btn, on_gps_type_pick, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, labels[i]);
        lv_obj_set_style_text_color(lbl, ui_text_color(), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_center(lbl);
    }

    lv_obj_t *close_btn = lv_btn_create(gps_popup);
    lv_obj_set_size(close_btn, 220, 30);
    style_neutral_button(close_btn);
    lv_obj_add_event_cb(close_btn, on_gps_popup_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t *clbl = lv_label_create(close_btn);
    lv_label_set_text(clbl, "Close");
    lv_obj_set_style_text_color(clbl, ui_text_color(), 0);
    lv_obj_center(clbl);
}

/* ================================================================== */
/*  Upload flow (WiGLE / WDGWars)                                      */
/* ================================================================== */

#define WD_UP_HARD_CAP 512

typedef struct {
    char name[80];
    bool selected;
} wd_file_t;

static wd_file_t *wd_files       = NULL;
static int        wd_files_cap   = 0;
static int        wd_files_count = 0;

static wifi_network_t *wd_aps     = NULL;
static int             wd_aps_cap = 0;
static int             wd_aps_cnt = 0;

static bool      wd_upload_wigle = true;
static char      wd_up_pass[64];
static wifi_network_t wd_up_net;
static lv_obj_t *wd_up_status = NULL;

static void show_file_picker(void);
static void show_ap_picker(void);
static void start_ap_scan(void);
static void start_upload(void);

/* ---- service menu popup ---- */

static lv_obj_t *svc_popup = NULL;

static void close_svc_popup(void)
{
    if (svc_popup) { lv_obj_del(svc_popup); svc_popup = NULL; }
}

static void files_collect_cb(const char **lines, int line_count)
{
    wd_files_count = 0;
    bool header = false;
    for (int i = 0; i < line_count; i++) {
        const char *line = lines[i];
        if (strstr(line, "Files in")) { header = true; continue; }
        if (strstr(line, "Found") && strstr(line, "file(s)")) break;
        if (!header) continue;

        int num;
        char fname[80];
        if (sscanf(line, "%d %79[^\r\n]", &num, fname) == 2) {
            if (!psram_dynarr_ensure((void **)&wd_files, &wd_files_cap,
                                     wd_files_count + 1, sizeof(*wd_files),
                                     WD_UP_HARD_CAP))
                break;
            snprintf(wd_files[wd_files_count].name,
                     sizeof(wd_files[0].name), "%s", fname);
            wd_files[wd_files_count].selected = false;
            wd_files_count++;
        }
    }
    ESP_LOGI(TAG, "Wardrive files: %d", wd_files_count);
    bsp_display_lock(0);
    show_file_picker();
    bsp_display_unlock();
}

static void on_service_pick(lv_event_t *e)
{
    wd_upload_wigle = (bool)(intptr_t)lv_event_get_user_data(e);
    close_svc_popup();

    wd_files_count = 0;
    stop_local_gps_timer();
    wd_local_gps_lbl = NULL;
    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Loading files...", on_back, NULL);
    lv_obj_t *lbl = lv_label_create(scr);
    lv_label_set_text(lbl, "Reading /sdcard/lab/wardrives ...");
    lv_obj_set_style_text_color(lbl, ui_text_color(), 0);
    lv_obj_center(lbl);

    uart_start_collect("Found", files_collect_cb);
    uart_send_command("list_dir /sdcard/lab/wardrives");
}

static void on_svc_close(lv_event_t *e) { (void)e; close_svc_popup(); }

static void wardrive_upload_btn_cb(lv_event_t *e)
{
    (void)e;
    if (wd_running) {
        wardrive_stop();
        bsp_display_lock(0);
        close_gps_overlay();
        if (start_btn) lv_obj_clear_flag(start_btn, LV_OBJ_FLAG_HIDDEN);
        if (stop_btn)  lv_obj_add_flag(stop_btn, LV_OBJ_FLAG_HIDDEN);
        bsp_display_unlock();
    }

    if (svc_popup) return;
    lv_obj_t *scr = lv_scr_act();
    svc_popup = lv_obj_create(scr);
    lv_obj_set_size(svc_popup, 240, LV_SIZE_CONTENT);
    lv_obj_center(svc_popup);
    style_popup_card(svc_popup, 12, UI_ACCENT_PURPLE);
    lv_obj_set_style_pad_all(svc_popup, 14, 0);
    lv_obj_set_flex_flow(svc_popup, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(svc_popup, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(svc_popup, 8, 0);
    lv_obj_clear_flag(svc_popup, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(svc_popup);
    lv_label_set_text(title, "Upload to");
    lv_obj_set_style_text_color(title, ui_text_color(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);

    lv_obj_t *wigle = lv_btn_create(svc_popup);
    lv_obj_set_size(wigle, 200, 34);
    lv_obj_set_style_bg_color(wigle, UI_ACCENT_PURPLE, 0);
    lv_obj_set_style_radius(wigle, 8, 0);
    lv_obj_add_event_cb(wigle, on_service_pick, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    lv_obj_t *wl = lv_label_create(wigle);
    lv_label_set_text(wl, "WiGLE");
    lv_obj_set_style_text_color(wl, lv_color_white(), 0);
    lv_obj_center(wl);

    lv_obj_t *wdg = lv_btn_create(svc_popup);
    lv_obj_set_size(wdg, 200, 34);
    lv_obj_set_style_bg_color(wdg, UI_ACCENT_CYAN, 0);
    lv_obj_set_style_radius(wdg, 8, 0);
    lv_obj_add_event_cb(wdg, on_service_pick, LV_EVENT_CLICKED, (void *)(intptr_t)0);
    lv_obj_t *wdl = lv_label_create(wdg);
    lv_label_set_text(wdl, "WDGWars");
    lv_obj_set_style_text_color(wdl, lv_color_white(), 0);
    lv_obj_center(wdl);

    lv_obj_t *close_btn = lv_btn_create(svc_popup);
    lv_obj_set_size(close_btn, 200, 28);
    style_neutral_button(close_btn);
    lv_obj_add_event_cb(close_btn, on_svc_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(close_btn);
    lv_label_set_text(cl, "Cancel");
    lv_obj_set_style_text_color(cl, ui_text_color(), 0);
    lv_obj_center(cl);
}

/* ---- file picker ---- */

static void on_file_toggle(lv_event_t *e)
{
    lv_obj_t *cb = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < wd_files_count)
        wd_files[idx].selected = lv_obj_has_state(cb, LV_STATE_CHECKED);
}

static void on_files_back(lv_event_t *e)
{
    (void)e;
    bsp_display_lock(0);
    show_wardrive_screen();
    bsp_display_unlock();
}

static void on_files_next(lv_event_t *e)
{
    (void)e;
    int sel = 0;
    for (int i = 0; i < wd_files_count; i++) if (wd_files[i].selected) sel++;
    if (sel == 0) {
        bsp_display_lock(0);
        if (wd_up_status) lv_label_set_text(wd_up_status, "Select at least one file.");
        bsp_display_unlock();
        return;
    }
    start_ap_scan();
}

static void show_file_picker(void)
{
    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, wd_upload_wigle ? "WiGLE Files" : "WDGWars Files",
                      on_files_back, NULL);

    wd_up_status = lv_label_create(scr);
    lv_label_set_text(wd_up_status,
                      wd_files_count ? "Select files to upload:" : "No wardrive files found.");
    lv_obj_set_style_text_color(wd_up_status, ui_muted_color(), 0);
    lv_obj_set_style_text_font(wd_up_status, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(wd_up_status, 8, 40);

    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_set_size(list, LV_PCT(100), 240 - 96);
    lv_obj_set_pos(list, 0, 58);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 6, 0);
    lv_obj_set_style_pad_row(list, 4, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    for (int i = 0; i < wd_files_count; i++) {
        lv_obj_t *cb = lv_checkbox_create(list);
        lv_checkbox_set_text(cb, wd_files[i].name);
        lv_obj_set_style_text_color(cb, ui_text_color(), 0);
        lv_obj_set_style_text_font(cb, &lv_font_montserrat_12, 0);
        if (wd_files[i].selected) lv_obj_add_state(cb, LV_STATE_CHECKED);
        lv_obj_add_event_cb(cb, on_file_toggle, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
    }

    lv_obj_t *next = lv_btn_create(scr);
    lv_obj_set_size(next, 120, 30);
    lv_obj_set_pos(next, 320 - 128, 240 - 34);
    lv_obj_set_style_bg_color(next, UI_ACCENT_GREEN, 0);
    lv_obj_set_style_radius(next, 8, 0);
    lv_obj_add_event_cb(next, on_files_next, LV_EVENT_CLICKED, NULL);
    lv_obj_t *nl = lv_label_create(next);
    lv_label_set_text(nl, "Connect WiFi");
    lv_obj_set_style_text_color(nl, lv_color_white(), 0);
    lv_obj_set_style_text_font(nl, &lv_font_montserrat_12, 0);
    lv_obj_center(nl);
}

/* ---- AP scan + picker ---- */

static const char *ap_parse_quoted(const char *p, char *out, int max)
{
    if (*p != '"') return NULL;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < max - 1) out[i++] = *p++;
    out[i] = '\0';
    if (*p != '"') return NULL;
    p++;
    if (*p == ',') p++;
    return p;
}

static bool ap_parse_line(const char *line, wifi_network_t *net)
{
    if (line[0] != '"') return false;
    const char *p = line;
    char field[64];
    memset(net, 0, sizeof(*net));

    p = ap_parse_quoted(p, field, sizeof(field));       if (!p) return false;
    net->index = (uint8_t)atoi(field);
    p = ap_parse_quoted(p, net->ssid, sizeof(net->ssid)); if (!p) return false;
    p = ap_parse_quoted(p, field, sizeof(field));        if (!p) return false; /* empty */
    p = ap_parse_quoted(p, net->bssid, sizeof(net->bssid)); if (!p) return false;
    p = ap_parse_quoted(p, field, sizeof(field));        if (!p) return false;
    net->channel = (uint8_t)atoi(field);
    p = ap_parse_quoted(p, net->security, sizeof(net->security)); if (!p) return false;
    p = ap_parse_quoted(p, field, sizeof(field));        if (!p) return false;
    net->rssi = (int8_t)atoi(field);
    return true;
}

static void ap_collect_cb(const char **lines, int line_count)
{
    wd_aps_cnt = 0;
    for (int i = 0; i < line_count; i++) {
        wifi_network_t net;
        if (ap_parse_line(lines[i], &net)) {
            if (net.ssid[0] == '\0') continue;
            if (!psram_dynarr_ensure((void **)&wd_aps, &wd_aps_cap,
                                     wd_aps_cnt + 1, sizeof(*wd_aps), 256))
                break;
            wd_aps[wd_aps_cnt++] = net;
        }
    }
    ESP_LOGI(TAG, "Wardrive upload: %d APs", wd_aps_cnt);
    bsp_display_lock(0);
    show_ap_picker();
    bsp_display_unlock();
}

static void on_connect_done(bool success, void *unused)
{
    (void)unused;
    if (!success) {
        bsp_display_lock(0);
        if (wd_up_status && lv_obj_is_valid(wd_up_status)) {
            lv_label_set_text(wd_up_status, "WiFi connect failed.");
            lv_obj_set_style_text_color(wd_up_status, UI_ACCENT_RED, 0);
        }
        bsp_display_unlock();
        return;
    }
    start_upload();
}

static void on_pass_confirm(const char *text, void *unused)
{
    (void)unused;
    if (text) snprintf(wd_up_pass, sizeof(wd_up_pass), "%s", text);
    bsp_display_lock(0);
    if (wd_up_status && lv_obj_is_valid(wd_up_status))
        lv_label_set_text(wd_up_status, "Connecting...");
    bsp_display_unlock();
    wifi_connect_async(&wd_up_net, wd_up_pass[0] ? wd_up_pass : NULL, on_connect_done, NULL);
}

static void on_ap_pick(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= wd_aps_cnt) return;
    wd_up_net = wd_aps[idx];
    wd_up_pass[0] = '\0';

    if (wifi_network_is_open(&wd_up_net)) {
        bsp_display_lock(0);
        if (wd_up_status && lv_obj_is_valid(wd_up_status))
            lv_label_set_text(wd_up_status, "Connecting...");
        bsp_display_unlock();
        wifi_connect_async(&wd_up_net, NULL, on_connect_done, NULL);
        return;
    }
    char evil[64] = {0};
    if (wifi_lookup_evil_password(wd_up_net.ssid, evil, sizeof(evil))) {
        snprintf(wd_up_pass, sizeof(wd_up_pass), "%s", evil);
        wifi_connect_async(&wd_up_net, wd_up_pass, on_connect_done, NULL);
        return;
    }
    ui_show_text_input_popup("WiFi Password", "", 63, UI_ACCENT_GREEN,
                             on_pass_confirm, NULL, NULL);
}

static void on_ap_back(lv_event_t *e)
{
    (void)e;
    bsp_display_lock(0);
    show_file_picker();
    bsp_display_unlock();
}

static void on_ap_rescan(lv_event_t *e);

static void show_ap_picker(void)
{
    lv_obj_t *scr = ui_screen_clear();
    lv_obj_t *bar = ui_create_top_bar(scr, "Select WiFi", on_ap_back, NULL);
    ui_add_top_bar_action(bar, LV_SYMBOL_REFRESH, on_ap_rescan, NULL);

    wd_up_status = lv_label_create(scr);
    lv_label_set_text(wd_up_status, wd_aps_cnt ? "Pick an access point:" : "No networks found.");
    lv_obj_set_style_text_color(wd_up_status, ui_muted_color(), 0);
    lv_obj_set_style_text_font(wd_up_status, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(wd_up_status, 8, 40);

    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_set_size(list, LV_PCT(100), 240 - 60);
    lv_obj_set_pos(list, 0, 58);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 6, 0);
    lv_obj_set_style_pad_row(list, 3, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    for (int i = 0; i < wd_aps_cnt; i++) {
        lv_obj_t *btn = lv_btn_create(list);
        lv_obj_set_size(btn, LV_PCT(100), 30);
        lv_obj_set_style_bg_color(btn, ui_panel_color(), 0);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_add_event_cb(btn, on_ap_pick, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *lbl = lv_label_create(btn);
        char buf[64];
        snprintf(buf, sizeof(buf), "%.24s  %ddBm%s", wd_aps[i].ssid, wd_aps[i].rssi,
                 wifi_network_is_open(&wd_aps[i]) ? "  OPEN" : "");
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_color(lbl, ui_text_color(), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_center(lbl);
    }
}

static void start_ap_scan(void)
{
    wd_aps_cnt = 0;
    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Scanning WiFi", on_ap_back, NULL);
    wd_up_status = lv_label_create(scr);
    lv_label_set_text(wd_up_status, "Scanning networks...");
    lv_obj_set_style_text_color(wd_up_status, ui_text_color(), 0);
    lv_obj_center(wd_up_status);

    uart_start_collect("Scan results printed", ap_collect_cb);
    uart_send_command("scan_networks");
}

static void on_ap_rescan(lv_event_t *e) { (void)e; start_ap_scan(); }

/* ---- upload progress ---- */

static lv_obj_t *wd_up_log = NULL;

static void upload_line_cb(const char *line)
{
    if (!line || !line[0]) return;
    bsp_display_lock(0);
    if (wd_up_log && lv_obj_is_valid(wd_up_log)) {
        lv_obj_t *l = lv_label_create(wd_up_log);
        lv_label_set_text(l, line);
        lv_obj_set_style_text_color(l, ui_text_color(), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_10, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, LV_PCT(100));
        lv_obj_scroll_to_view(l, LV_ANIM_OFF);
    }
    bsp_display_unlock();

    if (strstr(line, "Upload complete") || strstr(line, "uploaded") ||
        strstr(line, "Unrecognized command") || strstr(line, "Done") ||
        strstr(line, "FAILED") || strstr(line, "Error")) {
        uart_set_line_callback(NULL);
        bsp_display_lock(0);
        if (wd_up_status && lv_obj_is_valid(wd_up_status)) {
            lv_label_set_text(wd_up_status, "Finished.");
            lv_obj_set_style_text_color(wd_up_status, UI_ACCENT_GREEN, 0);
        }
        bsp_display_unlock();
    }
}

static void on_upload_done_back(lv_event_t *e)
{
    (void)e;
    uart_set_line_callback(NULL);
    uart_send_command("stop");
    bsp_display_lock(0);
    show_wardrive_screen();
    bsp_display_unlock();
}

static void start_upload(void)
{
    /* build command with selected filenames */
    char cmd[256];
    int off = snprintf(cmd, sizeof(cmd), "%s",
                       wd_upload_wigle ? "wigle_upload" : "wdgwars_upload");
    for (int i = 0; i < wd_files_count && off < (int)sizeof(cmd) - 1; i++) {
        if (!wd_files[i].selected) continue;
        off += snprintf(cmd + off, sizeof(cmd) - off, " %s", wd_files[i].name);
    }

    bsp_display_lock(0);
    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Uploading", on_upload_done_back, NULL);

    wd_up_status = lv_label_create(scr);
    lv_label_set_text(wd_up_status, "Connected. Uploading...");
    lv_obj_set_style_text_color(wd_up_status, UI_ACCENT_GREEN, 0);
    lv_obj_set_style_text_font(wd_up_status, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(wd_up_status, 8, 40);

    wd_up_log = lv_obj_create(scr);
    lv_obj_set_size(wd_up_log, LV_PCT(100), 240 - 60);
    lv_obj_set_pos(wd_up_log, 0, 58);
    lv_obj_set_style_bg_color(wd_up_log, ui_card_color(), 0);
    lv_obj_set_style_bg_opa(wd_up_log, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(wd_up_log, 0, 0);
    lv_obj_set_style_pad_all(wd_up_log, 4, 0);
    lv_obj_set_style_pad_row(wd_up_log, 1, 0);
    lv_obj_set_flex_flow(wd_up_log, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(wd_up_log, LV_DIR_VER);
    bsp_display_unlock();

    uart_set_line_callback(upload_line_cb);
    uart_handler_flush_rx();
    uart_send_command(cmd);
    ESP_LOGI(TAG, "Upload cmd: %s", cmd);
}

/* ================================================================== */
/*  Main page                                                          */
/* ================================================================== */

void show_wardrive_screen(void)
{
    wd_ring_head = 0;
    wd_ring_count = 0;
    wd_wifi_count = 0;
    wd_bt_count = 0;
    wd_sat_count = 0;
    wd_distance_m = 0.0;
    gps_overlay = NULL;
    gps_overlay_lbl = NULL;
    gps_popup = NULL;
    wd_local_gps_lbl = NULL;

    lv_obj_t *scr = ui_screen_clear();
    lv_obj_t *bar = ui_create_top_bar(scr, "Wardrive", on_back, NULL);
    ui_add_top_bar_action(bar, LV_SYMBOL_UPLOAD, wardrive_upload_btn_cb, NULL);
    ui_add_top_bar_action(bar, LV_SYMBOL_GPS, on_gps_btn, NULL);

    /* status + stats + local GPS stacked */
    lv_obj_t *info = lv_obj_create(scr);
    lv_obj_set_size(info, LV_PCT(100), 52);
    lv_obj_set_pos(info, 0, 36);
    lv_obj_set_style_bg_opa(info, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(info, 0, 0);
    lv_obj_set_style_pad_hor(info, 8, 0);
    lv_obj_set_style_pad_ver(info, 2, 0);
    lv_obj_set_style_pad_row(info, 0, 0);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(info, LV_OBJ_FLAG_SCROLLABLE);

    wd_status_lbl = lv_label_create(info);
    lv_label_set_text(wd_status_lbl, "Ready");
    lv_obj_set_style_text_color(wd_status_lbl, UI_ACCENT_TEAL, 0);
    lv_obj_set_style_text_font(wd_status_lbl, &lv_font_montserrat_12, 0);

    wd_stats_lbl = lv_label_create(info);
    lv_label_set_text(wd_stats_lbl, "WiFi:0  BT:0  SAT:0  0.00km");
    lv_obj_set_style_text_color(wd_stats_lbl, ui_muted_color(), 0);
    lv_obj_set_style_text_font(wd_stats_lbl, &lv_font_montserrat_12, 0);

    wd_local_gps_lbl = lv_label_create(info);
    lv_label_set_text(wd_local_gps_lbl, "GPS(local): ...");
    lv_obj_set_style_text_color(wd_local_gps_lbl, ui_muted_color(), 0);
    lv_obj_set_style_text_font(wd_local_gps_lbl, &lv_font_montserrat_10, 0);

    /* button row: Start / Stop / Trace */
    lv_obj_t *btn_row = lv_obj_create(scr);
    lv_obj_set_size(btn_row, LV_PCT(100), 34);
    lv_obj_set_pos(btn_row, 0, 90);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_set_style_pad_column(btn_row, 6, 0);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    start_btn = lv_btn_create(btn_row);
    lv_obj_set_size(start_btn, 96, 30);
    lv_obj_set_style_bg_color(start_btn, UI_ACCENT_GREEN, 0);
    lv_obj_set_style_radius(start_btn, 8, 0);
    lv_obj_add_event_cb(start_btn, on_start, LV_EVENT_CLICKED, NULL);
    lv_obj_t *start_lbl = lv_label_create(start_btn);
    lv_label_set_text(start_lbl, LV_SYMBOL_PLAY " Start");
    lv_obj_set_style_text_color(start_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(start_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(start_lbl);

    stop_btn = lv_btn_create(btn_row);
    lv_obj_set_size(stop_btn, 96, 30);
    lv_obj_set_style_bg_color(stop_btn, UI_ACCENT_RED, 0);
    lv_obj_set_style_radius(stop_btn, 8, 0);
    lv_obj_add_event_cb(stop_btn, on_stop, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(stop_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *stop_lbl = lv_label_create(stop_btn);
    lv_label_set_text(stop_lbl, LV_SYMBOL_CLOSE " Stop");
    lv_obj_set_style_text_color(stop_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(stop_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(stop_lbl);

    trace_btn = lv_btn_create(btn_row);
    lv_obj_set_size(trace_btn, 100, 30);
    lv_obj_set_style_radius(trace_btn, 8, 0);
    lv_obj_add_event_cb(trace_btn, on_trace, LV_EVENT_CLICKED, NULL);
    trace_lbl = lv_label_create(trace_btn);
    lv_obj_set_style_text_color(trace_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(trace_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(trace_lbl);
    update_trace_btn();

    /* network card list */
    wd_list = lv_obj_create(scr);
    lv_obj_set_size(wd_list, LV_PCT(100), 240 - 126);
    lv_obj_set_pos(wd_list, 0, 126);
    lv_obj_set_style_bg_color(wd_list, ui_card_color(), 0);
    lv_obj_set_style_bg_opa(wd_list, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(wd_list, 0, 0);
    lv_obj_set_style_border_width(wd_list, 0, 0);
    lv_obj_set_style_pad_all(wd_list, 4, 0);
    lv_obj_set_style_pad_row(wd_list, 3, 0);
    lv_obj_set_flex_flow(wd_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(wd_list, LV_DIR_VER);

    update_list();
    start_local_gps_timer();
}
