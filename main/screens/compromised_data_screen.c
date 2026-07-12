#include "compromised_data_screen.h"
#include "home_screen.h"
#include "ui_helpers.h"
#include "uart_handler.h"
#include "wifi_connect_helper.h"
#include "psram_dynarr.h"
#include "bsp/m5stack_core_s3.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static const char *TAG = "compromised";

/* ================================================================== */
/*  Shared data structures                                             */
/* ================================================================== */

#define COMPRO_HARD_CAP 1024

/* Evil Twin passwords: "SSID", "password" */
typedef struct {
    char ssid[33];
    char password[64];
} evil_pass_entry_t;

static evil_pass_entry_t *evil_entries = NULL;
static int evil_cap = 0;
static int evil_count = 0;

/* Portal data: "SSID", "field1=val1", ... */
typedef struct {
    char ssid[33];
    char fields[192];   /* all fields concatenated, separated by \n */
} portal_entry_t;

static portal_entry_t *portal_entries = NULL;
static int portal_cap = 0;
static int portal_count = 0;

/* Handshake files */
typedef struct {
    int  number;
    char filename[128];
} handshake_entry_t;

static handshake_entry_t *hs_entries = NULL;
static int hs_cap = 0;
static int hs_count = 0;

/* Timers for timeout-based collection (show_pass has no end marker) */
static esp_timer_handle_t evil_timer = NULL;
static esp_timer_handle_t portal_timer = NULL;
static bool timer_collecting = false;

/* ================================================================== */
/*  Forward declarations                                               */
/* ================================================================== */

static void show_evil_pass_list(void);
static void show_portal_data_list(void);
static void show_handshake_list(void);
static void on_wpasec_upload_btn(lv_event_t *e);

/* ================================================================== */
/*  Navigation: back to home / back to compromised menu                */
/* ================================================================== */

static void on_back_home(lv_event_t *e)
{
    (void)e;
    bsp_display_lock(0);
    show_home_screen();
    bsp_display_unlock();
}

static void on_back_menu(lv_event_t *e)
{
    (void)e;
    uart_set_line_callback(NULL);
    if (evil_timer) esp_timer_stop(evil_timer);
    if (portal_timer) esp_timer_stop(portal_timer);
    timer_collecting = false;
    bsp_display_lock(0);
    show_compromised_data_screen();
    bsp_display_unlock();
}

/* ================================================================== */
/*  Helper: create a loading screen with spinner                       */
/* ================================================================== */

static void show_loading(const char *title, const char *msg)
{
    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, title, on_back_menu, NULL);

    lv_obj_t *center = lv_obj_create(scr);
    lv_obj_set_size(center, LV_PCT(100), 240 - 36);
    lv_obj_set_pos(center, 0, 36);
    lv_obj_set_style_bg_opa(center, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(center, 0, 0);
    lv_obj_set_flex_flow(center, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(center, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(center, 10, 0);
    lv_obj_clear_flag(center, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *spinner = lv_spinner_create(center);
    lv_obj_set_size(spinner, 40, 40);
    lv_spinner_set_anim_params(spinner, 1000, 200);

    lv_obj_t *lbl = lv_label_create(center);
    lv_label_set_text(lbl, msg);
    lv_obj_set_style_text_color(lbl, ui_muted_color(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
}

/* ================================================================== */
/*  Evil Twin Passwords: show_pass evil                                */
/* ================================================================== */

/* ---- wifi_connect overlay ---- */

static lv_obj_t *connect_overlay = NULL;
static lv_obj_t *connect_status_lbl = NULL;

static void close_connect_overlay(lv_event_t *e)
{
    (void)e;
    uart_set_line_callback(NULL);
    if (connect_overlay) {
        lv_obj_del(connect_overlay);
        connect_overlay = NULL;
        connect_status_lbl = NULL;
    }
}

static void connect_line_cb(const char *line)
{
    if (strstr(line, "SUCCESS")) {
        bsp_display_lock(0);
        if (connect_status_lbl) {
            lv_label_set_text(connect_status_lbl, "Connected!");
            lv_obj_set_style_text_color(connect_status_lbl, UI_ACCENT_GREEN, 0);
        }
        bsp_display_unlock();
        uart_set_line_callback(NULL);
    } else if (strstr(line, "FAILED") || strstr(line, "Error")) {
        bsp_display_lock(0);
        if (connect_status_lbl) {
            lv_label_set_text(connect_status_lbl, "Connection failed!");
            lv_obj_set_style_text_color(connect_status_lbl, UI_ACCENT_RED, 0);
        }
        bsp_display_unlock();
        uart_set_line_callback(NULL);
    }
}

static void on_evil_item_clicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= evil_count) return;

    evil_pass_entry_t *entry = &evil_entries[idx];
    ESP_LOGI(TAG, "Connecting: SSID='%s' pass='%s'", entry->ssid, entry->password);

    /* send wifi_connect command */
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "wifi_connect %s %s", entry->ssid, entry->password);
    uart_set_line_callback(connect_line_cb);
    uart_send_command(cmd);

    /* show overlay */
    lv_obj_t *scr = lv_scr_act();
    connect_overlay = lv_obj_create(scr);
    lv_obj_set_size(connect_overlay, 280, 140);
    lv_obj_center(connect_overlay);
    lv_obj_set_style_bg_color(connect_overlay, ui_card_color(), 0);
    lv_obj_set_style_bg_opa(connect_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(connect_overlay, 12, 0);
    lv_obj_set_style_border_color(connect_overlay, UI_ACCENT_BLUE, 0);
    lv_obj_set_style_border_width(connect_overlay, 2, 0);
    lv_obj_set_style_pad_all(connect_overlay, 14, 0);
    lv_obj_set_flex_flow(connect_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(connect_overlay, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(connect_overlay, 8, 0);
    lv_obj_clear_flag(connect_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(connect_overlay);
    char title_txt[64];
    snprintf(title_txt, sizeof(title_txt), "Connecting to %s...", entry->ssid);
    lv_label_set_text(title, title_txt);
    lv_obj_set_style_text_color(title, ui_text_color(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    connect_status_lbl = lv_label_create(connect_overlay);
    lv_label_set_text(connect_status_lbl, "Waiting...");
    lv_obj_set_style_text_color(connect_status_lbl, UI_ACCENT_CYAN, 0);
    lv_obj_set_style_text_font(connect_status_lbl, &lv_font_montserrat_14, 0);

    lv_obj_t *btn = lv_btn_create(connect_overlay);
    lv_obj_set_size(btn, 100, 32);
    lv_obj_set_style_bg_color(btn, ui_muted_color(), 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_add_event_cb(btn, close_connect_overlay, LV_EVENT_CLICKED, NULL);
    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, "Close");
    lv_obj_set_style_text_color(btn_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(btn_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(btn_lbl);
}

/* ---- Build evil twin password list UI ---- */

static void show_evil_pass_list(void)
{
    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Evil Twin Passwords", on_back_menu, NULL);

    if (evil_count == 0) {
        lv_obj_t *lbl = lv_label_create(scr);
        lv_label_set_text(lbl, "No passwords captured.");
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
    lv_obj_set_style_pad_all(list, 4, 0);
    lv_obj_set_style_pad_row(list, 3, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    for (int i = 0; i < evil_count; i++) {
        lv_obj_t *btn = lv_btn_create(list);
        lv_obj_set_size(btn, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(btn, ui_card_color(), 0);
        lv_obj_set_style_bg_color(btn, UI_ACCENT_GREEN, LV_STATE_PRESSED);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_pad_all(btn, 6, 0);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(btn, 1, 0);
        lv_obj_add_event_cb(btn, on_evil_item_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);

        lv_obj_t *ssid_lbl = lv_label_create(btn);
        lv_label_set_text(ssid_lbl, evil_entries[i].ssid);
        lv_obj_set_style_text_color(ssid_lbl, UI_ACCENT_GREEN, 0);
        lv_obj_set_style_text_font(ssid_lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_width(ssid_lbl, LV_PCT(100));
        lv_label_set_long_mode(ssid_lbl, LV_LABEL_LONG_DOT);

        lv_obj_t *pass_lbl = lv_label_create(btn);
        lv_label_set_text(pass_lbl, evil_entries[i].password);
        lv_obj_set_style_text_color(pass_lbl, ui_text_color(), 0);
        lv_obj_set_style_text_font(pass_lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_width(pass_lbl, LV_PCT(100));
        lv_label_set_long_mode(pass_lbl, LV_LABEL_LONG_DOT);
    }
}

/* ---- Collect evil twin passwords via UART ---- */

static void evil_timeout_cb(void *arg)
{
    (void)arg;
    if (!timer_collecting) return;
    timer_collecting = false;
    uart_set_line_callback(NULL);
    ESP_LOGI(TAG, "Evil pass collection done, %d entries", evil_count);

    bsp_display_lock(0);
    show_evil_pass_list();
    bsp_display_unlock();
}

static void evil_line_cb(const char *line)
{
    if (!timer_collecting) return;

    /* Parse: "SSID", "password" */
    if (line[0] != '"') return;

    const char *ssid_start = line + 1;
    const char *ssid_end = strchr(ssid_start, '"');
    if (!ssid_end) return;

    /* expect ", " between SSID and password */
    const char *sep = ssid_end + 1;
    while (*sep == ',' || *sep == ' ') sep++;
    if (*sep != '"') return;

    const char *pass_start = sep + 1;
    const char *pass_end = strchr(pass_start, '"');
    if (!pass_end) return;

    if (!psram_dynarr_ensure((void **)&evil_entries, &evil_cap,
                             evil_count + 1, sizeof(*evil_entries),
                             COMPRO_HARD_CAP)) return;

    int ssid_len = ssid_end - ssid_start;
    int pass_len = pass_end - pass_start;
    if (ssid_len > 32) ssid_len = 32;
    if (pass_len > 63) pass_len = 63;

    memcpy(evil_entries[evil_count].ssid, ssid_start, ssid_len);
    evil_entries[evil_count].ssid[ssid_len] = '\0';
    memcpy(evil_entries[evil_count].password, pass_start, pass_len);
    evil_entries[evil_count].password[pass_len] = '\0';
    evil_count++;

    /* restart timeout — more lines may follow */
    esp_timer_stop(evil_timer);
    esp_timer_start_once(evil_timer, 500000); /* 500 ms */
}

static void on_evil_passwords(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Evil Twin Passwords selected");

    evil_count = 0;
    timer_collecting = true;

    if (!evil_timer) {
        esp_timer_create_args_t args = {
            .callback = evil_timeout_cb,
            .name = "evil_timer",
        };
        esp_timer_create(&args, &evil_timer);
    }

    show_loading("Evil Twin Passwords", "Loading passwords...");

    uart_set_line_callback(evil_line_cb);
    esp_timer_start_once(evil_timer, 2000000); /* 2 s initial timeout */
    uart_send_command("show_pass evil");
}

/* ================================================================== */
/*  Portal Data: show_pass portal                                      */
/* ================================================================== */

static void show_portal_data_list(void)
{
    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Portal Data", on_back_menu, NULL);

    if (portal_count == 0) {
        lv_obj_t *lbl = lv_label_create(scr);
        lv_label_set_text(lbl, "No portal data captured.");
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
    lv_obj_set_style_pad_all(list, 4, 0);
    lv_obj_set_style_pad_row(list, 3, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    for (int i = 0; i < portal_count; i++) {
        lv_obj_t *card = lv_obj_create(list);
        lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(card, ui_card_color(), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 6, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_pad_all(card, 6, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(card, 1, 0);

        lv_obj_t *ssid_lbl = lv_label_create(card);
        lv_label_set_text(ssid_lbl, portal_entries[i].ssid);
        lv_obj_set_style_text_color(ssid_lbl, UI_ACCENT_PURPLE, 0);
        lv_obj_set_style_text_font(ssid_lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_width(ssid_lbl, LV_PCT(100));
        lv_label_set_long_mode(ssid_lbl, LV_LABEL_LONG_DOT);

        lv_obj_t *fields_lbl = lv_label_create(card);
        lv_label_set_text(fields_lbl, portal_entries[i].fields);
        lv_obj_set_style_text_color(fields_lbl, ui_text_color(), 0);
        lv_obj_set_style_text_font(fields_lbl, &lv_font_montserrat_10, 0);
        lv_obj_set_width(fields_lbl, LV_PCT(100));
        lv_label_set_long_mode(fields_lbl, LV_LABEL_LONG_WRAP);
    }
}

/* ---- Collect portal data via UART ---- */

static void portal_timeout_cb(void *arg)
{
    (void)arg;
    if (!timer_collecting) return;
    timer_collecting = false;
    uart_set_line_callback(NULL);
    ESP_LOGI(TAG, "Portal data collection done, %d entries", portal_count);

    bsp_display_lock(0);
    show_portal_data_list();
    bsp_display_unlock();
}

static void portal_line_cb(const char *line)
{
    if (!timer_collecting) return;
    if (line[0] != '"') return;

    if (!psram_dynarr_ensure((void **)&portal_entries, &portal_cap,
                             portal_count + 1, sizeof(*portal_entries),
                             COMPRO_HARD_CAP)) return;

    /* Parse: "SSID", "field1=val1", "field2=val2", ... */
    const char *ssid_start = line + 1;
    const char *ssid_end = strchr(ssid_start, '"');
    if (!ssid_end) return;

    int ssid_len = ssid_end - ssid_start;
    if (ssid_len > 32) ssid_len = 32;
    memcpy(portal_entries[portal_count].ssid, ssid_start, ssid_len);
    portal_entries[portal_count].ssid[ssid_len] = '\0';

    /* collect remaining fields into a single string separated by \n */
    portal_entries[portal_count].fields[0] = '\0';
    int fields_pos = 0;
    const char *p = ssid_end + 1;

    while (*p) {
        /* skip to next quoted field */
        while (*p && *p != '"') p++;
        if (*p != '"') break;
        p++; /* skip opening quote */

        const char *field_start = p;
        const char *field_end = strchr(field_start, '"');
        if (!field_end) break;

        int field_len = field_end - field_start;
        if (fields_pos + field_len + 2 >= (int)sizeof(portal_entries[0].fields))
            break;

        if (fields_pos > 0) {
            portal_entries[portal_count].fields[fields_pos++] = '\n';
        }
        memcpy(&portal_entries[portal_count].fields[fields_pos], field_start, field_len);
        fields_pos += field_len;
        portal_entries[portal_count].fields[fields_pos] = '\0';

        p = field_end + 1;
    }

    portal_count++;

    esp_timer_stop(portal_timer);
    esp_timer_start_once(portal_timer, 500000);
}

static void on_portal_data(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Portal Data selected");

    portal_count = 0;
    timer_collecting = true;

    if (!portal_timer) {
        esp_timer_create_args_t args = {
            .callback = portal_timeout_cb,
            .name = "portal_timer",
        };
        esp_timer_create(&args, &portal_timer);
    }

    show_loading("Portal Data", "Loading portal data...");

    uart_set_line_callback(portal_line_cb);
    esp_timer_start_once(portal_timer, 2000000);
    uart_send_command("show_pass portal");
}

/* ================================================================== */
/*  Handshakes: list_dir /sdcard/lab/handshakes                        */
/* ================================================================== */

static void show_handshake_list(void)
{
    lv_obj_t *scr = ui_screen_clear();
    lv_obj_t *bar = ui_create_top_bar(scr, "Handshakes", on_back_menu, NULL);
    /* Upload all captured .pcap handshakes to wpa-sec.stanev.org. Shown even
     * with an empty list, since the C5 uploads every handshake on its SD card. */
    ui_add_top_bar_action(bar, LV_SYMBOL_UPLOAD, on_wpasec_upload_btn, NULL);

    if (hs_count == 0) {
        lv_obj_t *lbl = lv_label_create(scr);
        lv_label_set_text(lbl, "No handshake files found.");
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
    lv_obj_set_style_pad_all(list, 4, 0);
    lv_obj_set_style_pad_row(list, 3, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    for (int i = 0; i < hs_count; i++) {
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(row, ui_card_color(), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 6, 0);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, hs_entries[i].filename);
        lv_obj_set_style_text_color(lbl, UI_ACCENT_ORANGE, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
        lv_obj_set_width(lbl, LV_PCT(100));
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    }
}

/* ---- Collect handshake file list via UART (has end marker) ---- */

static void hs_collect_cb(const char **lines, int line_count)
{
    hs_count = 0;
    bool header_found = false;

    for (int i = 0; i < line_count; i++) {
        const char *line = lines[i];

        if (strstr(line, "Files in") != NULL) {
            header_found = true;
            continue;
        }
        if (strstr(line, "Found") != NULL && strstr(line, "file(s)") != NULL) {
            break;  /* end of listing */
        }
        if (!header_found) continue;

        /* Parse: "N filename" */
        int num;
        char filename[128];
        if (sscanf(line, "%d %127[^\n]", &num, filename) == 2) {
            if (!psram_dynarr_ensure((void **)&hs_entries, &hs_cap,
                                     hs_count + 1, sizeof(*hs_entries),
                                     COMPRO_HARD_CAP)) {
                ESP_LOGW(TAG, "hs cap reached at %d", hs_count);
                break;
            }
            hs_entries[hs_count].number = num;
            snprintf(hs_entries[hs_count].filename,
                     sizeof(hs_entries[0].filename), "%s", filename);
            hs_count++;
        }
    }

    ESP_LOGI(TAG, "Handshake list: %d files", hs_count);
    bsp_display_lock(0);
    show_handshake_list();
    bsp_display_unlock();
}

static void on_handshakes(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Handshakes selected");

    hs_count = 0;

    show_loading("Handshakes", "Loading handshake files...");

    uart_start_collect("Found", hs_collect_cb);
    uart_send_command("list_dir /sdcard/lab/handshakes");
}

/* ================================================================== */
/*  WPA-SEC upload flow                                                */
/*                                                                     */
/*  Mirrors the Wardrive upload flow (wardrive_screen.c): check the    */
/*  API key on the C5, scan/pick a WiFi network, connect (with         */
/*  evil-twin password fallback), then send `wpasec_upload` and stream */
/*  the firmware's progress lines into a scrolling log.                */
/* ================================================================== */

static wifi_network_t *ws_aps = NULL;   /* scan results */
static int ws_aps_cap = 0;
static int ws_aps_cnt = 0;
static wifi_network_t ws_net;           /* selected AP */
static char ws_pass[64];
static lv_obj_t *ws_status = NULL;      /* status label on the active screen */
static lv_obj_t *ws_log = NULL;         /* scrolling upload log container */

static void ws_show_ap_picker(void);
static void ws_start_scan(void);
static void ws_start_upload(void);

/* ---- key missing message ---- */

static void ws_show_key_missing(void)
{
    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "WPA-SEC Upload", on_back_menu, NULL);

    lv_obj_t *box = lv_obj_create(scr);
    lv_obj_set_size(box, LV_PCT(100), 240 - 36);
    lv_obj_set_pos(box, 0, 36);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 12, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 8, 0);

    lv_obj_t *title = lv_label_create(box);
    lv_label_set_text(title, "No WPA-SEC key set");
    lv_obj_set_style_text_color(title, UI_ACCENT_RED, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);

    lv_obj_t *msg = lv_label_create(box);
    lv_label_set_text(msg,
        "Set your key on the C5 module:\n"
        "  wpasec_key set <key>\n"
        "or create /sdcard/lab/wpa-sec.txt\n\n"
        "Get a key at:\n"
        "wpa-sec.stanev.org/?get_key");
    lv_obj_set_style_text_color(msg, ui_muted_color(), 0);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_12, 0);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg, LV_PCT(100));
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
}

/* ---- key check (off the LVGL thread) ---- */

static void ws_key_result_cb(void *arg)
{
    bool *key_set = (bool *)arg;
    bool ok = key_set && *key_set;
    free(key_set);

    bsp_display_lock(0);
    if (ok) ws_start_scan();
    else    ws_show_key_missing();
    bsp_display_unlock();
}

static void ws_key_check_task(void *arg)
{
    (void)arg;
    char line[256] = {0};
    bool got = uart_send_wait_line("wpasec_key read", "WPA-SEC key",
                                   line, sizeof(line), 5000);
    bool key_set = got && (strstr(line, "not set") == NULL);

    bool *res = malloc(sizeof(bool));
    if (res) {
        *res = key_set;
        if (!ui_lvgl_async_call(ws_key_result_cb, res))
            free(res);
    }
    vTaskDelete(NULL);
}

static void on_wpasec_upload_btn(lv_event_t *e)
{
    (void)e;
    show_loading("WPA-SEC Upload", "Checking WPA-SEC key...");
    xTaskCreate(ws_key_check_task, "ws_key", 4096, NULL, 5, NULL);
}

/* ---- AP scan + picker (ported from wardrive_screen.c) ---- */

static const char *ws_parse_quoted(const char *p, char *out, int max)
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

static bool ws_parse_ap_line(const char *line, wifi_network_t *net)
{
    if (line[0] != '"') return false;
    const char *p = line;
    char field[64];
    memset(net, 0, sizeof(*net));

    p = ws_parse_quoted(p, field, sizeof(field));            if (!p) return false;
    net->index = (uint8_t)atoi(field);
    p = ws_parse_quoted(p, net->ssid, sizeof(net->ssid));    if (!p) return false;
    p = ws_parse_quoted(p, field, sizeof(field));            if (!p) return false; /* empty */
    p = ws_parse_quoted(p, net->bssid, sizeof(net->bssid));  if (!p) return false;
    p = ws_parse_quoted(p, field, sizeof(field));            if (!p) return false;
    net->channel = (uint8_t)atoi(field);
    p = ws_parse_quoted(p, net->security, sizeof(net->security)); if (!p) return false;
    p = ws_parse_quoted(p, field, sizeof(field));            if (!p) return false;
    net->rssi = (int8_t)atoi(field);
    return true;
}

static void ws_ap_collect_cb(const char **lines, int line_count)
{
    ws_aps_cnt = 0;
    for (int i = 0; i < line_count; i++) {
        wifi_network_t net;
        if (ws_parse_ap_line(lines[i], &net)) {
            if (net.ssid[0] == '\0') continue;
            if (!psram_dynarr_ensure((void **)&ws_aps, &ws_aps_cap,
                                     ws_aps_cnt + 1, sizeof(*ws_aps), 256))
                break;
            ws_aps[ws_aps_cnt++] = net;
        }
    }
    ESP_LOGI(TAG, "WPA-SEC upload: %d APs", ws_aps_cnt);
    bsp_display_lock(0);
    ws_show_ap_picker();
    bsp_display_unlock();
}

/* ---- connect ---- */

static void ws_on_connect_done(bool success, void *unused)
{
    (void)unused;
    if (!success) {
        bsp_display_lock(0);
        if (ws_status && lv_obj_is_valid(ws_status)) {
            lv_label_set_text(ws_status, "WiFi connect failed.");
            lv_obj_set_style_text_color(ws_status, UI_ACCENT_RED, 0);
        }
        bsp_display_unlock();
        return;
    }
    ws_start_upload();
}

static void ws_on_pass_confirm(const char *text, void *unused)
{
    (void)unused;
    if (text) snprintf(ws_pass, sizeof(ws_pass), "%s", text);
    bsp_display_lock(0);
    if (ws_status && lv_obj_is_valid(ws_status))
        lv_label_set_text(ws_status, "Connecting...");
    bsp_display_unlock();
    wifi_connect_async(&ws_net, ws_pass[0] ? ws_pass : NULL, ws_on_connect_done, NULL);
}

static void ws_on_ap_pick(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= ws_aps_cnt) return;
    ws_net = ws_aps[idx];
    ws_pass[0] = '\0';

    if (wifi_network_is_open(&ws_net)) {
        if (ws_status && lv_obj_is_valid(ws_status))
            lv_label_set_text(ws_status, "Connecting...");
        wifi_connect_async(&ws_net, NULL, ws_on_connect_done, NULL);
        return;
    }
    char evil[64] = {0};
    if (wifi_lookup_evil_password(ws_net.ssid, evil, sizeof(evil))) {
        snprintf(ws_pass, sizeof(ws_pass), "%s", evil);
        if (ws_status && lv_obj_is_valid(ws_status))
            lv_label_set_text(ws_status, "Connecting...");
        wifi_connect_async(&ws_net, ws_pass, ws_on_connect_done, NULL);
        return;
    }
    ui_show_text_input_popup("WiFi Password", "", 63, UI_ACCENT_GREEN,
                             ws_on_pass_confirm, NULL, NULL);
}

static void ws_on_ap_rescan(lv_event_t *e) { (void)e; ws_start_scan(); }

static void ws_show_ap_picker(void)
{
    lv_obj_t *scr = ui_screen_clear();
    lv_obj_t *bar = ui_create_top_bar(scr, "Select WiFi", on_back_menu, NULL);
    ui_add_top_bar_action(bar, LV_SYMBOL_REFRESH, ws_on_ap_rescan, NULL);

    ws_status = lv_label_create(scr);
    lv_label_set_text(ws_status, ws_aps_cnt ? "Pick an access point:" : "No networks found.");
    lv_obj_set_style_text_color(ws_status, ui_muted_color(), 0);
    lv_obj_set_style_text_font(ws_status, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(ws_status, 8, 40);

    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_set_size(list, LV_PCT(100), 240 - 60);
    lv_obj_set_pos(list, 0, 58);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 6, 0);
    lv_obj_set_style_pad_row(list, 3, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    for (int i = 0; i < ws_aps_cnt; i++) {
        lv_obj_t *btn = lv_btn_create(list);
        lv_obj_set_size(btn, LV_PCT(100), 30);
        lv_obj_set_style_bg_color(btn, ui_panel_color(), 0);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_add_event_cb(btn, ws_on_ap_pick, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *lbl = lv_label_create(btn);
        char buf[64];
        snprintf(buf, sizeof(buf), "%.24s  %ddBm%s", ws_aps[i].ssid, ws_aps[i].rssi,
                 wifi_network_is_open(&ws_aps[i]) ? "  OPEN" : "");
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_color(lbl, ui_text_color(), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_center(lbl);
    }
}

static void ws_start_scan(void)
{
    ws_aps_cnt = 0;
    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Scanning WiFi", on_back_menu, NULL);
    ws_status = lv_label_create(scr);
    lv_label_set_text(ws_status, "Scanning networks...");
    lv_obj_set_style_text_color(ws_status, ui_text_color(), 0);
    lv_obj_center(ws_status);

    uart_start_collect("Scan results printed", ws_ap_collect_cb);
    uart_send_command("scan_networks");
}

/* ---- upload progress ---- */

static void ws_upload_line_cb(const char *line)
{
    if (!line || !line[0]) return;

    bsp_display_lock(0);
    if (ws_log && lv_obj_is_valid(ws_log)) {
        lv_obj_t *l = lv_label_create(ws_log);
        lv_label_set_text(l, line);
        lv_obj_set_style_text_color(l, ui_text_color(), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_10, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, LV_PCT(100));
        lv_obj_scroll_to_view(l, LV_ANIM_OFF);
    }
    bsp_display_unlock();

    int up = 0, dup = 0, fail = 0;
    if (sscanf(line, "Done: %d uploaded, %d duplicate, %d failed", &up, &dup, &fail) == 3) {
        uart_set_line_callback(NULL);
        bsp_display_lock(0);
        if (ws_status && lv_obj_is_valid(ws_status)) {
            lv_label_set_text_fmt(ws_status, "Done: %d up, %d dup, %d failed", up, dup, fail);
            lv_obj_set_style_text_color(ws_status, UI_ACCENT_GREEN, 0);
        }
        bsp_display_unlock();
        return;
    }
    if (strstr(line, "Done") || strstr(line, "FAILED") ||
        strstr(line, "Error") || strstr(line, "Unrecognized command")) {
        uart_set_line_callback(NULL);
        bsp_display_lock(0);
        if (ws_status && lv_obj_is_valid(ws_status)) {
            lv_label_set_text(ws_status, "Finished.");
            lv_obj_set_style_text_color(ws_status, UI_ACCENT_GREEN, 0);
        }
        bsp_display_unlock();
    }
}

static void ws_on_upload_back(lv_event_t *e)
{
    (void)e;
    uart_set_line_callback(NULL);
    uart_send_command("stop");
    bsp_display_lock(0);
    show_handshake_list();
    bsp_display_unlock();
}

static void ws_start_upload(void)
{
    bsp_display_lock(0);
    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "WPA-SEC Upload", ws_on_upload_back, NULL);

    ws_status = lv_label_create(scr);
    lv_label_set_text(ws_status, "Connected. Uploading...");
    lv_obj_set_style_text_color(ws_status, UI_ACCENT_GREEN, 0);
    lv_obj_set_style_text_font(ws_status, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(ws_status, 8, 40);

    ws_log = lv_obj_create(scr);
    lv_obj_set_size(ws_log, LV_PCT(100), 240 - 60);
    lv_obj_set_pos(ws_log, 0, 58);
    lv_obj_set_style_bg_color(ws_log, ui_card_color(), 0);
    lv_obj_set_style_bg_opa(ws_log, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ws_log, 0, 0);
    lv_obj_set_style_pad_all(ws_log, 4, 0);
    lv_obj_set_style_pad_row(ws_log, 1, 0);
    lv_obj_set_flex_flow(ws_log, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(ws_log, LV_DIR_VER);
    bsp_display_unlock();

    uart_set_line_callback(ws_upload_line_cb);
    uart_handler_flush_rx();
    uart_send_command("wpasec_upload");
    ESP_LOGI(TAG, "Sent wpasec_upload");
}

/* ================================================================== */
/*  Main menu: 3 tiles                                                 */
/* ================================================================== */

void show_compromised_data_screen(void)
{
    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Compromised Data", on_back_home, NULL);

    lv_obj_t *grid = lv_obj_create(scr);
    lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(grid, 10, 0);
    lv_obj_set_style_pad_column(grid, 8, 0);
    lv_obj_set_style_pad_top(grid, 10, 0);
    lv_obj_set_style_pad_bottom(grid, 10, 0);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_y(grid, 36);

    ui_create_tile(grid, LV_SYMBOL_EYE_CLOSE, "Evil Twin\nPasswords", UI_ACCENT_GREEN,  on_evil_passwords, NULL);
    ui_create_tile(grid, LV_SYMBOL_FILE,      "Portal\nData",         UI_ACCENT_PURPLE, on_portal_data,    NULL);
    ui_create_tile(grid, LV_SYMBOL_DOWNLOAD,  "Handshakes",           UI_ACCENT_ORANGE, on_handshakes,     NULL);
}
