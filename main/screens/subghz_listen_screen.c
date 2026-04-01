#include "subghz_listen_screen.h"
#include "subghz_screen.h"
#include "ui_helpers.h"
#include "uart_handler.h"
#include "cardkb.h"
#include "esp_log.h"
#include "bsp/m5stack_core_s3.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static const char *TAG = "subghz_listen";

#define WATERFALL_W       320
#define WATERFALL_H       60
#define MAX_SIGNALS       64
#define COL_IDX_W         22
#define COL_TYPE_W        60
#define COL_FREQ_W        55
#define COL_ID_W          70
#define COL_BITS_W        40

typedef struct {
    int   idx;
    char  type[16];
    float freq;
    char  id[24];
    int   bits;
    bool  is_raw;
} subghz_signal_t;

static subghz_signal_t s_signals[MAX_SIGNALS];
static int             s_signal_count;
static int             s_seen_idx[MAX_SIGNALS];
static int             s_seen_count;
static bool            s_running;
static bool            s_raw_mode;
static float           s_freq_mhz = 433.92f;

static lv_obj_t   *s_canvas;
static lv_color_t *s_canvas_buf;
static lv_obj_t   *s_sig_list;
static lv_obj_t   *s_sig_count_lbl;
static lv_obj_t   *s_freq_lbl;
static lv_obj_t   *s_btn_start;
static lv_obj_t   *s_btn_stop;
static lv_obj_t   *s_btn_raw;
static lv_obj_t   *s_freq_popup;
static lv_obj_t   *s_rollers[5];
static lv_timer_t *s_kb_timer;

static void on_back(lv_event_t *e);
static void on_start(lv_event_t *e);
static void on_stop(lv_event_t *e);
static void on_raw_toggle(lv_event_t *e);
static void on_freq_tap(lv_event_t *e);
static void subghz_line_cb(const char *line);
static void kb_poll_cb(lv_timer_t *t);
static void add_signal_row(const subghz_signal_t *sig);

static lv_color_t rssi_to_color(int rssi)
{
    if (rssi > -50)  return lv_color_hex(0xFF0000);
    if (rssi > -70)  return lv_color_hex(0xCC0000);
    if (rssi > -85)  return lv_color_hex(0x990000);
    if (rssi > -95)  return lv_color_hex(0x550000);
    if (rssi > -105) return lv_color_hex(0x2A0000);
    return lv_color_hex(0x120000);
}

#define WF_BG_COLOR    0x0A1628
#define WF_GRID_COLOR  0x152540

static void waterfall_fill_bg(void)
{
    lv_color_t bg   = lv_color_hex(WF_BG_COLOR);
    lv_color_t grid = lv_color_hex(WF_GRID_COLOR);
    for (int y = 0; y < WATERFALL_H; y++) {
        lv_color_t c = (y % 10 == 0) ? grid : bg;
        for (int x = 0; x < WATERFALL_W; x++)
            s_canvas_buf[y * WATERFALL_W + x] = c;
    }
}

static void waterfall_push_rssi(int rssi)
{
    if (!s_canvas || !s_canvas_buf) return;

    for (int y = 0; y < WATERFALL_H; y++) {
        for (int x = 0; x < WATERFALL_W - 1; x++) {
            s_canvas_buf[y * WATERFALL_W + x] = s_canvas_buf[y * WATERFALL_W + x + 1];
        }
    }

    lv_color_t col = rssi_to_color(rssi);
    int bar_h = WATERFALL_H;
    if (rssi < -105) bar_h = 2;
    else if (rssi < -95) bar_h = (int)(WATERFALL_H * 0.25f);
    else if (rssi < -85) bar_h = (int)(WATERFALL_H * 0.5f);
    else if (rssi < -70) bar_h = (int)(WATERFALL_H * 0.75f);

    lv_color_t bg   = lv_color_hex(WF_BG_COLOR);
    lv_color_t grid = lv_color_hex(WF_GRID_COLOR);
    for (int y = 0; y < WATERFALL_H; y++) {
        if (y >= (WATERFALL_H - bar_h))
            s_canvas_buf[y * WATERFALL_W + WATERFALL_W - 1] = col;
        else
            s_canvas_buf[y * WATERFALL_W + WATERFALL_W - 1] = (y % 10 == 0) ? grid : bg;
    }

    lv_obj_invalidate(s_canvas);
}

static bool parse_subghz_rx(const char *line, subghz_signal_t *out)
{
    memset(out, 0, sizeof(*out));
    if (strstr(line, "[SUBGHZ_RX] ") || strstr(line, "[SUBGHZ_RX_DUP] ")) {
        out->is_raw = false;
        const char *data = strstr(line, "] ");
        if (!data) return false;
        data += 2;
        sscanf(data, "idx=%d type=%15s freq=%f id=%23s",
               &out->idx, out->type, &out->freq, out->id);
        return true;
    }
    if (strstr(line, "[SUBGHZ_RAW] ")) {
        out->is_raw = true;
        const char *data = strstr(line, "] ");
        if (!data) return false;
        data += 2;
        sscanf(data, "idx=%d freq=%f edges=%d",
               &out->idx, &out->freq, &out->bits);
        snprintf(out->type, sizeof(out->type), "RAW");
        snprintf(out->id, sizeof(out->id), "--");
        return true;
    }
    return false;
}

static void subghz_line_cb(const char *line)
{
    if (strstr(line, "[SUBGHZ_RSSI] ")) {
        int rssi = atoi(line + 14);
        bsp_display_lock(0);
        waterfall_push_rssi(rssi);
        bsp_display_unlock();
        return;
    }

    if (!s_raw_mode && strstr(line, "[SUBGHZ_RAW] "))
        return;

    if (s_raw_mode && strstr(line, "[SUBGHZ_RX_DUP] "))
        return;

    subghz_signal_t sig;
    if (parse_subghz_rx(line, &sig)) {
        bool already_seen = false;
        for (int i = 0; i < s_seen_count; i++) {
            if (s_seen_idx[i] == sig.idx) { already_seen = true; break; }
        }
        if (already_seen) return;

        if (s_seen_count < MAX_SIGNALS)
            s_seen_idx[s_seen_count++] = sig.idx;

        if (s_signal_count < MAX_SIGNALS)
            s_signals[s_signal_count++] = sig;

        bsp_display_lock(0);
        if (s_sig_count_lbl)
            lv_label_set_text_fmt(s_sig_count_lbl, "Sig: %d", s_signal_count);
        add_signal_row(&sig);
        bsp_display_unlock();
    }
}

static void add_signal_row(const subghz_signal_t *sig)
{
    if (!s_sig_list) return;

    lv_obj_t *row = lv_obj_create(s_sig_list);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(row, 1, 0);
    lv_obj_set_style_pad_gap(row, 2, 0);
    lv_obj_set_style_bg_color(row, ui_card_color(), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 3, 0);
    lv_obj_set_style_min_height(row, 16, 0);

    lv_obj_t *l;

    l = lv_label_create(row);
    lv_obj_set_width(l, COL_IDX_W);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(l, UI_ACCENT_CYAN, 0);
    lv_label_set_text_fmt(l, "%d", sig->idx);

    l = lv_label_create(row);
    lv_obj_set_width(l, COL_TYPE_W);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(l, sig->is_raw ? UI_ACCENT_ORANGE : UI_ACCENT_GREEN, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_label_set_text(l, sig->type);

    l = lv_label_create(row);
    lv_obj_set_width(l, COL_FREQ_W);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(l, ui_muted_color(), 0);
    lv_label_set_text_fmt(l, "%.2f", sig->freq);

    l = lv_label_create(row);
    lv_obj_set_width(l, COL_ID_W);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(l, ui_text_color(), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_label_set_text(l, sig->id);

    l = lv_label_create(row);
    lv_obj_set_width(l, COL_BITS_W);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(l, ui_muted_color(), 0);
    if (sig->is_raw)
        lv_label_set_text_fmt(l, "%de", sig->bits);
    else
        lv_label_set_text_fmt(l, "%d", sig->bits);
}

static void close_freq_popup(void)
{
    if (s_freq_popup) {
        lv_obj_delete(s_freq_popup);
        s_freq_popup = NULL;
    }
}

static void update_freq_label(void)
{
    if (s_freq_lbl)
        lv_label_set_text_fmt(s_freq_lbl, "%.2f MHz", s_freq_mhz);
}

static void on_freq_set(lv_event_t *e)
{
    (void)e;
    int d[5];
    for (int i = 0; i < 5; i++)
        d[i] = (int)lv_roller_get_selected(s_rollers[i]);

    s_freq_mhz = d[0] * 100.0f + d[1] * 10.0f + d[2] * 1.0f
               + d[3] * 0.1f + d[4] * 0.01f;
    update_freq_label();
    close_freq_popup();
}

static void on_freq_cancel(lv_event_t *e)
{
    (void)e;
    close_freq_popup();
}

static void freq_decompose(float freq, int digits[5])
{
    int val = (int)(freq * 100.0f + 0.5f);
    digits[0] = (val / 10000) % 10;
    digits[1] = (val / 1000) % 10;
    digits[2] = (val / 100) % 10;
    digits[3] = (val / 10) % 10;
    digits[4] = val % 10;
}

static const char *s_digit_opts = "0\n1\n2\n3\n4\n5\n6\n7\n8\n9";

static void on_freq_tap(lv_event_t *e)
{
    (void)e;
    if (s_running) return;
    if (s_freq_popup) { close_freq_popup(); return; }

    int digits[5];
    freq_decompose(s_freq_mhz, digits);

    s_freq_popup = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_freq_popup, 260, 160);
    lv_obj_center(s_freq_popup);
    style_popup_card(s_freq_popup, 10, UI_ACCENT_PINK);
    lv_obj_set_flex_flow(s_freq_popup, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_freq_popup, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(s_freq_popup, 8, 0);
    lv_obj_set_style_pad_gap(s_freq_popup, 6, 0);
    lv_obj_clear_flag(s_freq_popup, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_freq_popup);
    lv_label_set_text(title, "Frequency (MHz)");
    lv_obj_set_style_text_color(title, ui_text_color(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);

    lv_obj_t *roller_row = lv_obj_create(s_freq_popup);
    lv_obj_set_size(roller_row, 240, 70);
    lv_obj_set_flex_flow(roller_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(roller_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(roller_row, 0, 0);
    lv_obj_set_style_pad_gap(roller_row, 2, 0);
    lv_obj_set_style_bg_opa(roller_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(roller_row, 0, 0);
    lv_obj_clear_flag(roller_row, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 5; i++) {
        if (i == 3) {
            lv_obj_t *dot = lv_label_create(roller_row);
            lv_label_set_text(dot, ".");
            lv_obj_set_style_text_font(dot, &lv_font_montserrat_20, 0);
            lv_obj_set_style_text_color(dot, ui_text_color(), 0);
        }
        s_rollers[i] = lv_roller_create(roller_row);
        lv_roller_set_options(s_rollers[i], s_digit_opts, LV_ROLLER_MODE_INFINITE);
        lv_roller_set_visible_row_count(s_rollers[i], 3);
        lv_obj_set_width(s_rollers[i], 36);
        lv_obj_set_style_bg_color(s_rollers[i], ui_card_color(), 0);
        lv_obj_set_style_bg_opa(s_rollers[i], LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(s_rollers[i], ui_text_color(), 0);
        lv_obj_set_style_text_font(s_rollers[i], &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(s_rollers[i], UI_ACCENT_CYAN, LV_PART_SELECTED);
        lv_obj_set_style_bg_color(s_rollers[i], ui_panel_color(), LV_PART_SELECTED);
        lv_obj_set_style_border_width(s_rollers[i], 0, 0);
        lv_obj_set_style_radius(s_rollers[i], 6, 0);
        lv_roller_set_selected(s_rollers[i], digits[i], LV_ANIM_OFF);
    }

    lv_obj_t *btn_row = lv_obj_create(s_freq_popup);
    lv_obj_set_size(btn_row, LV_PCT(100), 30);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *set_btn = lv_btn_create(btn_row);
    lv_obj_set_size(set_btn, 90, 26);
    lv_obj_set_style_bg_color(set_btn, UI_ACCENT_GREEN, 0);
    lv_obj_set_style_radius(set_btn, 6, 0);
    lv_obj_add_event_cb(set_btn, on_freq_set, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl2 = lv_label_create(set_btn);
    lv_label_set_text(sl2, "Set");
    lv_obj_set_style_text_color(sl2, lv_color_white(), 0);
    lv_obj_set_style_text_font(sl2, &lv_font_montserrat_12, 0);
    lv_obj_center(sl2);

    lv_obj_t *cancel_btn = lv_btn_create(btn_row);
    lv_obj_set_size(cancel_btn, 90, 26);
    lv_obj_set_style_bg_color(cancel_btn, ui_muted_color(), 0);
    lv_obj_set_style_radius(cancel_btn, 6, 0);
    lv_obj_add_event_cb(cancel_btn, on_freq_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(cancel_btn);
    lv_label_set_text(cl, "Cancel");
    lv_obj_set_style_text_color(cl, lv_color_white(), 0);
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_12, 0);
    lv_obj_center(cl);
}

static void stop_listening(void)
{
    if (!s_running) return;
    s_running = false;
    uart_send_command("subghz_stop");
    uart_set_line_callback(NULL);

    if (s_btn_start) lv_obj_clear_state(s_btn_start, LV_STATE_DISABLED);
    if (s_btn_stop)  lv_obj_add_state(s_btn_stop, LV_STATE_DISABLED);
    if (s_btn_raw)   lv_obj_clear_state(s_btn_raw, LV_STATE_DISABLED);
    ESP_LOGI(TAG, "SubGHz listen stopped");
}

static void on_start(lv_event_t *e)
{
    (void)e;
    if (s_running) return;
    s_running = true;

    s_signal_count = 0;
    s_seen_count   = 0;
    if (s_sig_list) lv_obj_clean(s_sig_list);
    if (s_sig_count_lbl) lv_label_set_text(s_sig_count_lbl, "Sig: 0");

    if (s_canvas_buf) {
        waterfall_fill_bg();
        if (s_canvas) lv_obj_invalidate(s_canvas);
    }

    lv_obj_add_state(s_btn_start, LV_STATE_DISABLED);
    lv_obj_clear_state(s_btn_stop, LV_STATE_DISABLED);
    lv_obj_add_state(s_btn_raw, LV_STATE_DISABLED);

    char cmd[32];
    snprintf(cmd, sizeof(cmd), "subghz_freq %.2f", s_freq_mhz);
    uart_send_command(cmd);

    uart_set_line_callback(subghz_line_cb);

    if (s_raw_mode)
        uart_send_command("subghz_rx raw");
    else
        uart_send_command("subghz_rx");

    ESP_LOGI(TAG, "SubGHz listen started (%.2f MHz, raw=%d)", s_freq_mhz, s_raw_mode);
}

static void on_stop(lv_event_t *e)
{
    (void)e;
    stop_listening();
}

static void on_raw_toggle(lv_event_t *e)
{
    (void)e;
    s_raw_mode = !s_raw_mode;
    if (s_btn_raw) {
        lv_obj_t *lbl = lv_obj_get_child(s_btn_raw, 0);
        if (lbl) lv_label_set_text(lbl, s_raw_mode ? "RAW" : "Dec");
    }
}

static void on_back(lv_event_t *e)
{
    (void)e;
    stop_listening();
    close_freq_popup();
    if (s_kb_timer) { lv_timer_delete(s_kb_timer); s_kb_timer = NULL; }

    if (s_canvas_buf) {
        heap_caps_free(s_canvas_buf);
        s_canvas_buf = NULL;
    }
    s_canvas = NULL;

    show_subghz_screen();
}

static void kb_poll_cb(lv_timer_t *t)
{
    (void)t;
    uint8_t key = cardkb_read_key();
    if (key == 0) return;
    if (key == 0x1B || key == 0x08 || key == 0x7F) {
        on_back(NULL);
    }
}

void show_subghz_listen_screen(void)
{
    s_signal_count = 0;
    s_seen_count   = 0;
    s_running      = false;
    s_sig_list     = NULL;
    s_sig_count_lbl = NULL;
    s_freq_lbl     = NULL;
    s_btn_start    = NULL;
    s_btn_stop     = NULL;
    s_btn_raw      = NULL;
    s_freq_popup   = NULL;
    s_kb_timer     = NULL;
    s_canvas       = NULL;

    lv_obj_t *scr = ui_screen_clear();

    /* Top bar with freq label */
    lv_obj_t *bar = ui_create_top_bar(scr, "Listen", on_back, NULL);

    s_freq_lbl = lv_label_create(bar);
    lv_label_set_text_fmt(s_freq_lbl, "%.2f MHz", s_freq_mhz);
    lv_obj_set_style_text_color(s_freq_lbl, UI_ACCENT_PINK, 0);
    lv_obj_set_style_text_font(s_freq_lbl, &lv_font_montserrat_12, 0);
    lv_obj_add_flag(s_freq_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_freq_lbl, on_freq_tap, LV_EVENT_CLICKED, NULL);

    /* Control bar */
    lv_obj_t *ctrl = lv_obj_create(scr);
    lv_obj_set_size(ctrl, LV_PCT(100), 30);
    lv_obj_set_y(ctrl, 36);
    lv_obj_set_flex_flow(ctrl, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctrl, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(ctrl, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ctrl, 0, 0);
    lv_obj_set_style_pad_all(ctrl, 2, 0);

    s_btn_start = lv_btn_create(ctrl);
    lv_obj_set_size(s_btn_start, 60, 24);
    lv_obj_set_style_bg_color(s_btn_start, UI_ACCENT_GREEN, 0);
    lv_obj_set_style_radius(s_btn_start, 5, 0);
    lv_obj_add_event_cb(s_btn_start, on_start, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(s_btn_start);
    lv_label_set_text(sl, LV_SYMBOL_PLAY " Start");
    lv_obj_set_style_text_font(sl, &lv_font_montserrat_10, 0);
    lv_obj_center(sl);

    s_btn_stop = lv_btn_create(ctrl);
    lv_obj_set_size(s_btn_stop, 60, 24);
    lv_obj_set_style_bg_color(s_btn_stop, UI_ACCENT_RED, 0);
    lv_obj_set_style_radius(s_btn_stop, 5, 0);
    lv_obj_add_event_cb(s_btn_stop, on_stop, LV_EVENT_CLICKED, NULL);
    lv_obj_add_state(s_btn_stop, LV_STATE_DISABLED);
    lv_obj_t *tl = lv_label_create(s_btn_stop);
    lv_label_set_text(tl, LV_SYMBOL_STOP " Stop");
    lv_obj_set_style_text_font(tl, &lv_font_montserrat_10, 0);
    lv_obj_center(tl);

    s_btn_raw = lv_btn_create(ctrl);
    lv_obj_set_size(s_btn_raw, 40, 24);
    lv_obj_set_style_bg_color(s_btn_raw, ui_card_color(), 0);
    lv_obj_set_style_bg_color(s_btn_raw, ui_card_pressed_color(), LV_STATE_PRESSED);
    lv_obj_set_style_radius(s_btn_raw, 5, 0);
    lv_obj_add_event_cb(s_btn_raw, on_raw_toggle, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rl = lv_label_create(s_btn_raw);
    lv_label_set_text(rl, s_raw_mode ? "RAW" : "Dec");
    lv_obj_set_style_text_color(rl, ui_text_color(), 0);
    lv_obj_set_style_text_font(rl, &lv_font_montserrat_10, 0);
    lv_obj_center(rl);

    s_sig_count_lbl = lv_label_create(ctrl);
    lv_obj_set_style_text_font(s_sig_count_lbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_sig_count_lbl, UI_ACCENT_CYAN, 0);
    lv_label_set_text(s_sig_count_lbl, "Sig: 0");

    /* Waterfall canvas */
    size_t buf_sz = WATERFALL_W * WATERFALL_H * sizeof(lv_color_t);
    s_canvas_buf = heap_caps_malloc(buf_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_canvas_buf)
        s_canvas_buf = malloc(buf_sz);

    if (s_canvas_buf) {
        waterfall_fill_bg();

        s_canvas = lv_canvas_create(scr);
        lv_canvas_set_buffer(s_canvas, s_canvas_buf, WATERFALL_W, WATERFALL_H, LV_COLOR_FORMAT_NATIVE);
        lv_obj_set_pos(s_canvas, 0, 66);
    }

    /* Signal table header */
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_set_size(hdr, LV_PCT(100), 14);
    lv_obj_set_y(hdr, 128);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(hdr, 1, 0);
    lv_obj_set_style_pad_gap(hdr, 2, 0);
    lv_obj_set_style_bg_color(hdr, ui_panel_color(), 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);

    static const struct { const char *t; int w; } cols[] = {
        {"#", COL_IDX_W}, {"Type", COL_TYPE_W}, {"Freq", COL_FREQ_W},
        {"ID", COL_ID_W}, {"Bits", COL_BITS_W},
    };
    for (int i = 0; i < 5; i++) {
        lv_obj_t *l = lv_label_create(hdr);
        lv_obj_set_width(l, cols[i].w);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(l, ui_muted_color(), 0);
        lv_label_set_text(l, cols[i].t);
    }

    /* Signal list */
    s_sig_list = lv_obj_create(scr);
    lv_obj_set_size(s_sig_list, LV_PCT(100), 240 - 142);
    lv_obj_set_y(s_sig_list, 142);
    lv_obj_set_flex_flow(s_sig_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_sig_list, 2, 0);
    lv_obj_set_style_pad_gap(s_sig_list, 1, 0);
    lv_obj_set_style_bg_color(s_sig_list, ui_bg_color(), 0);
    lv_obj_set_style_bg_opa(s_sig_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_sig_list, 0, 0);
    lv_obj_set_scrollbar_mode(s_sig_list, LV_SCROLLBAR_MODE_AUTO);

    s_kb_timer = lv_timer_create(kb_poll_cb, 50, NULL);

    ESP_LOGI(TAG, "SubGHz Listen screen ready");
}
