#include "compromised_data_screen.h"
#include "home_screen.h"
#include "ui_helpers.h"
#include "uart_handler.h"
#include "bsp/m5stack_core_s3.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static const char *TAG = "compromised";

/* ================================================================== */
/*  Shared data structures                                             */
/* ================================================================== */

#define MAX_ENTRIES 64

/* Evil Twin passwords: "SSID", "password" */
typedef struct {
    char ssid[33];
    char password[64];
} evil_pass_entry_t;

static evil_pass_entry_t evil_entries[MAX_ENTRIES];
static int evil_count = 0;

/* Portal data: "SSID", "field1=val1", ... */
typedef struct {
    char ssid[33];
    char fields[192];   /* all fields concatenated, separated by \n */
} portal_entry_t;

static portal_entry_t portal_entries[MAX_ENTRIES];
static int portal_count = 0;

/* Handshake files */
typedef struct {
    int  number;
    char filename[128];
} handshake_entry_t;

static handshake_entry_t hs_entries[MAX_ENTRIES];
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

    if (evil_count >= MAX_ENTRIES) return;

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

    if (portal_count >= MAX_ENTRIES) return;

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
    ui_create_top_bar(scr, "Handshakes", on_back_menu, NULL);

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

    for (int i = 0; i < line_count && hs_count < MAX_ENTRIES; i++) {
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
