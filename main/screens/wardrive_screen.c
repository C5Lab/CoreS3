#include "wardrive_screen.h"
#include "home_screen.h"
#include "wifi_scan_screen.h"
#include "wifi_connect_helper.h"
#include "ui_helpers.h"
#include "uart_handler.h"
#include "gps_module.h"
#include "psram_dynarr.h"
#include "bsp/m5stack_core_s3.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <stdint.h>
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
    WD_GPS_M5 = 0,   /* m5           */
    WD_GPS_ATGM,     /* atgm         */
    WD_GPS_EXTERNAL, /* external/tab5 (host pushes set_gps_position) */
    WD_GPS_CAP,      /* cap / external_cap */
} wd_gps_type_t;

/* ------------------------------------------------------------------ */
/*  Wardrive 2.0 configuration (mirrors JanOS get_wardrive_config /     */
/*  set_wardrive_* commands; ported from the Tab5 setup screen).        */
/* ------------------------------------------------------------------ */
#define WD_BAND_WIFI24    0x01
#define WD_BAND_WIFI5     0x02
#define WD_BAND_BLE       0x04
#define WD_CUSTOM_CH_MAX  96
#define WD_GPS_DEBUG_LOG_SIZE 4096

typedef enum {
    WD_CH_POPULAR = 0,
    WD_CH_ALL     = 1,
    WD_CH_CUSTOM  = 2,
} wd_channel_mode_t;

typedef enum {
    WD_ANTISURV_LOW  = 0,
    WD_ANTISURV_MED  = 1,
    WD_ANTISURV_HIGH = 2,
} wd_antisurv_t;

typedef struct {
    uint8_t           bands;                          /* WD_BAND_* bitmask */
    wd_channel_mode_t channel_mode;
    char              custom_channels[WD_CUSTOM_CH_MAX]; /* e.g. "1:6:11:36" */
    int               wifi_rssi_delta;                /* 0-50, 0 = log once */
    int               ble_rssi_delta;                 /* 0-50 */
    int               startup_cooldown;               /* 0-600 s */
    int               mem_cap;                         /* 1000-200000 */
    wd_antisurv_t     antisurv;
    bool              loaded;                          /* populated from device */
} wd_config_t;

static const char *wd_memcap_options = "10000\n20000\n40000\n80000\n120000\n200000";
static const int   wd_memcap_values[] = { 10000, 20000, 40000, 80000, 120000, 200000 };

static bool          wd_running        = false;
/* True once the *current* session's "Promiscuous wardrive started" was seen.
 * Starting a new wardrive makes the firmware tear down the prior run first,
 * which emits a "Wardrive promisc stopped" line; we must ignore that until
 * the new session has actually started. */
static bool          wd_session_started = false;
static bool          wd_trace_enabled  = true;   /* matches Tab5 default */
static wd_gps_type_t wd_gps_type       = WD_GPS_M5;
static bool          wd_use_external   = false;

/* Live wardrive config + setup-overlay state. */
static wd_config_t   wd_config;
static bool          wd_setup_gps_dirty = false;   /* GPS dd changed by user */
static volatile bool wd_setup_applying  = false;   /* apply worker running */

static lv_obj_t *wd_list        = NULL;   /* scrollable network card list */
static lv_obj_t *wd_status_lbl  = NULL;
static lv_obj_t *wd_stats_lbl   = NULL;
static lv_obj_t *start_btn      = NULL;
static lv_obj_t *stop_btn       = NULL;
static lv_obj_t *trace_btn      = NULL;
static lv_obj_t *trace_lbl      = NULL;
static lv_obj_t *gps_overlay    = NULL;
static lv_obj_t *gps_overlay_lbl = NULL;

/* Setup overlay controls. */
static lv_obj_t *wd_setup_overlay   = NULL;
static lv_obj_t *wd_setup_trace_sw  = NULL;
static lv_obj_t *wd_setup_gps_dd    = NULL;
static lv_obj_t *wd_setup_band_cb[3] = { NULL, NULL, NULL };
static lv_obj_t *wd_setup_channel_dd = NULL;
static lv_obj_t *wd_setup_custom_btn = NULL;
static lv_obj_t *wd_setup_custom_lbl = NULL;
static lv_obj_t *wd_setup_wifi_slider = NULL, *wd_setup_wifi_val = NULL;
static lv_obj_t *wd_setup_ble_slider  = NULL, *wd_setup_ble_val  = NULL;
static lv_obj_t *wd_setup_cd_slider   = NULL, *wd_setup_cd_val   = NULL;
static lv_obj_t *wd_setup_memcap_dd   = NULL;
static lv_obj_t *wd_setup_antisurv_dd = NULL;
static lv_obj_t *wd_setup_status      = NULL;
static lv_obj_t *wd_setup_load_btn    = NULL;
static lv_obj_t *wd_setup_apply_btn   = NULL;
static lv_obj_t *wd_setup_close_btn   = NULL;
static lv_obj_t *wd_setup_gps_debug_btn = NULL;

/* GPS raw-NMEA debug popup (start_gps_raw) — sub-overlay of the setup popup. */
static lv_obj_t *wd_gps_dbg_overlay   = NULL;
static lv_obj_t *wd_gps_dbg_fix_lbl   = NULL;
static lv_obj_t *wd_gps_dbg_sat_lbl   = NULL;
static lv_obj_t *wd_gps_dbg_hdop_lbl  = NULL;
static lv_obj_t *wd_gps_dbg_presence_lbl = NULL;
static lv_obj_t *wd_gps_dbg_antenna_lbl  = NULL;
static lv_obj_t *wd_gps_dbg_coord_lbl = NULL;
static lv_obj_t *wd_gps_dbg_log_box   = NULL;
static lv_obj_t *wd_gps_dbg_log_lbl   = NULL;
static lv_obj_t *wd_gps_dbg_start_btn = NULL;
static lv_obj_t *wd_gps_dbg_stop_btn  = NULL;
static lv_obj_t *wd_gps_dbg_close_btn = NULL;
static volatile bool wd_gps_dbg_running        = false;
static volatile bool wd_gps_dbg_stop_requested = false;
static char *wd_gps_dbg_log = NULL;   /* PSRAM, allocated lazily on first open */
/* Refresh throttle + last-shown diagnostic states. The NMEA stream is many
 * lines/sec; repainting the (multi-KB, word-wrapped) log label on every line
 * thrashes the PSRAM heap and the layout engine, which eventually starves RAM
 * and hangs the core. We keep the ring buffer current on every line but only
 * push it to the label a few times a second, and only touch the tiny status
 * labels when their value actually changes. */
#define WD_GPS_DEBUG_UI_MIN_MS 250
static uint32_t wd_gps_dbg_last_ui = 0;
static int wd_gps_dbg_presence_state = -1;  /* -1 unset, 0 checking, 1 detected, 2 missing */
static int wd_gps_dbg_antenna_state  = -1;  /* -1 unset, 0 checking, 1 ok, 2 missing */

static lv_timer_t *wd_gps_push_timer = NULL;

/* forward decls */
static void show_wardrive_setup(void);
static void wd_setup_close(void);
static void start_gps_push_timer(void);
static void stop_gps_push_timer(void);
static void set_status(const char *txt, lv_color_t color);
static void update_trace_btn(void);
static void wardrive_upload_btn_cb(lv_event_t *e);
static void wd_gps_debug_btn_cb(lv_event_t *e);
static void wd_gps_debug_start_cb(lv_event_t *e);
static void wd_gps_debug_stop_cb(lv_event_t *e);
static void wd_gps_debug_close_cb(lv_event_t *e);

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
    snprintf(buf, sizeof(buf), "WiFi:%d BT:%d SAT:%d %.2fkm",
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
    show_wardrive_setup();
}

static void on_back(lv_event_t *e)
{
    (void)e;
    if (wd_setup_applying) return;   /* don't leave while a worker task runs */
    wd_setup_close();
    if (wd_running) wardrive_stop();
    stop_gps_push_timer();
    close_gps_overlay();
    wd_list = NULL;
    wd_status_lbl = NULL;
    wd_stats_lbl = NULL;
    start_btn = NULL;
    stop_btn = NULL;
    trace_btn = NULL;
    trace_lbl = NULL;

    bsp_display_lock(0);
    show_home_screen();
    bsp_display_unlock();
}

/* ================================================================== */
/*  Wardrive 2.0 setup overlay  (ported from Tab5 main.c)              */
/* ================================================================== */

static void wd_config_set_defaults(wd_config_t *cfg)
{
    if (!cfg) return;
    cfg->bands            = WD_BAND_WIFI24 | WD_BAND_WIFI5 | WD_BAND_BLE;
    cfg->channel_mode     = WD_CH_ALL;
    cfg->custom_channels[0] = '\0';
    cfg->wifi_rssi_delta  = 5;
    cfg->ble_rssi_delta   = 15;
    cfg->startup_cooldown = 0;
    cfg->mem_cap          = 40000;
    cfg->antisurv         = WD_ANTISURV_MED;
    cfg->loaded           = false;
}

static const char *wd_gps_cmd_for_type(wd_gps_type_t t)
{
    switch (t) {
    case WD_GPS_M5:   return "m5";
    case WD_GPS_ATGM: return "atgm";
    case WD_GPS_CAP:  return "cap";
    default:          return "external";
    }
}

static int wd_memcap_to_index(int memcap)
{
    int best = 2, best_diff = 1 << 30;   /* default 40000 */
    for (int i = 0; i < (int)(sizeof(wd_memcap_values) / sizeof(int)); i++) {
        int diff = abs(memcap - wd_memcap_values[i]);
        if (diff < best_diff) { best_diff = diff; best = i; }
    }
    return best;
}

/* Send `cmd` and block until a line containing `ack` arrives. Worker-thread
 * only (uart_send_wait_line blocks on a semaphore). */
static bool wd_send_set(const char *cmd, const char *ack)
{
    char buf[192];
    return uart_send_wait_line(cmd, ack, buf, sizeof(buf), 1500);
}

/* ---- Query the firmware's active GPS module ("gps_set" no args) ---- */
static bool wd_load_gps_module(void)
{
    char resp[192];
    if (!uart_send_wait_line("gps_set", "Current GPS module", resp, sizeof(resp), 1500))
        return false;

    if (strstr(resp, "M5Stack") || strstr(resp, "M5STACK")) {
        wd_gps_type = WD_GPS_M5;
    } else if (strstr(resp, "ATGM") || strstr(resp, "atgm")) {
        wd_gps_type = WD_GPS_ATGM;
    } else if (strstr(resp, "ExternalCap") || strstr(resp, "external_cap") ||
               strstr(resp, "cap")) {
        wd_gps_type = WD_GPS_CAP;
    } else if (strstr(resp, "External") || strstr(resp, "external") ||
               strstr(resp, "tab5")) {
        wd_gps_type = WD_GPS_EXTERNAL;
    } else {
        return false;
    }
    wd_setup_gps_dirty = false;
    wd_use_external = (wd_gps_type == WD_GPS_EXTERNAL);
    return true;
}

/* ---- Load config via get_wardrive_config ([WDCFG] ... [WDCFG] END) ---- */
static SemaphoreHandle_t wd_cfg_sem = NULL;
static wd_config_t       wd_cfg_scratch;
static volatile bool     wd_cfg_ok;

static void wd_cfg_collect_cb(const char **lines, int count)
{
    wd_config_t cfg;
    wd_config_set_defaults(&cfg);
    bool any = false;

    for (int i = 0; i < count; i++) {
        const char *p = strstr(lines[i], "[WDCFG]");
        if (!p) continue;
        p += 7;                          /* strlen("[WDCFG]") */
        while (*p == ' ') p++;
        if (strncmp(p, "bands=", 6) == 0) {
            cfg.bands = 0;
            if (strstr(p, "wifi24")) cfg.bands |= WD_BAND_WIFI24;
            if (strstr(p, "wifi5"))  cfg.bands |= WD_BAND_WIFI5;
            if (strstr(p, "ble"))    cfg.bands |= WD_BAND_BLE;
            any = true;
        } else if (strncmp(p, "channels=", 9) == 0) {
            const char *v = p + 9;
            if (strncmp(v, "popular", 7) == 0)     cfg.channel_mode = WD_CH_POPULAR;
            else if (strncmp(v, "custom", 6) == 0) cfg.channel_mode = WD_CH_CUSTOM;
            else                                    cfg.channel_mode = WD_CH_ALL;
        } else if (strncmp(p, "custom=", 7) == 0) {
            snprintf(cfg.custom_channels, sizeof(cfg.custom_channels), "%s", p + 7);
        } else if (strncmp(p, "wifi_rssi_delta=", 16) == 0) {
            cfg.wifi_rssi_delta = atoi(p + 16);
        } else if (strncmp(p, "ble_rssi_delta=", 15) == 0) {
            cfg.ble_rssi_delta = atoi(p + 15);
        } else if (strncmp(p, "startup_cooldown=", 17) == 0) {
            cfg.startup_cooldown = atoi(p + 17);
        } else if (strncmp(p, "mem_cap=", 8) == 0) {
            cfg.mem_cap = atoi(p + 8);
        } else if (strncmp(p, "antisurv_sensitivity=", 21) == 0) {
            const char *v = p + 21;
            if (strncmp(v, "low", 3) == 0)       cfg.antisurv = WD_ANTISURV_LOW;
            else if (strncmp(v, "high", 4) == 0) cfg.antisurv = WD_ANTISURV_HIGH;
            else                                  cfg.antisurv = WD_ANTISURV_MED;
        }
    }

    if (any) { cfg.loaded = true; wd_cfg_scratch = cfg; }
    wd_cfg_ok = any;
    if (wd_cfg_sem) xSemaphoreGive(wd_cfg_sem);
}

static bool wd_load_config(void)
{
    if (!wd_cfg_sem) {
        wd_cfg_sem = xSemaphoreCreateBinary();
        if (!wd_cfg_sem) return false;
    }
    xSemaphoreTake(wd_cfg_sem, 0);       /* drain any stale signal */
    wd_cfg_ok = false;
    uart_start_collect("[WDCFG] END", wd_cfg_collect_cb);
    uart_send_command("get_wardrive_config");
    if (xSemaphoreTake(wd_cfg_sem, pdMS_TO_TICKS(3000)) != pdTRUE) {
        uart_stop_collect();
        return false;
    }
    if (wd_cfg_ok) wd_config = wd_cfg_scratch;
    return wd_cfg_ok;
}

/* ---- Push current config values into the overlay controls (LVGL locked) ---- */
static void wd_setup_sync_controls(void)
{
    if (!wd_setup_overlay || !lv_obj_is_valid(wd_setup_overlay)) return;
    wd_config_t *cfg = &wd_config;

    if (wd_setup_trace_sw) {
        if (wd_trace_enabled) lv_obj_add_state(wd_setup_trace_sw, LV_STATE_CHECKED);
        else                  lv_obj_clear_state(wd_setup_trace_sw, LV_STATE_CHECKED);
    }
    if (wd_setup_gps_dd) {
        lv_dropdown_set_selected(wd_setup_gps_dd, (uint16_t)wd_gps_type);
        wd_setup_gps_dirty = false;
    }
    for (int i = 0; i < 3; i++) {
        if (!wd_setup_band_cb[i]) continue;
        uint8_t mask = (i == 0) ? WD_BAND_WIFI24 : (i == 1) ? WD_BAND_WIFI5 : WD_BAND_BLE;
        if (cfg->bands & mask) lv_obj_add_state(wd_setup_band_cb[i], LV_STATE_CHECKED);
        else                   lv_obj_clear_state(wd_setup_band_cb[i], LV_STATE_CHECKED);
    }
    if (wd_setup_channel_dd)
        lv_dropdown_set_selected(wd_setup_channel_dd, (uint16_t)cfg->channel_mode);
    if (wd_setup_custom_lbl)
        lv_label_set_text_fmt(wd_setup_custom_lbl, "Custom: %s",
                              cfg->custom_channels[0] ? cfg->custom_channels : "(none)");
    if (wd_setup_custom_btn) {
        if (cfg->channel_mode == WD_CH_CUSTOM)
            lv_obj_clear_flag(wd_setup_custom_btn, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(wd_setup_custom_btn, LV_OBJ_FLAG_HIDDEN);
    }
    if (wd_setup_wifi_slider) {
        lv_slider_set_value(wd_setup_wifi_slider, cfg->wifi_rssi_delta, LV_ANIM_OFF);
        if (wd_setup_wifi_val)
            lv_label_set_text_fmt(wd_setup_wifi_val, "%d dBm%s", cfg->wifi_rssi_delta,
                                  cfg->wifi_rssi_delta == 0 ? " (once)" : "");
    }
    if (wd_setup_ble_slider) {
        lv_slider_set_value(wd_setup_ble_slider, cfg->ble_rssi_delta, LV_ANIM_OFF);
        if (wd_setup_ble_val)
            lv_label_set_text_fmt(wd_setup_ble_val, "%d dBm%s", cfg->ble_rssi_delta,
                                  cfg->ble_rssi_delta == 0 ? " (once)" : "");
    }
    if (wd_setup_cd_slider) {
        lv_slider_set_value(wd_setup_cd_slider, cfg->startup_cooldown, LV_ANIM_OFF);
        if (wd_setup_cd_val)
            lv_label_set_text_fmt(wd_setup_cd_val, "%d s", cfg->startup_cooldown);
    }
    if (wd_setup_memcap_dd)
        lv_dropdown_set_selected(wd_setup_memcap_dd, (uint16_t)wd_memcap_to_index(cfg->mem_cap));
    if (wd_setup_antisurv_dd)
        lv_dropdown_set_selected(wd_setup_antisurv_dd, (uint16_t)cfg->antisurv);
}

static void wd_setup_enable_buttons(bool enable)
{
    lv_obj_t *btns[] = { wd_setup_load_btn, wd_setup_apply_btn, wd_setup_close_btn };
    for (int i = 0; i < 3; i++) {
        if (!btns[i] || !lv_obj_is_valid(btns[i])) continue;
        if (enable) lv_obj_clear_state(btns[i], LV_STATE_DISABLED);
        else        lv_obj_add_state(btns[i], LV_STATE_DISABLED);
    }
}

static void wd_setup_set_status(const char *txt, lv_color_t color)
{
    if (wd_setup_status && lv_obj_is_valid(wd_setup_status)) {
        lv_label_set_text(wd_setup_status, txt);
        lv_obj_set_style_text_color(wd_setup_status, color, 0);
    }
}

/* ---- Load worker: query device, then refresh controls ---- */
static void wd_setup_load_task(void *arg)
{
    (void)arg;
    bool cfg_ok = wd_load_config();
    bool gps_ok = wd_load_gps_module();

    bsp_display_lock(0);
    wd_setup_sync_controls();
    if (cfg_ok && gps_ok)
        wd_setup_set_status("Loaded config + GPS from device", UI_ACCENT_GREEN);
    else if (cfg_ok)
        wd_setup_set_status("Loaded config; GPS not reported", UI_ACCENT_ORANGE);
    else if (gps_ok)
        wd_setup_set_status("Loaded GPS; config not reported", UI_ACCENT_ORANGE);
    else
        wd_setup_set_status("No device response - local values", UI_ACCENT_RED);
    wd_setup_enable_buttons(true);
    bsp_display_unlock();

    wd_setup_applying = false;
    vTaskDelete(NULL);
}

/* ---- Apply worker: send every set_* command, waiting for each ACK ---- */
static void wd_setup_apply_task(void *arg)
{
    (void)arg;
    wd_config_t *cfg = &wd_config;
    char cmd[192];
    int  ok = 0, total = 0;
    const bool apply_gps = wd_setup_gps_dirty;

    if (apply_gps) {
        snprintf(cmd, sizeof(cmd), "gps_set %s", wd_gps_cmd_for_type(wd_gps_type));
        bsp_display_lock(0); wd_setup_set_status("Applying: GPS module", UI_ACCENT_ORANGE); bsp_display_unlock();
        total++;
        if (wd_send_set(cmd, "GPS module")) { ok++; wd_use_external = (wd_gps_type == WD_GPS_EXTERNAL); }
    }

    /* bands */
    {
        char list[48] = "";
        if (cfg->bands & WD_BAND_WIFI24) strcat(list, "wifi24,");
        if (cfg->bands & WD_BAND_WIFI5)  strcat(list, "wifi5,");
        if (cfg->bands & WD_BAND_BLE)    strcat(list, "ble,");
        size_t l = strlen(list);
        if (l && list[l - 1] == ',') list[l - 1] = '\0';
        snprintf(cmd, sizeof(cmd), "set_wardrive_bands %s", list);
        bsp_display_lock(0); wd_setup_set_status("Applying: bands", UI_ACCENT_ORANGE); bsp_display_unlock();
        total++; if (wd_send_set(cmd, "Wardrive bands")) ok++;
    }
    /* channels */
    {
        const char *mode = cfg->channel_mode == WD_CH_POPULAR ? "popular" :
                           cfg->channel_mode == WD_CH_CUSTOM  ? "custom"  : "all";
        if (cfg->channel_mode == WD_CH_CUSTOM && cfg->custom_channels[0])
            snprintf(cmd, sizeof(cmd), "set_wardrive_channels custom %s", cfg->custom_channels);
        else
            snprintf(cmd, sizeof(cmd), "set_wardrive_channels %s", mode);
        bsp_display_lock(0); wd_setup_set_status("Applying: channels", UI_ACCENT_ORANGE); bsp_display_unlock();
        total++; if (wd_send_set(cmd, "Wardrive channels")) ok++;
    }
    /* rssi delta wifi / ble */
    snprintf(cmd, sizeof(cmd), "set_wardrive_rssi_delta wifi %d", cfg->wifi_rssi_delta);
    bsp_display_lock(0); wd_setup_set_status("Applying: WiFi RSSI delta", UI_ACCENT_ORANGE); bsp_display_unlock();
    total++; if (wd_send_set(cmd, "Wardrive RSSI")) ok++;
    snprintf(cmd, sizeof(cmd), "set_wardrive_rssi_delta ble %d", cfg->ble_rssi_delta);
    bsp_display_lock(0); wd_setup_set_status("Applying: BLE RSSI delta", UI_ACCENT_ORANGE); bsp_display_unlock();
    total++; if (wd_send_set(cmd, "Wardrive RSSI")) ok++;
    /* memcap */
    snprintf(cmd, sizeof(cmd), "set_wardrive_memcap %d", cfg->mem_cap);
    bsp_display_lock(0); wd_setup_set_status("Applying: memory cap", UI_ACCENT_ORANGE); bsp_display_unlock();
    total++; if (wd_send_set(cmd, "Wardrive memory")) ok++;
    /* cooldown */
    snprintf(cmd, sizeof(cmd), "set_wardrive_cooldown %d", cfg->startup_cooldown);
    bsp_display_lock(0); wd_setup_set_status("Applying: startup cooldown", UI_ACCENT_ORANGE); bsp_display_unlock();
    total++; if (wd_send_set(cmd, "Wardrive startup")) ok++;
    /* antisurv */
    {
        const char *s = cfg->antisurv == WD_ANTISURV_LOW ? "low" :
                        cfg->antisurv == WD_ANTISURV_HIGH ? "high" : "med";
        snprintf(cmd, sizeof(cmd), "set_antisurv_sensitivity %s", s);
        bsp_display_lock(0); wd_setup_set_status("Applying: anti-surv", UI_ACCENT_ORANGE); bsp_display_unlock();
        total++; if (wd_send_set(cmd, "Anti-surveillance")) ok++;
    }

    cfg->loaded = true;
    wd_setup_gps_dirty = false;

    char done[64];
    snprintf(done, sizeof(done), "Applied %d/%d settings (saved)", ok, total);
    bsp_display_lock(0);
    wd_setup_set_status(done, ok == total ? UI_ACCENT_GREEN : UI_ACCENT_ORANGE);
    wd_setup_enable_buttons(true);
    bsp_display_unlock();

    wd_setup_applying = false;
    vTaskDelete(NULL);
}

/* ---- Control callbacks ---- */
static void wd_setup_trace_cb(lv_event_t *e)
{
    (void)e;
    if (wd_setup_trace_sw)
        wd_trace_enabled = lv_obj_has_state(wd_setup_trace_sw, LV_STATE_CHECKED);
    update_trace_btn();
}

static void wd_setup_gps_dd_cb(lv_event_t *e)
{
    (void)e;
    if (!wd_setup_gps_dd) return;
    wd_gps_type = (wd_gps_type_t)lv_dropdown_get_selected(wd_setup_gps_dd);
    wd_setup_gps_dirty = true;
    wd_use_external = (wd_gps_type == WD_GPS_EXTERNAL);
    if (wd_use_external && !gps_module_is_running())
        set_status("Enable External GPS in Settings!", UI_ACCENT_ORANGE);
}

static void wd_setup_channel_dd_cb(lv_event_t *e)
{
    (void)e;
    if (!wd_setup_channel_dd || !wd_setup_custom_btn) return;
    if (lv_dropdown_get_selected(wd_setup_channel_dd) == WD_CH_CUSTOM)
        lv_obj_clear_flag(wd_setup_custom_btn, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(wd_setup_custom_btn, LV_OBJ_FLAG_HIDDEN);
}

static void wd_setup_wifi_slider_cb(lv_event_t *e)
{
    (void)e;
    if (!wd_setup_wifi_slider || !wd_setup_wifi_val) return;
    int v = lv_slider_get_value(wd_setup_wifi_slider);
    lv_label_set_text_fmt(wd_setup_wifi_val, "%d dBm%s", v, v == 0 ? " (once)" : "");
}

static void wd_setup_ble_slider_cb(lv_event_t *e)
{
    (void)e;
    if (!wd_setup_ble_slider || !wd_setup_ble_val) return;
    int v = lv_slider_get_value(wd_setup_ble_slider);
    lv_label_set_text_fmt(wd_setup_ble_val, "%d dBm%s", v, v == 0 ? " (once)" : "");
}

static void wd_setup_cd_slider_cb(lv_event_t *e)
{
    (void)e;
    if (!wd_setup_cd_slider || !wd_setup_cd_val) return;
    int v = lv_slider_get_value(wd_setup_cd_slider);
    lv_label_set_text_fmt(wd_setup_cd_val, "%d s", v);
}

static void wd_custom_ch_confirm(const char *text, void *ud)
{
    (void)ud;
    snprintf(wd_config.custom_channels, sizeof(wd_config.custom_channels), "%s", text ? text : "");
    if (wd_setup_custom_lbl && lv_obj_is_valid(wd_setup_custom_lbl))
        lv_label_set_text_fmt(wd_setup_custom_lbl, "Custom: %s",
                              wd_config.custom_channels[0] ? wd_config.custom_channels : "(none)");
}

static void wd_setup_custom_btn_cb(lv_event_t *e)
{
    (void)e;
    ui_show_text_input_popup("Custom channels", wd_config.custom_channels,
                             sizeof(wd_config.custom_channels) - 1, UI_ACCENT_TEAL,
                             wd_custom_ch_confirm, NULL, NULL);
}

static void wd_setup_close(void)
{
    if (wd_setup_applying) return;   /* never tear down while a worker runs */
    wd_gps_debug_close_cb(NULL);     /* stop reader + free the sub-overlay */
    if (wd_setup_overlay) {
        lv_obj_del(wd_setup_overlay);
        wd_setup_overlay = NULL;
    }
    wd_setup_trace_sw = wd_setup_gps_dd = wd_setup_channel_dd = NULL;
    wd_setup_band_cb[0] = wd_setup_band_cb[1] = wd_setup_band_cb[2] = NULL;
    wd_setup_custom_btn = wd_setup_custom_lbl = NULL;
    wd_setup_wifi_slider = wd_setup_wifi_val = NULL;
    wd_setup_ble_slider = wd_setup_ble_val = NULL;
    wd_setup_cd_slider = wd_setup_cd_val = NULL;
    wd_setup_memcap_dd = wd_setup_antisurv_dd = NULL;
    wd_setup_status = NULL;
    wd_setup_load_btn = wd_setup_apply_btn = wd_setup_close_btn = NULL;
    wd_setup_gps_debug_btn = NULL;
}

static void wd_setup_close_cb(lv_event_t *e)
{
    (void)e;
    wd_setup_close();
}

static void wd_setup_load_cb(lv_event_t *e)
{
    (void)e;
    if (wd_setup_applying) return;
    wd_setup_applying = true;
    wd_setup_enable_buttons(false);
    wd_setup_set_status("Reading device config...", UI_ACCENT_ORANGE);
    xTaskCreate(wd_setup_load_task, "wd_load", 8192, NULL, 5, NULL);
}

static void wd_setup_apply_cb(lv_event_t *e)
{
    (void)e;
    if (wd_setup_applying) return;
    wd_config_t *cfg = &wd_config;

    /* gather from controls (LVGL thread) */
    uint8_t bands = 0;
    if (lv_obj_has_state(wd_setup_band_cb[0], LV_STATE_CHECKED)) bands |= WD_BAND_WIFI24;
    if (lv_obj_has_state(wd_setup_band_cb[1], LV_STATE_CHECKED)) bands |= WD_BAND_WIFI5;
    if (lv_obj_has_state(wd_setup_band_cb[2], LV_STATE_CHECKED)) bands |= WD_BAND_BLE;
    if (bands == 0) {
        wd_setup_set_status("Select at least one band", UI_ACCENT_RED);
        return;
    }
    cfg->bands            = bands;
    cfg->channel_mode     = (wd_channel_mode_t)lv_dropdown_get_selected(wd_setup_channel_dd);
    cfg->wifi_rssi_delta  = lv_slider_get_value(wd_setup_wifi_slider);
    cfg->ble_rssi_delta   = lv_slider_get_value(wd_setup_ble_slider);
    cfg->startup_cooldown = lv_slider_get_value(wd_setup_cd_slider);
    cfg->mem_cap          = wd_memcap_values[lv_dropdown_get_selected(wd_setup_memcap_dd)];
    cfg->antisurv         = (wd_antisurv_t)lv_dropdown_get_selected(wd_setup_antisurv_dd);
    if (wd_setup_gps_dd && wd_setup_gps_dirty)
        wd_gps_type = (wd_gps_type_t)lv_dropdown_get_selected(wd_setup_gps_dd);

    wd_setup_applying = true;
    wd_setup_enable_buttons(false);
    wd_setup_set_status("Applying...", UI_ACCENT_ORANGE);
    xTaskCreate(wd_setup_apply_task, "wd_apply", 8192, NULL, 5, NULL);
}

/* ---- small builders ---- */
static lv_obj_t *wd_setup_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

static lv_obj_t *wd_setup_label(lv_obj_t *parent, const char *text, lv_color_t color)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    return lbl;
}

static void show_wardrive_setup(void)
{
    if (wd_running) {          /* config is start-time only */
        set_status("Stop wardrive before setup", UI_ACCENT_ORANGE);
        return;
    }
    if (wd_setup_overlay) return;
    if (!wd_config.loaded) wd_config_set_defaults(&wd_config);

    lv_obj_t *scr = lv_scr_act();

    wd_setup_overlay = lv_obj_create(scr);
    lv_obj_remove_style_all(wd_setup_overlay);
    lv_obj_set_size(wd_setup_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(wd_setup_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(wd_setup_overlay, LV_OPA_70, 0);
    lv_obj_clear_flag(wd_setup_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(wd_setup_overlay, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *popup = lv_obj_create(wd_setup_overlay);
    lv_obj_set_size(popup, 300, 224);
    lv_obj_center(popup);
    lv_obj_set_style_bg_color(popup, ui_card_color(), 0);
    lv_obj_set_style_bg_opa(popup, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(popup, UI_ACCENT_TEAL, 0);
    lv_obj_set_style_border_width(popup, 2, 0);
    lv_obj_set_style_radius(popup, 12, 0);
    lv_obj_set_style_pad_all(popup, 10, 0);
    lv_obj_set_flex_flow(popup, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(popup, 6, 0);
    lv_obj_set_scroll_dir(popup, LV_DIR_VER);

    lv_obj_t *title = lv_label_create(popup);
    lv_label_set_text(title, LV_SYMBOL_SETTINGS " Wardrive Setup");
    lv_obj_set_style_text_color(title, UI_ACCENT_TEAL, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);

    /* Trace (KML track) */
    lv_obj_t *trace_row = wd_setup_row(popup);
    wd_setup_label(trace_row, "Trace (KML track)", UI_ACCENT_CYAN);
    wd_setup_trace_sw = lv_switch_create(trace_row);
    lv_obj_add_event_cb(wd_setup_trace_sw, wd_setup_trace_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* GPS module */
    lv_obj_t *gps_row = wd_setup_row(popup);
    wd_setup_label(gps_row, "GPS module", UI_ACCENT_CYAN);
    wd_setup_gps_dd = lv_dropdown_create(gps_row);
    lv_dropdown_set_options(wd_setup_gps_dd, "m5\natgm\nexternal\ncap");
    lv_dropdown_set_selected(wd_setup_gps_dd, (uint16_t)wd_gps_type);
    lv_obj_set_width(wd_setup_gps_dd, 150);
    lv_obj_add_event_cb(wd_setup_gps_dd, wd_setup_gps_dd_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* GPS raw-NMEA debug / diagnostics popup */
    wd_setup_gps_debug_btn = lv_btn_create(popup);
    lv_obj_set_size(wd_setup_gps_debug_btn, LV_PCT(100), 28);
    lv_obj_set_style_bg_color(wd_setup_gps_debug_btn, ui_card_color(), 0);
    lv_obj_set_style_border_color(wd_setup_gps_debug_btn, UI_ACCENT_TEAL, 0);
    lv_obj_set_style_border_width(wd_setup_gps_debug_btn, 1, 0);
    lv_obj_set_style_radius(wd_setup_gps_debug_btn, 8, 0);
    lv_obj_add_event_cb(wd_setup_gps_debug_btn, wd_gps_debug_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *gps_dbg_lbl = lv_label_create(wd_setup_gps_debug_btn);
    lv_label_set_text(gps_dbg_lbl, LV_SYMBOL_GPS " GPS Debug (raw NMEA)");
    lv_obj_set_style_text_color(gps_dbg_lbl, ui_text_color(), 0);
    lv_obj_set_style_text_font(gps_dbg_lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(gps_dbg_lbl);

    /* Bands */
    wd_setup_label(popup, "Bands", UI_ACCENT_CYAN);
    lv_obj_t *band_row = lv_obj_create(popup);
    lv_obj_set_size(band_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(band_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(band_row, 0, 0);
    lv_obj_set_style_pad_all(band_row, 0, 0);
    lv_obj_set_flex_flow(band_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(band_row, 10, 0);
    lv_obj_clear_flag(band_row, LV_OBJ_FLAG_SCROLLABLE);
    const char *band_lbls[] = { "2.4G", "5G", "BLE" };
    for (int i = 0; i < 3; i++) {
        wd_setup_band_cb[i] = lv_checkbox_create(band_row);
        lv_checkbox_set_text(wd_setup_band_cb[i], band_lbls[i]);
        lv_obj_set_style_text_color(wd_setup_band_cb[i], ui_text_color(), 0);
        lv_obj_set_style_text_font(wd_setup_band_cb[i], &lv_font_montserrat_12, 0);
    }

    /* Channels */
    wd_setup_label(popup, "Channels", UI_ACCENT_CYAN);
    wd_setup_channel_dd = lv_dropdown_create(popup);
    lv_dropdown_set_options(wd_setup_channel_dd, "popular\nall\ncustom");
    lv_obj_set_width(wd_setup_channel_dd, LV_PCT(100));
    lv_obj_add_event_cb(wd_setup_channel_dd, wd_setup_channel_dd_cb, LV_EVENT_VALUE_CHANGED, NULL);
    wd_setup_custom_btn = lv_btn_create(popup);
    lv_obj_set_size(wd_setup_custom_btn, LV_PCT(100), 30);
    lv_obj_set_style_bg_color(wd_setup_custom_btn, ui_card_color(), 0);
    lv_obj_set_style_border_color(wd_setup_custom_btn, UI_ACCENT_TEAL, 0);
    lv_obj_set_style_border_width(wd_setup_custom_btn, 1, 0);
    lv_obj_set_style_radius(wd_setup_custom_btn, 8, 0);
    lv_obj_add_flag(wd_setup_custom_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(wd_setup_custom_btn, wd_setup_custom_btn_cb, LV_EVENT_CLICKED, NULL);
    wd_setup_custom_lbl = lv_label_create(wd_setup_custom_btn);
    lv_label_set_text(wd_setup_custom_lbl, "Custom: (none)");
    lv_obj_set_style_text_color(wd_setup_custom_lbl, ui_text_color(), 0);
    lv_obj_set_style_text_font(wd_setup_custom_lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(wd_setup_custom_lbl);

    /* WiFi RSSI delta */
    lv_obj_t *wifi_hdr = wd_setup_row(popup);
    wd_setup_label(wifi_hdr, "WiFi RSSI delta", UI_ACCENT_CYAN);
    wd_setup_wifi_val = wd_setup_label(wifi_hdr, "5 dBm", UI_ACCENT_TEAL);
    wd_setup_wifi_slider = lv_slider_create(popup);
    lv_slider_set_range(wd_setup_wifi_slider, 0, 50);
    lv_obj_set_width(wd_setup_wifi_slider, LV_PCT(100));
    lv_obj_add_event_cb(wd_setup_wifi_slider, wd_setup_wifi_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* BLE RSSI delta */
    lv_obj_t *ble_hdr = wd_setup_row(popup);
    wd_setup_label(ble_hdr, "BLE RSSI delta", UI_ACCENT_CYAN);
    wd_setup_ble_val = wd_setup_label(ble_hdr, "15 dBm", UI_ACCENT_TEAL);
    wd_setup_ble_slider = lv_slider_create(popup);
    lv_slider_set_range(wd_setup_ble_slider, 0, 50);
    lv_obj_set_width(wd_setup_ble_slider, LV_PCT(100));
    lv_obj_add_event_cb(wd_setup_ble_slider, wd_setup_ble_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Startup cooldown */
    lv_obj_t *cd_hdr = wd_setup_row(popup);
    wd_setup_label(cd_hdr, "Startup cooldown", UI_ACCENT_CYAN);
    wd_setup_cd_val = wd_setup_label(cd_hdr, "0 s", UI_ACCENT_TEAL);
    wd_setup_cd_slider = lv_slider_create(popup);
    lv_slider_set_range(wd_setup_cd_slider, 0, 600);
    lv_obj_set_width(wd_setup_cd_slider, LV_PCT(100));
    lv_obj_add_event_cb(wd_setup_cd_slider, wd_setup_cd_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Memory cap */
    lv_obj_t *mc_row = wd_setup_row(popup);
    wd_setup_label(mc_row, "Memory cap", UI_ACCENT_CYAN);
    wd_setup_memcap_dd = lv_dropdown_create(mc_row);
    lv_dropdown_set_options(wd_setup_memcap_dd, wd_memcap_options);
    lv_obj_set_width(wd_setup_memcap_dd, 130);

    /* Anti-surv sensitivity */
    lv_obj_t *as_row = wd_setup_row(popup);
    wd_setup_label(as_row, "Anti-surv sens.", UI_ACCENT_CYAN);
    wd_setup_antisurv_dd = lv_dropdown_create(as_row);
    lv_dropdown_set_options(wd_setup_antisurv_dd, "low\nmed\nhigh");
    lv_obj_set_width(wd_setup_antisurv_dd, 130);

    /* Status */
    wd_setup_status = lv_label_create(popup);
    lv_label_set_text(wd_setup_status, "Configure once, then Start");
    lv_obj_set_style_text_font(wd_setup_status, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(wd_setup_status, ui_muted_color(), 0);
    lv_obj_set_width(wd_setup_status, LV_PCT(100));
    lv_label_set_long_mode(wd_setup_status, LV_LABEL_LONG_WRAP);

    /* Action buttons */
    lv_obj_t *act_row = lv_obj_create(popup);
    lv_obj_set_size(act_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(act_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(act_row, 0, 0);
    lv_obj_set_style_pad_all(act_row, 0, 0);
    lv_obj_set_flex_flow(act_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(act_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(act_row, LV_OBJ_FLAG_SCROLLABLE);

    wd_setup_load_btn = lv_btn_create(act_row);
    lv_obj_set_size(wd_setup_load_btn, 88, 34);
    lv_obj_set_style_bg_color(wd_setup_load_btn, UI_ACCENT_BLUE, 0);
    lv_obj_set_style_radius(wd_setup_load_btn, 8, 0);
    lv_obj_add_event_cb(wd_setup_load_btn, wd_setup_load_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *load_lbl = lv_label_create(wd_setup_load_btn);
    lv_label_set_text(load_lbl, LV_SYMBOL_REFRESH " Load");
    lv_obj_set_style_text_color(load_lbl, lv_color_white(), 0);
    lv_obj_center(load_lbl);

    wd_setup_apply_btn = lv_btn_create(act_row);
    lv_obj_set_size(wd_setup_apply_btn, 90, 34);
    lv_obj_set_style_bg_color(wd_setup_apply_btn, UI_ACCENT_GREEN, 0);
    lv_obj_set_style_radius(wd_setup_apply_btn, 8, 0);
    lv_obj_add_event_cb(wd_setup_apply_btn, wd_setup_apply_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *apply_lbl = lv_label_create(wd_setup_apply_btn);
    lv_label_set_text(apply_lbl, LV_SYMBOL_OK " Apply");
    lv_obj_set_style_text_color(apply_lbl, lv_color_white(), 0);
    lv_obj_center(apply_lbl);

    wd_setup_close_btn = lv_btn_create(act_row);
    lv_obj_set_size(wd_setup_close_btn, 88, 34);
    style_neutral_button(wd_setup_close_btn);
    lv_obj_add_event_cb(wd_setup_close_btn, wd_setup_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_lbl = lv_label_create(wd_setup_close_btn);
    lv_label_set_text(close_lbl, LV_SYMBOL_CLOSE " Close");
    lv_obj_set_style_text_color(close_lbl, ui_text_color(), 0);
    lv_obj_center(close_lbl);

    /* Show local values, then refresh from device on a worker task. */
    wd_setup_sync_controls();
    wd_setup_load_cb(NULL);
}

/* ================================================================== */
/*  Wardrive — GPS raw NMEA debug popup (ported from Tab5 main.c)       */
/*                                                                      */
/*  Unlike the Tab5 build (dual UART + dedicated raw-reader task) the   */
/*  CoreS3 has a single UART with a line-callback dispatcher, so the    */
/*  raw NMEA stream is consumed via uart_set_line_callback() instead of */
/*  a private task.                                                     */
/* ================================================================== */

static void wd_gps_debug_set_running_controls(bool running)
{
    if (wd_gps_dbg_start_btn) {
        if (running) lv_obj_add_state(wd_gps_dbg_start_btn, LV_STATE_DISABLED);
        else         lv_obj_clear_state(wd_gps_dbg_start_btn, LV_STATE_DISABLED);
    }
    if (wd_gps_dbg_stop_btn) {
        if (running) lv_obj_clear_state(wd_gps_dbg_stop_btn, LV_STATE_DISABLED);
        else         lv_obj_add_state(wd_gps_dbg_stop_btn, LV_STATE_DISABLED);
    }
}

/* Append a line to the PSRAM ring log and (rate-limited) refresh the on-screen
 * box. Called from both the LVGL thread (start/stop echo, force=true) and the
 * uart_rx task (NMEA stream, force=false); bsp_display_lock guards the LVGL
 * access in either case. The buffer is always updated; the expensive label
 * repaint is throttled to WD_GPS_DEBUG_UI_MIN_MS. */
static void wd_gps_debug_append_line(const char *line, bool force)
{
    if (!wd_gps_dbg_log || !line || !line[0]) return;

    size_t used = strnlen(wd_gps_dbg_log, WD_GPS_DEBUG_LOG_SIZE);
    size_t line_len = strnlen(line, 255);
    size_t needed = line_len + 1; /* newline */
    if (needed >= WD_GPS_DEBUG_LOG_SIZE) return;
    if (used + needed >= WD_GPS_DEBUG_LOG_SIZE) {
        size_t discard = used + needed - WD_GPS_DEBUG_LOG_SIZE + 1;
        const char *next_line = memchr(wd_gps_dbg_log + discard, '\n', used - discard);
        if (next_line) discard = (size_t)(next_line - wd_gps_dbg_log) + 1;
        memmove(wd_gps_dbg_log, wd_gps_dbg_log + discard, used - discard + 1);
        used -= discard;
    }
    memcpy(wd_gps_dbg_log + used, line, line_len);
    used += line_len;
    wd_gps_dbg_log[used++] = '\n';
    wd_gps_dbg_log[used] = '\0';

    if (!force && wd_gps_dbg_last_ui &&
        lv_tick_elaps(wd_gps_dbg_last_ui) < WD_GPS_DEBUG_UI_MIN_MS)
        return;                 /* keep the buffer fresh, skip the repaint */
    wd_gps_dbg_last_ui = lv_tick_get();

    bsp_display_lock(0);
    if (wd_gps_dbg_overlay && wd_gps_dbg_log_lbl) {
        lv_label_set_text(wd_gps_dbg_log_lbl, wd_gps_dbg_log);
        if (wd_gps_dbg_log_box)
            lv_obj_scroll_to_y(wd_gps_dbg_log_box, LV_COORD_MAX, LV_ANIM_OFF);
    }
    bsp_display_unlock();
}

static bool wd_gps_debug_coord(const char *value, char hemisphere, double *out)
{
    if (!value || !value[0] || !out) return false;
    double nmea_value = strtod(value, NULL);
    if (nmea_value <= 0.0) return false;
    int degrees = (int)(nmea_value / 100.0);
    double decimal = degrees + (nmea_value - degrees * 100.0) / 60.0;
    if (hemisphere == 'S' || hemisphere == 'W') decimal = -decimal;
    *out = decimal;
    return true;
}

static void wd_gps_debug_parse_diagnostics(const char *line)
{
    if (!line) return;

    bool gps_seen = strchr(line, '$') != NULL || strstr(line, "GPS raw reader started") != NULL;
    bool gps_missing = strstr(line, "GPS not detected") != NULL ||
                       strstr(line, "GPS module not found") != NULL;
    bool antenna_ok = strstr(line, "ANTENNA OK") != NULL;
    bool antenna_missing = strstr(line, "ANTENNA") != NULL &&
                           (strstr(line, "NOT") != NULL || strstr(line, "FAIL") != NULL ||
                            strstr(line, "OPEN") != NULL || strstr(line, "SHORT") != NULL);
    if (!gps_seen && !gps_missing && !antenna_ok && !antenna_missing) return;

    /* Only repaint on an actual state change — GPS presence is re-asserted by
     * every '$' sentence, so an unguarded set_text here fires many times/sec. */
    int presence = gps_seen ? 1 : gps_missing ? 2 : -1;
    int antenna  = antenna_ok ? 1 : antenna_missing ? 2 : -1;

    bsp_display_lock(0);
    if (wd_gps_dbg_overlay) {
        if (wd_gps_dbg_presence_lbl && presence != -1 && presence != wd_gps_dbg_presence_state) {
            wd_gps_dbg_presence_state = presence;
            lv_label_set_text(wd_gps_dbg_presence_lbl,
                              presence == 1 ? "GPS: DETECTED" : "GPS: NOT DETECTED");
            lv_obj_set_style_text_color(wd_gps_dbg_presence_lbl,
                                        presence == 1 ? UI_ACCENT_GREEN : UI_ACCENT_RED, 0);
        }
        if (wd_gps_dbg_antenna_lbl && antenna != -1 && antenna != wd_gps_dbg_antenna_state) {
            wd_gps_dbg_antenna_state = antenna;
            lv_label_set_text(wd_gps_dbg_antenna_lbl,
                              antenna == 1 ? "Ant: OK" : "Ant: NONE");
            lv_obj_set_style_text_color(wd_gps_dbg_antenna_lbl,
                                        antenna == 1 ? UI_ACCENT_GREEN : UI_ACCENT_RED, 0);
        }
    }
    bsp_display_unlock();
}

static void wd_gps_debug_parse_nmea(const char *line)
{
    const char *nmea = line ? strchr(line, '$') : NULL;
    if (!nmea) return;

    char copy[192];
    snprintf(copy, sizeof(copy), "%s", nmea);
    char *checksum = strchr(copy, '*');
    if (checksum) *checksum = '\0';

    char *fields[16] = {0};
    int count = 0;
    char *field = copy;
    while (field && count < (int)(sizeof(fields) / sizeof(fields[0]))) {
        fields[count++] = field;
        char *comma = strchr(field, ',');
        if (!comma) break;
        *comma = '\0';
        field = comma + 1;
    }
    if (count < 9 || !fields[0] || !strstr(fields[0], "GGA")) return;

    int quality = atoi(fields[6]);
    int satellites = atoi(fields[7]);
    const char *hdop = fields[8][0] ? fields[8] : "-";
    double latitude = 0.0, longitude = 0.0;
    bool have_coords = quality > 0 && count > 5 &&
                       wd_gps_debug_coord(fields[2], fields[3][0], &latitude) &&
                       wd_gps_debug_coord(fields[4], fields[5][0], &longitude);

    bsp_display_lock(0);
    if (wd_gps_dbg_overlay) {
        if (wd_gps_dbg_fix_lbl) {
            lv_label_set_text(wd_gps_dbg_fix_lbl, quality > 0 ? "Fix: YES" : "Fix: NO");
            lv_obj_set_style_text_color(wd_gps_dbg_fix_lbl,
                                        quality > 0 ? UI_ACCENT_GREEN : UI_ACCENT_RED, 0);
        }
        if (wd_gps_dbg_sat_lbl)
            lv_label_set_text_fmt(wd_gps_dbg_sat_lbl, "Sat: %d", satellites);
        if (wd_gps_dbg_hdop_lbl)
            lv_label_set_text_fmt(wd_gps_dbg_hdop_lbl, "HDOP: %s", hdop);
        if (wd_gps_dbg_coord_lbl) {
            if (have_coords) {
                lv_label_set_text_fmt(wd_gps_dbg_coord_lbl, "%.6f, %.6f", latitude, longitude);
                lv_obj_set_style_text_color(wd_gps_dbg_coord_lbl, UI_ACCENT_GREEN, 0);
            } else {
                lv_label_set_text(wd_gps_dbg_coord_lbl, "Coords: waiting for fix");
                lv_obj_set_style_text_color(wd_gps_dbg_coord_lbl, ui_muted_color(), 0);
            }
        }
    }
    bsp_display_unlock();
}

/* UART line callback (runs on uart_rx task) while GPS debug is active. */
static void wd_gps_debug_line_cb(const char *line)
{
    if (!wd_gps_dbg_running || !line) return;

    wd_gps_debug_append_line(line, false);
    wd_gps_debug_parse_diagnostics(line);
    wd_gps_debug_parse_nmea(line);

    if (wd_gps_dbg_stop_requested && strstr(line, "All operations stopped")) {
        wd_gps_dbg_running = false;
        wd_gps_dbg_stop_requested = false;
        uart_set_line_callback(NULL);
        bsp_display_lock(0);
        if (wd_gps_dbg_overlay) wd_gps_debug_set_running_controls(false);
        bsp_display_unlock();
    }
}

static void wd_gps_debug_start_cb(lv_event_t *e)
{
    (void)e;
    if (!wd_gps_dbg_overlay || wd_gps_dbg_running || !wd_gps_dbg_log) return;

    wd_gps_dbg_log[0] = '\0';
    if (wd_gps_dbg_log_lbl) lv_label_set_text(wd_gps_dbg_log_lbl, "");
    if (wd_gps_dbg_fix_lbl) {
        lv_label_set_text(wd_gps_dbg_fix_lbl, "Fix: waiting");
        lv_obj_set_style_text_color(wd_gps_dbg_fix_lbl, UI_ACCENT_ORANGE, 0);
    }
    if (wd_gps_dbg_sat_lbl)  lv_label_set_text(wd_gps_dbg_sat_lbl, "Sat: -");
    if (wd_gps_dbg_hdop_lbl) lv_label_set_text(wd_gps_dbg_hdop_lbl, "HDOP: -");
    if (wd_gps_dbg_presence_lbl) {
        lv_label_set_text(wd_gps_dbg_presence_lbl, "GPS: checking");
        lv_obj_set_style_text_color(wd_gps_dbg_presence_lbl, UI_ACCENT_ORANGE, 0);
    }
    if (wd_gps_dbg_antenna_lbl) {
        lv_label_set_text(wd_gps_dbg_antenna_lbl, "Ant: checking");
        lv_obj_set_style_text_color(wd_gps_dbg_antenna_lbl, UI_ACCENT_ORANGE, 0);
    }
    if (wd_gps_dbg_coord_lbl) {
        lv_label_set_text(wd_gps_dbg_coord_lbl, "Coords: waiting for fix");
        lv_obj_set_style_text_color(wd_gps_dbg_coord_lbl, ui_muted_color(), 0);
    }

    wd_gps_dbg_stop_requested = false;
    wd_gps_dbg_running = true;
    wd_gps_dbg_last_ui = 0;
    wd_gps_dbg_presence_state = -1;
    wd_gps_dbg_antenna_state = -1;
    wd_gps_debug_set_running_controls(true);

    uart_handler_flush_rx();
    uart_set_line_callback(wd_gps_debug_line_cb);
    wd_gps_debug_append_line("> start_gps_raw", true);
    uart_send_command("start_gps_raw");
}

static void wd_gps_debug_stop_cb(lv_event_t *e)
{
    (void)e;
    if (!wd_gps_dbg_running || wd_gps_dbg_stop_requested) return;

    wd_gps_dbg_stop_requested = true;
    if (wd_gps_dbg_stop_btn) lv_obj_add_state(wd_gps_dbg_stop_btn, LV_STATE_DISABLED);
    wd_gps_debug_append_line("> stop", true);
    uart_send_command("stop");
}

static void wd_gps_debug_close_cb(lv_event_t *e)
{
    (void)e;

    if (wd_gps_dbg_running) {
        wd_gps_dbg_stop_requested = true;
        wd_gps_dbg_running = false;
        uart_send_command("stop");
    }
    uart_set_line_callback(NULL);

    bsp_display_lock(0);
    if (wd_gps_dbg_overlay) lv_obj_del(wd_gps_dbg_overlay);
    wd_gps_dbg_overlay = NULL;
    wd_gps_dbg_fix_lbl = NULL;
    wd_gps_dbg_sat_lbl = NULL;
    wd_gps_dbg_hdop_lbl = NULL;
    wd_gps_dbg_presence_lbl = NULL;
    wd_gps_dbg_antenna_lbl = NULL;
    wd_gps_dbg_coord_lbl = NULL;
    wd_gps_dbg_log_box = NULL;
    wd_gps_dbg_log_lbl = NULL;
    wd_gps_dbg_start_btn = NULL;
    wd_gps_dbg_stop_btn = NULL;
    wd_gps_dbg_close_btn = NULL;
    bsp_display_unlock();
}

/* Small transparent flex-row helper for the stat / diagnostic / action rows. */
static lv_obj_t *wd_gps_debug_row(lv_obj_t *parent, lv_coord_t h)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), h ? h : LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

static void wd_gps_debug_btn_cb(lv_event_t *e)
{
    (void)e;
    if (!wd_setup_overlay || wd_gps_dbg_overlay) return;

    /* Keep the sizeable raw-UART history out of internal RAM. */
    if (!wd_gps_dbg_log) {
        wd_gps_dbg_log = heap_caps_calloc(1, WD_GPS_DEBUG_LOG_SIZE,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!wd_gps_dbg_log) {
            ESP_LOGE(TAG, "GPS Debug: failed to allocate %d-byte PSRAM log",
                     WD_GPS_DEBUG_LOG_SIZE);
            return;
        }
    }

    wd_gps_dbg_overlay = lv_obj_create(wd_setup_overlay);
    lv_obj_remove_style_all(wd_gps_dbg_overlay);
    lv_obj_set_size(wd_gps_dbg_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(wd_gps_dbg_overlay, 0, 0);
    lv_obj_set_style_bg_color(wd_gps_dbg_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(wd_gps_dbg_overlay, LV_OPA_70, 0);
    lv_obj_clear_flag(wd_gps_dbg_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(wd_gps_dbg_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(wd_gps_dbg_overlay);

    lv_obj_t *popup = lv_obj_create(wd_gps_dbg_overlay);
    lv_obj_set_size(popup, 314, 234);
    lv_obj_center(popup);
    lv_obj_set_style_bg_color(popup, ui_card_color(), 0);
    lv_obj_set_style_bg_opa(popup, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(popup, UI_ACCENT_TEAL, 0);
    lv_obj_set_style_border_width(popup, 2, 0);
    lv_obj_set_style_radius(popup, 12, 0);
    lv_obj_set_style_pad_all(popup, 6, 0);
    lv_obj_set_flex_flow(popup, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(popup, 3, 0);
    lv_obj_clear_flag(popup, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(popup);
    lv_label_set_text(title, LV_SYMBOL_GPS " GPS Debug");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, UI_ACCENT_TEAL, 0);

    /* Fix / satellites / HDOP */
    lv_obj_t *stats = wd_gps_debug_row(popup, 0);
    wd_gps_dbg_fix_lbl  = lv_label_create(stats);
    wd_gps_dbg_sat_lbl  = lv_label_create(stats);
    wd_gps_dbg_hdop_lbl = lv_label_create(stats);
    lv_label_set_text(wd_gps_dbg_fix_lbl, "Fix: waiting");
    lv_label_set_text(wd_gps_dbg_sat_lbl, "Sat: -");
    lv_label_set_text(wd_gps_dbg_hdop_lbl, "HDOP: -");
    lv_obj_set_style_text_font(wd_gps_dbg_fix_lbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_font(wd_gps_dbg_sat_lbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_font(wd_gps_dbg_hdop_lbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(wd_gps_dbg_fix_lbl, UI_ACCENT_ORANGE, 0);
    lv_obj_set_style_text_color(wd_gps_dbg_sat_lbl, UI_ACCENT_CYAN, 0);
    lv_obj_set_style_text_color(wd_gps_dbg_hdop_lbl, UI_ACCENT_CYAN, 0);

    /* GPS presence / antenna diagnostics */
    lv_obj_t *diag = wd_gps_debug_row(popup, 0);
    wd_gps_dbg_presence_lbl = lv_label_create(diag);
    wd_gps_dbg_antenna_lbl  = lv_label_create(diag);
    lv_label_set_text(wd_gps_dbg_presence_lbl, "GPS: checking");
    lv_label_set_text(wd_gps_dbg_antenna_lbl, "Ant: checking");
    lv_obj_set_style_text_font(wd_gps_dbg_presence_lbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_font(wd_gps_dbg_antenna_lbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(wd_gps_dbg_presence_lbl, UI_ACCENT_ORANGE, 0);
    lv_obj_set_style_text_color(wd_gps_dbg_antenna_lbl, UI_ACCENT_ORANGE, 0);

    wd_gps_dbg_coord_lbl = lv_label_create(popup);
    lv_label_set_text(wd_gps_dbg_coord_lbl, "Coords: waiting for fix");
    lv_obj_set_style_text_font(wd_gps_dbg_coord_lbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(wd_gps_dbg_coord_lbl, ui_muted_color(), 0);

    /* Raw UART output log box (fills the remaining vertical space) */
    wd_gps_dbg_log_box = lv_obj_create(popup);
    lv_obj_set_width(wd_gps_dbg_log_box, LV_PCT(100));
    lv_obj_set_flex_grow(wd_gps_dbg_log_box, 1);
    lv_obj_set_style_bg_color(wd_gps_dbg_log_box, lv_color_hex(0x0B1018), 0);
    lv_obj_set_style_border_color(wd_gps_dbg_log_box, lv_color_hex(0x37474F), 0);
    lv_obj_set_style_border_width(wd_gps_dbg_log_box, 1, 0);
    lv_obj_set_style_radius(wd_gps_dbg_log_box, 6, 0);
    lv_obj_set_style_pad_all(wd_gps_dbg_log_box, 6, 0);
    lv_obj_set_flex_flow(wd_gps_dbg_log_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(wd_gps_dbg_log_box, 0, 0);
    lv_obj_set_scroll_dir(wd_gps_dbg_log_box, LV_DIR_VER);
    wd_gps_dbg_log_lbl = lv_label_create(wd_gps_dbg_log_box);
    lv_label_set_text(wd_gps_dbg_log_lbl, "Tap Start to read NMEA sentences.");
    lv_label_set_long_mode(wd_gps_dbg_log_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(wd_gps_dbg_log_lbl, LV_PCT(100));
    lv_obj_set_style_text_font(wd_gps_dbg_log_lbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(wd_gps_dbg_log_lbl, lv_color_hex(0xCFD8DC), 0);

    /* Start / Stop / Close */
    lv_obj_t *actions = wd_gps_debug_row(popup, 32);

    wd_gps_dbg_start_btn = lv_btn_create(actions);
    lv_obj_set_size(wd_gps_dbg_start_btn, 96, 30);
    lv_obj_set_style_bg_color(wd_gps_dbg_start_btn, UI_ACCENT_GREEN, 0);
    lv_obj_set_style_radius(wd_gps_dbg_start_btn, 8, 0);
    lv_obj_add_event_cb(wd_gps_dbg_start_btn, wd_gps_debug_start_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *start_lbl = lv_label_create(wd_gps_dbg_start_btn);
    lv_label_set_text(start_lbl, LV_SYMBOL_PLAY " Start");
    lv_obj_set_style_text_color(start_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(start_lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(start_lbl);

    wd_gps_dbg_stop_btn = lv_btn_create(actions);
    lv_obj_set_size(wd_gps_dbg_stop_btn, 96, 30);
    lv_obj_set_style_bg_color(wd_gps_dbg_stop_btn, UI_ACCENT_RED, 0);
    lv_obj_set_style_bg_color(wd_gps_dbg_stop_btn, ui_card_color(), LV_STATE_DISABLED);
    lv_obj_set_style_radius(wd_gps_dbg_stop_btn, 8, 0);
    lv_obj_add_event_cb(wd_gps_dbg_stop_btn, wd_gps_debug_stop_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_state(wd_gps_dbg_stop_btn, LV_STATE_DISABLED);
    lv_obj_t *stop_lbl = lv_label_create(wd_gps_dbg_stop_btn);
    lv_label_set_text(stop_lbl, LV_SYMBOL_STOP " Stop");
    lv_obj_set_style_text_color(stop_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(stop_lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(stop_lbl);

    wd_gps_dbg_close_btn = lv_btn_create(actions);
    lv_obj_set_size(wd_gps_dbg_close_btn, 96, 30);
    style_neutral_button(wd_gps_dbg_close_btn);
    lv_obj_add_event_cb(wd_gps_dbg_close_btn, wd_gps_debug_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_lbl = lv_label_create(wd_gps_dbg_close_btn);
    lv_label_set_text(close_lbl, LV_SYMBOL_CLOSE " Close");
    lv_obj_set_style_text_color(close_lbl, ui_text_color(), 0);
    lv_obj_set_style_text_font(close_lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(close_lbl);
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
    wd_setup_overlay = NULL;
    wd_setup_applying = false;

    lv_obj_t *scr = ui_screen_clear();
    lv_obj_t *bar = ui_create_top_bar(scr, "Wardrive", on_back, NULL);
    ui_add_top_bar_action(bar, LV_SYMBOL_UPLOAD, wardrive_upload_btn_cb, NULL);
    ui_add_top_bar_action(bar, LV_SYMBOL_GPS, on_gps_btn, NULL);

    /* Header: status + live stats on the LEFT, compact action controls on the
     * RIGHT (reclaims the old full-width button row so the list starts higher). */
    lv_obj_t *info = lv_obj_create(scr);
    lv_obj_set_size(info, 190, 40);
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
    lv_label_set_long_mode(wd_status_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(wd_status_lbl, LV_PCT(100));

    wd_stats_lbl = lv_label_create(info);
    lv_label_set_text(wd_stats_lbl, "WiFi:0 BT:0 SAT:0 0.00km");
    lv_obj_set_style_text_color(wd_stats_lbl, ui_muted_color(), 0);
    lv_obj_set_style_text_font(wd_stats_lbl, &lv_font_montserrat_10, 0);

    /* action column (right): Start/Stop (icon) + Trace, hugging the edge */
    lv_obj_t *btn_row = lv_obj_create(scr);
    lv_obj_set_size(btn_row, 130, 34);
    lv_obj_set_pos(btn_row, 190, 39);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_set_style_pad_right(btn_row, 8, 0);
    lv_obj_set_style_pad_column(btn_row, 6, 0);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    start_btn = lv_btn_create(btn_row);
    lv_obj_set_size(start_btn, 44, 30);
    lv_obj_set_style_bg_color(start_btn, UI_ACCENT_GREEN, 0);
    lv_obj_set_style_radius(start_btn, 8, 0);
    lv_obj_add_event_cb(start_btn, on_start, LV_EVENT_CLICKED, NULL);
    lv_obj_t *start_lbl = lv_label_create(start_btn);
    lv_label_set_text(start_lbl, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_color(start_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(start_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(start_lbl);

    stop_btn = lv_btn_create(btn_row);
    lv_obj_set_size(stop_btn, 44, 30);
    lv_obj_set_style_bg_color(stop_btn, UI_ACCENT_RED, 0);
    lv_obj_set_style_radius(stop_btn, 8, 0);
    lv_obj_add_event_cb(stop_btn, on_stop, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(stop_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *stop_lbl = lv_label_create(stop_btn);
    lv_label_set_text(stop_lbl, LV_SYMBOL_STOP);
    lv_obj_set_style_text_color(stop_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(stop_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(stop_lbl);

    trace_btn = lv_btn_create(btn_row);
    lv_obj_set_size(trace_btn, 72, 30);
    lv_obj_set_style_radius(trace_btn, 8, 0);
    lv_obj_set_style_pad_hor(trace_btn, 3, 0);
    lv_obj_add_event_cb(trace_btn, on_trace, LV_EVENT_CLICKED, NULL);
    trace_lbl = lv_label_create(trace_btn);
    lv_obj_set_style_text_color(trace_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(trace_lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(trace_lbl);
    update_trace_btn();

    /* network card list */
    wd_list = lv_obj_create(scr);
    lv_obj_set_size(wd_list, LV_PCT(100), 240 - 80);
    lv_obj_set_pos(wd_list, 0, 80);
    lv_obj_set_style_bg_color(wd_list, ui_card_color(), 0);
    lv_obj_set_style_bg_opa(wd_list, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(wd_list, 0, 0);
    lv_obj_set_style_border_width(wd_list, 0, 0);
    lv_obj_set_style_pad_all(wd_list, 4, 0);
    lv_obj_set_style_pad_row(wd_list, 3, 0);
    lv_obj_set_flex_flow(wd_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(wd_list, LV_DIR_VER);

    update_list();
}
