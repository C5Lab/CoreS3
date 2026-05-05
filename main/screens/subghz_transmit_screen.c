#include "subghz_transmit_screen.h"
#include "subghz_screen.h"
#include "subghz_parser.h"
#include "ui_helpers.h"
#include "uart_handler.h"
#include "cardkb.h"
#include "psram_dynarr.h"
#include "esp_log.h"
#include "bsp/m5stack_core_s3.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "subghz_tx";

#define SUBGHZ_TX_SIG_HARD_CAP 2048

typedef struct {
    int   idx;
    char  type[32];
    float freq;
    char  serial[32];
    int   btn;
    int   cnt;
    char  mf[32];
} tx_signal_t;

static tx_signal_t *s_sigs;
static int          s_sig_cap;
static int          s_sig_count;
static lv_obj_t    *s_list;
static lv_obj_t    *s_status_lbl;
static lv_obj_t    *s_tx_popup;
static int          s_pending_tx_idx;
static lv_timer_t  *s_kb_timer;

static void on_back(lv_event_t *e);
static void kb_poll_cb(lv_timer_t *t);

static void build_list(void);
static void on_signal_tap(lv_event_t *e);
static void fill_signal(tx_signal_t *dst, const subghz_signal_info_t *src);

static void fill_signal(tx_signal_t *dst, const subghz_signal_info_t *src)
{
    memset(dst, 0, sizeof(*dst));
    dst->idx = src->idx;
    dst->freq = src->freq;
    dst->btn = src->btn;
    dst->cnt = src->cnt;
    snprintf(dst->type, sizeof(dst->type), "%s", src->type[0] ? src->type : "--");
    snprintf(dst->serial, sizeof(dst->serial), "%s", src->serial[0] ? src->serial : "--");
    snprintf(dst->mf, sizeof(dst->mf), "%s", src->mf[0] ? src->mf : "--");
}

static void on_list_received(const char **lines, int count)
{
    s_sig_count = 0;
    for (int i = 0; i < count; i++) {
        subghz_signal_info_t parsed;

        if (!subghz_parse_signal_line(lines[i], &parsed) || parsed.kind != SUBGHZ_SIGNAL_KIND_LIST)
            continue;

        if (!psram_dynarr_ensure((void **)&s_sigs, &s_sig_cap,
                                 s_sig_count + 1, sizeof(*s_sigs),
                                 SUBGHZ_TX_SIG_HARD_CAP)) {
            ESP_LOGW(TAG, "tx signal cap reached at %d, dropping rest", s_sig_count);
            break;
        }

        tx_signal_t *s = &s_sigs[s_sig_count];
        fill_signal(s, &parsed);
        if (s->idx > 0) {
            s_sig_count++;
        }
    }

    bsp_display_lock(0);
    build_list();
    bsp_display_unlock();
}

static void build_list(void)
{
    if (!s_list) return;
    lv_obj_clean(s_list);

    if (s_sig_count == 0) {
        lv_obj_t *l = lv_label_create(s_list);
        lv_label_set_text(l, "No stored signals");
        lv_obj_set_style_text_color(l, ui_muted_color(), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
        return;
    }

    for (int i = 0; i < s_sig_count; i++) {
        tx_signal_t *sig = &s_sigs[i];

        lv_obj_t *row = lv_obj_create(s_list);
        lv_obj_set_size(row, LV_PCT(100), 32);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(row, 4, 0);
        lv_obj_set_style_pad_gap(row, 6, 0);
        lv_obj_set_style_bg_color(row, ui_card_color(), 0);
        lv_obj_set_style_bg_color(row, ui_card_pressed_color(), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 5, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, on_signal_tap, LV_EVENT_CLICKED, (void *)(intptr_t)sig->idx);

        lv_obj_t *l;

        l = lv_label_create(row);
        lv_obj_set_width(l, 22);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(l, UI_ACCENT_GREEN, 0);
        lv_label_set_text_fmt(l, "%d", sig->idx);

        l = lv_label_create(row);
        lv_obj_set_width(l, 60);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(l, ui_text_color(), 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        lv_label_set_text(l, sig->type);

        l = lv_label_create(row);
        lv_obj_set_width(l, 50);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(l, ui_muted_color(), 0);
        lv_label_set_text_fmt(l, "%d.%02d", (int)sig->freq, ((int)(sig->freq * 100.0f + 0.5f)) % 100);

        l = lv_label_create(row);
        lv_obj_set_flex_grow(l, 1);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(l, UI_ACCENT_CYAN, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        lv_label_set_text(l, sig->mf[0] ? sig->mf : sig->serial);

        l = lv_label_create(row);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(l, UI_ACCENT_GREEN, 0);
        lv_label_set_text(l, LV_SYMBOL_PLAY);
    }
}

static void close_tx_popup(void)
{
    if (s_tx_popup) {
        lv_obj_delete(s_tx_popup);
        s_tx_popup = NULL;
    }
}

static void on_tx_confirm(lv_event_t *e)
{
    (void)e;
    int idx = s_pending_tx_idx;
    close_tx_popup();

    char cmd[32];
    snprintf(cmd, sizeof(cmd), "subghz_tx %d", idx);
    uart_send_command(cmd);

    if (s_status_lbl) {
        lv_label_set_text_fmt(s_status_lbl, "Transmitted signal #%d", idx);
        lv_obj_set_style_text_color(s_status_lbl, UI_ACCENT_GREEN, 0);
    }
    ESP_LOGI(TAG, "Transmit signal idx=%d", idx);
}

static void on_tx_cancel(lv_event_t *e)
{
    (void)e;
    close_tx_popup();
}

static tx_signal_t *find_sig_by_idx(int idx)
{
    for (int i = 0; i < s_sig_count; i++) {
        if (s_sigs[i].idx == idx) return &s_sigs[i];
    }
    return NULL;
}

static void on_signal_tap(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_tx_popup) close_tx_popup();

    s_pending_tx_idx = idx;
    tx_signal_t *sig = find_sig_by_idx(idx);

    s_tx_popup = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_tx_popup, 220, 120);
    lv_obj_center(s_tx_popup);
    style_popup_card(s_tx_popup, 10, UI_ACCENT_GREEN);
    lv_obj_set_flex_flow(s_tx_popup, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_tx_popup, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(s_tx_popup, 10, 0);
    lv_obj_set_style_pad_gap(s_tx_popup, 8, 0);
    lv_obj_clear_flag(s_tx_popup, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *info = lv_label_create(s_tx_popup);
    if (sig)
        lv_label_set_text_fmt(info, "#%d  %s  %d.%02d MHz\n%s  %s",
                              sig->idx, sig->type,
                              (int)sig->freq, ((int)(sig->freq * 100.0f + 0.5f)) % 100,
                              sig->mf[0] ? sig->mf : "--", sig->serial);
    else
        lv_label_set_text_fmt(info, "Signal #%d", idx);
    lv_obj_set_style_text_color(info, ui_text_color(), 0);
    lv_obj_set_style_text_font(info, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *btn_row = lv_obj_create(s_tx_popup);
    lv_obj_set_size(btn_row, LV_PCT(100), 32);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *tx_btn = lv_btn_create(btn_row);
    lv_obj_set_size(tx_btn, 90, 28);
    lv_obj_set_style_bg_color(tx_btn, UI_ACCENT_GREEN, 0);
    lv_obj_set_style_radius(tx_btn, 6, 0);
    lv_obj_add_event_cb(tx_btn, on_tx_confirm, LV_EVENT_CLICKED, NULL);
    lv_obj_t *tl = lv_label_create(tx_btn);
    lv_label_set_text(tl, LV_SYMBOL_PLAY " Transmit");
    lv_obj_set_style_text_color(tl, lv_color_white(), 0);
    lv_obj_set_style_text_font(tl, &lv_font_montserrat_12, 0);
    lv_obj_center(tl);

    lv_obj_t *cancel_btn = lv_btn_create(btn_row);
    lv_obj_set_size(cancel_btn, 80, 28);
    lv_obj_set_style_bg_color(cancel_btn, ui_muted_color(), 0);
    lv_obj_set_style_radius(cancel_btn, 6, 0);
    lv_obj_add_event_cb(cancel_btn, on_tx_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(cancel_btn);
    lv_label_set_text(cl, "Cancel");
    lv_obj_set_style_text_color(cl, lv_color_white(), 0);
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_12, 0);
    lv_obj_center(cl);
}

static void on_back(lv_event_t *e)
{
    (void)e;
    close_tx_popup();
    uart_stop_collect();
    if (s_kb_timer) { lv_timer_delete(s_kb_timer); s_kb_timer = NULL; }
    show_subghz_screen();
}

static void kb_poll_cb(lv_timer_t *t)
{
    (void)t;
    uint8_t key = cardkb_read_key();
    if (key == 0) return;
    if (key == 0x1B || key == 0x08 || key == 0x7F)
        on_back(NULL);
}

void show_subghz_transmit_screen(void)
{
    s_sig_count = 0;
    s_list      = NULL;
    s_status_lbl = NULL;
    s_tx_popup  = NULL;
    s_kb_timer  = NULL;

    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Transmit", on_back, NULL);

    s_status_lbl = lv_label_create(scr);
    lv_obj_set_y(s_status_lbl, 38);
    lv_obj_set_x(s_status_lbl, 8);
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_status_lbl, ui_muted_color(), 0);
    lv_label_set_text(s_status_lbl, "Loading signals...");

    s_list = lv_obj_create(scr);
    lv_obj_set_size(s_list, LV_PCT(100), 240 - 54);
    lv_obj_set_y(s_list, 54);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_list, 4, 0);
    lv_obj_set_style_pad_gap(s_list, 3, 0);
    lv_obj_set_style_bg_color(s_list, ui_bg_color(), 0);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_AUTO);

    uart_send_command("subghz_list");
    uart_start_collect("[SUBGHZ_LIST_END]", on_list_received);

    s_kb_timer = lv_timer_create(kb_poll_cb, 50, NULL);

    ESP_LOGI(TAG, "SubGHz Transmit screen ready");
}
