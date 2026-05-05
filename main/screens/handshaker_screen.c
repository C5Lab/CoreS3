#include "handshaker_screen.h"
#include "global_attacks_screen.h"
#include "attack_select_screen.h"
#include "ui_helpers.h"
#include "uart_handler.h"
#include "bsp/m5stack_core_s3.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "handshaker";
static bool from_attack_select = false;

/* ---- state ---- */
#define LOG_MAX_LEN  2048

static char     log_buf[LOG_MAX_LEN];
static int      log_len = 0;
static int      captured_count = 0;
static bool     hs_running = false;

static lv_obj_t *log_label   = NULL;
static lv_obj_t *stats_label = NULL;

/* ---- helpers ---- */

static void append_log(const char *text)
{
    int add = strlen(text);
    if (log_len + add + 2 >= LOG_MAX_LEN) {
        /* shift buffer: drop first half */
        int half = LOG_MAX_LEN / 2;
        memmove(log_buf, log_buf + half, log_len - half);
        log_len -= half;
    }
    if (log_len > 0) log_buf[log_len++] = '\n';
    memcpy(log_buf + log_len, text, add);
    log_len += add;
    log_buf[log_len] = '\0';
}

static void update_stats_ui(void)
{
    if (!stats_label) return;
    char txt[48];
    snprintf(txt, sizeof(txt), "Captured: %d", captured_count);
    lv_label_set_text(stats_label, txt);
}

/* ---- UART line callback ---- */

static void hs_uart_line_cb(const char *line)
{
    if (!hs_running) return;

    bool update_log = false;
    bool update_stats = false;

    const char *p;

    if ((p = strstr(line, "Handshakes captured so far:")) != NULL) {
        captured_count = atoi(p + strlen("Handshakes captured so far:"));
        update_stats = true;
    }

    if (strstr(line, "Attacking '") || strstr(line, ">>> [")) {
        append_log(line);
        update_log = true;
    }

    if (strstr(line, "HANDSHAKE IS COMPLETE AND VALID")) {
        append_log("** HANDSHAKE VALID **");
        update_log = true;
    }

    if ((p = strstr(line, "PCAP saved:")) != NULL) {
        const char *fname = strrchr(p, '/');
        if (fname) fname++; else fname = p + strlen("PCAP saved:");
        while (*fname == ' ') fname++;
        char entry[128];
        snprintf(entry, sizeof(entry), "Saved: %s", fname);
        append_log(entry);
        update_log = true;
    }

    if ((p = strstr(line, "handshake saved for SSID:")) != NULL) {
        p += strlen("handshake saved for SSID:");
        while (*p == ' ') p++;
        char entry[80];
        snprintf(entry, sizeof(entry), "Handshake: %s", p);
        append_log(entry);
        captured_count++;
        update_log = true;
        update_stats = true;
    }

    if (strstr(line, "No handshake for")) {
        append_log(line);
        update_log = true;
    }

    if (strstr(line, "Attack Cycle Complete")) {
        append_log("-- Cycle Complete --");
        update_log = true;
    }

    if (strstr(line, "Found") && strstr(line, "networks")) {
        append_log(line);
        update_log = true;
    }

    if (strstr(line, "Scanning")) {
        append_log(line);
        update_log = true;
    }

    if (update_log || update_stats) {
        bsp_display_lock(0);
        if (update_log && log_label)
            lv_label_set_text(log_label, log_buf);
        if (update_stats)
            update_stats_ui();
        bsp_display_unlock();
    }
}

/* ---- active screen ---- */

static void on_stop(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Handshaker STOP");
    hs_running = false;
    uart_set_line_callback(NULL);
    uart_send_command("stop");
    bsp_display_lock(0);
    if (from_attack_select)
        show_attack_select_screen();
    else
        show_global_attacks_screen();
    bsp_display_unlock();
}

static void show_active_screen(void)
{
    log_buf[0] = '\0';
    log_len = 0;
    captured_count = 0;
    hs_running = true;

    uart_send_command("start_handshake");
    ESP_LOGI(TAG, "Handshaker started");

    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Handshaker", NULL, NULL);

    /* stats row */
    stats_label = lv_label_create(scr);
    lv_label_set_text(stats_label, "Captured: 0");
    lv_obj_set_style_text_color(stats_label, UI_ACCENT_GREEN, 0);
    lv_obj_set_style_text_font(stats_label, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(stats_label, 8, 40);

    /* scrollable log area */
    lv_obj_t *log_cont = lv_obj_create(scr);
    lv_obj_set_size(log_cont, 310, 130);
    lv_obj_set_pos(log_cont, 5, 58);
    lv_obj_set_style_bg_color(log_cont, ui_card_color(), 0);
    lv_obj_set_style_bg_opa(log_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(log_cont, 6, 0);
    lv_obj_set_style_border_width(log_cont, 1, 0);
    lv_obj_set_style_border_color(log_cont, ui_muted_color(), 0);
    lv_obj_set_style_pad_all(log_cont, 4, 0);

    log_label = lv_label_create(log_cont);
    lv_label_set_text(log_label, "Starting handshake capture...");
    lv_obj_set_width(log_label, 295);
    lv_label_set_long_mode(log_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(log_label, ui_text_color(), 0);
    lv_obj_set_style_text_font(log_label, &lv_font_montserrat_10, 0);

    /* stop button */
    lv_obj_t *btn = lv_btn_create(scr);
    lv_obj_set_size(btn, 140, 34);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(btn, UI_ACCENT_RED, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_add_event_cb(btn, on_stop, LV_EVENT_CLICKED, NULL);
    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, LV_SYMBOL_CLOSE " Stop");
    lv_obj_set_style_text_color(btn_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(btn_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(btn_lbl);

    uart_set_line_callback(hs_uart_line_cb);
}

/* ---- confirm popup (shown over the global attacks menu) ---- */

static lv_obj_t *confirm_overlay = NULL;

static void close_confirm(void)
{
    if (confirm_overlay) {
        lv_obj_del(confirm_overlay);
        confirm_overlay = NULL;
    }
}

static void confirm_yes_cb(lv_event_t *e)
{
    (void)e;
    close_confirm();
    show_active_screen();
}

static void confirm_no_cb(lv_event_t *e)
{
    (void)e;
    close_confirm();
}

void show_handshaker_screen(void)
{
    from_attack_select = false;
    lv_obj_t *scr = lv_scr_act();
    confirm_overlay = lv_obj_create(scr);
    lv_obj_set_size(confirm_overlay, 280, 180);
    lv_obj_center(confirm_overlay);
    lv_obj_set_style_bg_color(confirm_overlay, ui_card_color(), 0);
    lv_obj_set_style_bg_opa(confirm_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(confirm_overlay, 12, 0);
    lv_obj_set_style_border_color(confirm_overlay, UI_ACCENT_ORANGE, 0);
    lv_obj_set_style_border_width(confirm_overlay, 2, 0);
    lv_obj_set_style_pad_all(confirm_overlay, 14, 0);
    lv_obj_set_flex_flow(confirm_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(confirm_overlay, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(confirm_overlay, 8, 0);
    lv_obj_clear_flag(confirm_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = lv_label_create(confirm_overlay);
    lv_label_set_text(icon, LV_SYMBOL_DOWNLOAD);
    lv_obj_set_style_text_color(icon, UI_ACCENT_ORANGE, 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, 0);

    lv_obj_t *title = lv_label_create(confirm_overlay);
    lv_label_set_text(title, "HANDSHAKER");
    lv_obj_set_style_text_color(title, UI_ACCENT_ORANGE, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);

    lv_obj_t *desc = lv_label_create(confirm_overlay);
    lv_label_set_text(desc, "Deauth all networks\nto grab handshakes?");
    lv_obj_set_style_text_color(desc, ui_text_color(), 0);
    lv_obj_set_style_text_font(desc, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(desc, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *btn_row = lv_obj_create(confirm_overlay);
    lv_obj_set_size(btn_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *no_btn = lv_btn_create(btn_row);
    lv_obj_set_size(no_btn, 100, 32);
    lv_obj_set_style_bg_color(no_btn, ui_muted_color(), 0);
    lv_obj_set_style_radius(no_btn, 8, 0);
    lv_obj_add_event_cb(no_btn, confirm_no_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *no_lbl = lv_label_create(no_btn);
    lv_label_set_text(no_lbl, "No");
    lv_obj_set_style_text_color(no_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(no_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(no_lbl);

    lv_obj_t *yes_btn = lv_btn_create(btn_row);
    lv_obj_set_size(yes_btn, 100, 32);
    lv_obj_set_style_bg_color(yes_btn, UI_ACCENT_ORANGE, 0);
    lv_obj_set_style_radius(yes_btn, 8, 0);
    lv_obj_add_event_cb(yes_btn, confirm_yes_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *yes_lbl = lv_label_create(yes_btn);
    lv_label_set_text(yes_lbl, "Yes");
    lv_obj_set_style_text_color(yes_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(yes_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(yes_lbl);
}

void show_handshaker_for_selected(void)
{
    from_attack_select = true;
    send_select_networks();
    show_active_screen();
}
