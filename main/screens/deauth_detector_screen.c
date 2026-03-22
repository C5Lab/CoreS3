#include "deauth_detector_screen.h"
#include "home_screen.h"
#include "ui_helpers.h"
#include "uart_handler.h"
#include "cardkb.h"
#include "esp_log.h"
#include "bsp/m5stack_core_s3.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "deauth_det";

#define MAX_DEAUTH_ENTRIES 100
#define COL_CH_W      30
#define COL_RSSI_W    38
#define COL_AP_W      110
#define COL_BSSID_W   120

/* ---------- Parsed deauth entry ---------- */
typedef struct {
    int  channel;
    int  rssi;
    char ap_name[33];
    char bssid[18];
} deauth_entry_t;

/* ---------- Module state ---------- */
static deauth_entry_t s_entries[MAX_DEAUTH_ENTRIES];
static int            s_entry_count;
static bool           s_running;

static lv_obj_t      *s_list;          /* scrollable container for rows */
static lv_obj_t      *s_count_label;   /* "Detected: N" */
static lv_obj_t      *s_btn_start;
static lv_obj_t      *s_btn_stop;
static lv_timer_t    *s_kb_timer;

/* ---------- Forward declarations ---------- */
static void on_back(lv_event_t *e);
static void on_start(lv_event_t *e);
static void on_stop(lv_event_t *e);
static void deauth_line_cb(const char *line);
static void kb_poll_cb(lv_timer_t *t);
static void add_row_to_list(const deauth_entry_t *ent);

/* ================================================================== */
/*  Parse "[DEAUTH] CH: 6 | AP: MyNet (AA:BB:CC:DD:EE:FF) | RSSI: -42" */
/* ================================================================== */
static bool parse_deauth_line(const char *line, deauth_entry_t *out)
{
    const char *tag = strstr(line, "[DEAUTH]");
    if (!tag) return false;

    /* Channel */
    const char *ch_ptr = strstr(tag, "CH:");
    if (!ch_ptr) return false;
    out->channel = atoi(ch_ptr + 3);

    /* AP name + BSSID — between "AP: " and " | RSSI:" */
    const char *ap_ptr = strstr(tag, "AP:");
    if (!ap_ptr) return false;
    ap_ptr += 4; /* skip "AP: " */

    const char *rssi_sep = strstr(ap_ptr, "| RSSI:");
    if (!rssi_sep) return false;

    /* Find '(' for BSSID */
    const char *paren = strchr(ap_ptr, '(');
    if (paren && paren < rssi_sep) {
        int name_len = (int)(paren - ap_ptr) - 1; /* -1 for space before '(' */
        if (name_len < 0) name_len = 0;
        if (name_len > 32) name_len = 32;
        memcpy(out->ap_name, ap_ptr, name_len);
        out->ap_name[name_len] = '\0';

        const char *paren_close = strchr(paren, ')');
        if (paren_close) {
            int bssid_len = (int)(paren_close - paren - 1);
            if (bssid_len > 17) bssid_len = 17;
            if (bssid_len > 0) {
                memcpy(out->bssid, paren + 1, bssid_len);
                out->bssid[bssid_len] = '\0';
            } else {
                snprintf(out->bssid, sizeof(out->bssid), "??:??:??");
            }
        }
    } else {
        /* No parenthesised BSSID */
        int name_len = (int)(rssi_sep - ap_ptr) - 1;
        if (name_len < 0) name_len = 0;
        if (name_len > 32) name_len = 32;
        memcpy(out->ap_name, ap_ptr, name_len);
        out->ap_name[name_len] = '\0';
        snprintf(out->bssid, sizeof(out->bssid), "N/A");
    }

    /* RSSI */
    const char *rssi_ptr = strstr(tag, "RSSI:");
    if (!rssi_ptr) return false;
    out->rssi = atoi(rssi_ptr + 5);

    return true;
}

/* ================================================================== */
/*  UART callback — runs on UART task, schedule LVGL update           */
/* ================================================================== */
static void deauth_line_cb(const char *line)
{
    deauth_entry_t ent;
    if (!parse_deauth_line(line, &ent)) return;

    /* Shift entries down (newest first) */
    if (s_entry_count < MAX_DEAUTH_ENTRIES)
        s_entry_count++;
    for (int i = s_entry_count - 1; i > 0; i--)
        s_entries[i] = s_entries[i - 1];
    s_entries[0] = ent;

    /* Update UI (LVGL not thread-safe — guard with display lock) */
    bsp_display_lock(0);

    /* Update counter */
    if (s_count_label) {
        lv_label_set_text_fmt(s_count_label, "Detected: %d", s_entry_count);
    }

    /* Prepend a new row at the top of the list */
    add_row_to_list(&s_entries[0]);
    if (s_list) {
        lv_obj_move_to_index(lv_obj_get_child(s_list,
            lv_obj_get_child_count(s_list) - 1), 0);

        /* Remove excess rows from the bottom */
        while (lv_obj_get_child_count(s_list) > MAX_DEAUTH_ENTRIES) {
            lv_obj_t *last = lv_obj_get_child(s_list,
                lv_obj_get_child_count(s_list) - 1);
            lv_obj_delete(last);
        }
    }

    bsp_display_unlock();
}

/* ================================================================== */
/*  Build one row inside the scrollable list                          */
/* ================================================================== */
static void add_row_to_list(const deauth_entry_t *ent)
{
    if (!s_list) return;

    lv_obj_t *row = lv_obj_create(s_list);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(row, 2, 0);
    lv_obj_set_style_pad_gap(row, 2, 0);
    lv_obj_set_style_bg_color(row, ui_card_color(), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 4, 0);
    lv_obj_set_style_min_height(row, 18, 0);

    /* CH */
    lv_obj_t *lbl_ch = lv_label_create(row);
    lv_obj_set_width(lbl_ch, COL_CH_W);
    lv_obj_set_style_text_font(lbl_ch, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_ch, UI_ACCENT_CYAN, 0);
    lv_label_set_text_fmt(lbl_ch, "%d", ent->channel);

    /* RSSI */
    lv_obj_t *lbl_rssi = lv_label_create(row);
    lv_obj_set_width(lbl_rssi, COL_RSSI_W);
    lv_obj_set_style_text_font(lbl_rssi, &lv_font_montserrat_12, 0);
    lv_color_t rssi_col = (ent->rssi > -50) ? UI_ACCENT_GREEN :
                          (ent->rssi > -70) ? UI_ACCENT_ORANGE : UI_ACCENT_RED;
    lv_obj_set_style_text_color(lbl_rssi, rssi_col, 0);
    lv_label_set_text_fmt(lbl_rssi, "%d", ent->rssi);

    /* AP name */
    lv_obj_t *lbl_ap = lv_label_create(row);
    lv_obj_set_width(lbl_ap, COL_AP_W);
    lv_obj_set_style_text_font(lbl_ap, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_ap, ui_text_color(), 0);
    lv_label_set_long_mode(lbl_ap, LV_LABEL_LONG_CLIP);
    lv_label_set_text(lbl_ap, ent->ap_name);

    /* BSSID */
    lv_obj_t *lbl_bssid = lv_label_create(row);
    lv_obj_set_width(lbl_bssid, COL_BSSID_W);
    lv_obj_set_style_text_font(lbl_bssid, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_bssid, ui_muted_color(), 0);
    lv_label_set_long_mode(lbl_bssid, LV_LABEL_LONG_CLIP);
    lv_label_set_text(lbl_bssid, ent->bssid);
}

/* ================================================================== */
/*  CardKB polling                                                    */
/* ================================================================== */
static void kb_poll_cb(lv_timer_t *t)
{
    (void)t;
    uint8_t key = cardkb_read_key();
    if (key == 0) return;

    if (key == 0x1B || key == 0x08 || key == 0x7F) { /* Esc / Backspace */
        if (s_running) {
            uart_send_command("stop");
            uart_set_line_callback(NULL);
            s_running = false;
        }
        if (s_kb_timer) { lv_timer_delete(s_kb_timer); s_kb_timer = NULL; }
        show_home_screen();
    }
}

/* ================================================================== */
/*  Button handlers                                                   */
/* ================================================================== */
static void on_start(lv_event_t *e)
{
    (void)e;
    if (s_running) return;
    s_running = true;

    /* Clear previous data */
    s_entry_count = 0;
    if (s_list) lv_obj_clean(s_list);
    if (s_count_label) lv_label_set_text(s_count_label, "Detected: 0");

    lv_obj_add_state(s_btn_start, LV_STATE_DISABLED);
    lv_obj_clear_state(s_btn_stop, LV_STATE_DISABLED);

    uart_set_line_callback(deauth_line_cb);
    uart_send_command("deauth_detector");
    ESP_LOGI(TAG, "Deauth detection started");
}

static void on_stop(lv_event_t *e)
{
    (void)e;
    if (!s_running) return;
    s_running = false;

    uart_send_command("stop");
    uart_set_line_callback(NULL);

    lv_obj_clear_state(s_btn_start, LV_STATE_DISABLED);
    lv_obj_add_state(s_btn_stop, LV_STATE_DISABLED);

    ESP_LOGI(TAG, "Deauth detection stopped");
}

static void on_back(lv_event_t *e)
{
    (void)e;
    if (s_running) {
        uart_send_command("stop");
        uart_set_line_callback(NULL);
        s_running = false;
    }
    if (s_kb_timer) { lv_timer_delete(s_kb_timer); s_kb_timer = NULL; }
    show_home_screen();
}

/* ================================================================== */
/*  Main screen builder                                               */
/* ================================================================== */
void show_deauth_detector_screen(void)
{
    s_entry_count = 0;
    s_running     = false;
    s_list        = NULL;
    s_count_label = NULL;
    s_btn_start   = NULL;
    s_btn_stop    = NULL;
    s_kb_timer    = NULL;

    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Deauth Detector", on_back, NULL);

    /* --- Control bar: Start / Stop / counter --- */
    lv_obj_t *ctrl = lv_obj_create(scr);
    lv_obj_set_size(ctrl, LV_PCT(100), 36);
    lv_obj_set_flex_flow(ctrl, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctrl, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(ctrl, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ctrl, 0, 0);
    lv_obj_set_style_pad_all(ctrl, 2, 0);
    lv_obj_set_y(ctrl, 36);

    /* Start button */
    s_btn_start = lv_btn_create(ctrl);
    lv_obj_set_size(s_btn_start, 90, 28);
    lv_obj_set_style_bg_color(s_btn_start, UI_ACCENT_GREEN, 0);
    lv_obj_set_style_radius(s_btn_start, 6, 0);
    lv_obj_add_event_cb(s_btn_start, on_start, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_s = lv_label_create(s_btn_start);
    lv_label_set_text(lbl_s, LV_SYMBOL_PLAY " Start");
    lv_obj_set_style_text_font(lbl_s, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl_s);

    /* Stop button */
    s_btn_stop = lv_btn_create(ctrl);
    lv_obj_set_size(s_btn_stop, 90, 28);
    lv_obj_set_style_bg_color(s_btn_stop, UI_ACCENT_RED, 0);
    lv_obj_set_style_radius(s_btn_stop, 6, 0);
    lv_obj_add_event_cb(s_btn_stop, on_stop, LV_EVENT_CLICKED, NULL);
    lv_obj_add_state(s_btn_stop, LV_STATE_DISABLED);
    lv_obj_t *lbl_t = lv_label_create(s_btn_stop);
    lv_label_set_text(lbl_t, LV_SYMBOL_STOP " Stop");
    lv_obj_set_style_text_font(lbl_t, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl_t);

    /* Counter label */
    s_count_label = lv_label_create(ctrl);
    lv_obj_set_style_text_font(s_count_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_count_label, UI_ACCENT_CYAN, 0);
    lv_label_set_text(s_count_label, "Detected: 0");

    /* --- Column header row --- */
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_set_size(hdr, LV_PCT(100), 18);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(hdr, 2, 0);
    lv_obj_set_style_pad_gap(hdr, 2, 0);
    lv_obj_set_style_bg_color(hdr, ui_panel_color(), 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_y(hdr, 72);

    static const struct { const char *txt; int w; } cols[] = {
        {"CH",   COL_CH_W},
        {"RSSI", COL_RSSI_W},
        {"AP",   COL_AP_W},
        {"BSSID",COL_BSSID_W},
    };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *l = lv_label_create(hdr);
        lv_obj_set_width(l, cols[i].w);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(l, ui_muted_color(), 0);
        lv_label_set_text(l, cols[i].txt);
    }

    /* --- Scrollable results list --- */
    s_list = lv_obj_create(scr);
    lv_obj_set_size(s_list, LV_PCT(100), 240 - 90);  /* remaining space */
    lv_obj_set_y(s_list, 90);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_list, 2, 0);
    lv_obj_set_style_pad_gap(s_list, 2, 0);
    lv_obj_set_style_bg_color(s_list, ui_bg_color(), 0);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_AUTO);

    /* CardKB polling */
    s_kb_timer = lv_timer_create(kb_poll_cb, 50, NULL);

    ESP_LOGI(TAG, "Deauth Detector screen ready");
}
