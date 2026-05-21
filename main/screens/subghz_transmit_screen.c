#include "subghz_transmit_screen.h"
#include "subghz_screen.h"
#include "subghz_parser.h"
#include "ui_helpers.h"
#include "uart_handler.h"
#include "cardkb.h"
#include "led_indicator.h"
#include "psram_dynarr.h"
#include "esp_log.h"
#include "bsp/m5stack_core_s3.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "subghz_tx";

#define SUBGHZ_TX_SIG_HARD_CAP 2048
#define TX_ROW_HEIGHT          32
#define TX_ROW_POOL_SIZE       8

typedef struct {
    int   idx;
    char  type[32];
    float freq;
    char  serial[32];
    int   btn;
    int   cnt;
    char  mf[32];
    char  name[64];
} tx_signal_t;

typedef struct {
    lv_obj_t *row;
    lv_obj_t *idx;
    lv_obj_t *type;
    lv_obj_t *freq;
    lv_obj_t *info;
    lv_obj_t *play;
    int       sig_idx;
} tx_row_view_t;

static tx_signal_t *s_sigs;
static int          s_sig_cap;
static int          s_sig_count;
static lv_obj_t    *s_list;
static lv_obj_t    *s_list_spacer;
static lv_obj_t    *s_empty_lbl;
static lv_obj_t    *s_status_lbl;
static lv_obj_t    *s_tx_popup;
static int          s_pending_tx_idx;
static lv_timer_t  *s_kb_timer;
static tx_row_view_t s_row_pool[TX_ROW_POOL_SIZE];

static void on_back(lv_event_t *e);
static void kb_poll_cb(lv_timer_t *t);

static void on_signal_tap(lv_event_t *e);
static void on_tx_list_scroll(lv_event_t *e);
static void refresh_tx_list_view(void);
static void configure_tx_row(tx_row_view_t *view);
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
    snprintf(dst->name, sizeof(dst->name), "%s", src->name);
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
    if (s_status_lbl) {
        if (s_sig_count == 0) {
            lv_label_set_text(s_status_lbl, "No stored signals");
            lv_obj_set_style_text_color(s_status_lbl, ui_muted_color(), 0);
        } else {
            lv_label_set_text_fmt(s_status_lbl, "%d signal%s", s_sig_count,
                                  s_sig_count == 1 ? "" : "s");
            lv_obj_set_style_text_color(s_status_lbl, UI_ACCENT_CYAN, 0);
        }
    }
    if (s_list_spacer)
        lv_obj_set_height(s_list_spacer, (lv_coord_t)(s_sig_count * TX_ROW_HEIGHT));
    if (s_list)
        lv_obj_scroll_to_y(s_list, 0, LV_ANIM_OFF);
    refresh_tx_list_view();
    bsp_display_unlock();
}

static void configure_tx_row(tx_row_view_t *view)
{
    if (!view || !s_list)
        return;

    view->row = lv_obj_create(s_list);
    lv_obj_set_size(view->row, LV_PCT(100), TX_ROW_HEIGHT - 3);
    lv_obj_set_flex_flow(view->row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(view->row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(view->row, 4, 0);
    lv_obj_set_style_pad_gap(view->row, 6, 0);
    lv_obj_set_style_bg_color(view->row, ui_card_color(), 0);
    lv_obj_set_style_bg_color(view->row, ui_card_pressed_color(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(view->row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(view->row, 0, 0);
    lv_obj_set_style_radius(view->row, 5, 0);
    lv_obj_clear_flag(view->row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(view->row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(view->row, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(view->row, on_signal_tap, LV_EVENT_CLICKED, view);

    view->idx = lv_label_create(view->row);
    lv_obj_set_width(view->idx, 22);
    lv_obj_set_style_text_font(view->idx, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(view->idx, UI_ACCENT_GREEN, 0);

    view->type = lv_label_create(view->row);
    lv_obj_set_width(view->type, 60);
    lv_obj_set_style_text_font(view->type, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(view->type, ui_text_color(), 0);
    lv_label_set_long_mode(view->type, LV_LABEL_LONG_CLIP);

    view->freq = lv_label_create(view->row);
    lv_obj_set_width(view->freq, 50);
    lv_obj_set_style_text_font(view->freq, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(view->freq, ui_muted_color(), 0);

    view->info = lv_label_create(view->row);
    lv_obj_set_flex_grow(view->info, 1);
    lv_obj_set_style_text_font(view->info, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(view->info, UI_ACCENT_CYAN, 0);
    lv_label_set_long_mode(view->info, LV_LABEL_LONG_CLIP);

    view->play = lv_label_create(view->row);
    lv_obj_set_style_text_font(view->play, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(view->play, UI_ACCENT_GREEN, 0);
    lv_label_set_text(view->play, LV_SYMBOL_PLAY);

    view->sig_idx = -1;
}

static void refresh_tx_list_view(void)
{
    if (!s_list || !s_list_spacer || !s_empty_lbl)
        return;

    if (s_sig_count == 0) {
        lv_obj_clear_flag(s_empty_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(s_list_spacer, 1);
        for (int i = 0; i < TX_ROW_POOL_SIZE; i++) {
            if (s_row_pool[i].row)
                lv_obj_add_flag(s_row_pool[i].row, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    lv_obj_add_flag(s_empty_lbl, LV_OBJ_FLAG_HIDDEN);

    lv_coord_t scroll_y = lv_obj_get_scroll_y(s_list);
    if (scroll_y < 0)
        scroll_y = 0;

    int first_index = (int)(scroll_y / TX_ROW_HEIGHT);
    if (first_index < 0)
        first_index = 0;
    if (first_index >= s_sig_count)
        first_index = s_sig_count - 1;

    for (int i = 0; i < TX_ROW_POOL_SIZE; i++) {
        tx_row_view_t *view = &s_row_pool[i];

        if (!view->row)
            continue;

        int sig_index = first_index + i;
        if (sig_index >= s_sig_count) {
            lv_obj_add_flag(view->row, LV_OBJ_FLAG_HIDDEN);
            view->sig_idx = -1;
            continue;
        }

        const tx_signal_t *sig = &s_sigs[sig_index];

        lv_obj_set_pos(view->row, 0, (lv_coord_t)(sig_index * TX_ROW_HEIGHT));
        lv_label_set_text_fmt(view->idx, "%d", sig->idx);
        lv_label_set_text(view->type, sig->type);
        lv_label_set_text_fmt(view->freq, "%d.%02d",
                              (int)sig->freq, ((int)(sig->freq * 100.0f + 0.5f)) % 100);
        /* Prefer the editable `name` (firmware emits it in [SUBGHZ_LIST]),
         * fall back to mf or serial for older firmware. */
        const char *info_text = sig->name[0]
            ? sig->name
            : (sig->mf[0] && strcmp(sig->mf, "--") != 0 ? sig->mf : sig->serial);
        lv_label_set_text(view->info, info_text);
        view->sig_idx = sig->idx;
        lv_obj_clear_flag(view->row, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_tx_list_scroll(lv_event_t *e)
{
    (void)e;
    refresh_tx_list_view();
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
    led_indicator_tx_pulse(1500);

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
    tx_row_view_t *view = (tx_row_view_t *)lv_event_get_user_data(e);
    if (!view || view->sig_idx < 0)
        return;

    int idx = view->sig_idx;
    if (s_tx_popup) close_tx_popup();

    s_pending_tx_idx = idx;
    tx_signal_t *sig = find_sig_by_idx(idx);

    s_tx_popup = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_tx_popup, 280, 200);
    lv_obj_center(s_tx_popup);
    style_popup_card(s_tx_popup, 10, UI_ACCENT_GREEN);
    lv_obj_set_flex_flow(s_tx_popup, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_tx_popup, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(s_tx_popup, 12, 0);
    lv_obj_set_style_pad_gap(s_tx_popup, 10, 0);
    lv_obj_clear_flag(s_tx_popup, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *info = lv_label_create(s_tx_popup);
    if (sig)
        lv_label_set_text_fmt(info, "#%d  %s  %d.%02d MHz\nname: %s\n%s  %s",
                              sig->idx, sig->type,
                              (int)sig->freq, ((int)(sig->freq * 100.0f + 0.5f)) % 100,
                              sig->name[0] ? sig->name : "(unset)",
                              sig->mf[0] ? sig->mf : "--", sig->serial);
    else
        lv_label_set_text_fmt(info, "Signal #%d", idx);
    lv_obj_set_style_text_color(info, ui_text_color(), 0);
    lv_obj_set_style_text_font(info, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *btn_row = lv_obj_create(s_tx_popup);
    lv_obj_set_size(btn_row, LV_PCT(100), 64);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *tx_btn = lv_btn_create(btn_row);
    lv_obj_set_size(tx_btn, 130, 56);
    lv_obj_set_style_bg_color(tx_btn, UI_ACCENT_GREEN, 0);
    lv_obj_set_style_radius(tx_btn, 8, 0);
    lv_obj_add_event_cb(tx_btn, on_tx_confirm, LV_EVENT_CLICKED, NULL);
    lv_obj_t *tl = lv_label_create(tx_btn);
    lv_label_set_text(tl, LV_SYMBOL_PLAY " Transmit");
    lv_obj_set_style_text_color(tl, lv_color_white(), 0);
    lv_obj_set_style_text_font(tl, &lv_font_montserrat_16, 0);
    lv_obj_center(tl);

    lv_obj_t *cancel_btn = lv_btn_create(btn_row);
    lv_obj_set_size(cancel_btn, 90, 40);
    lv_obj_set_style_bg_color(cancel_btn, ui_muted_color(), 0);
    lv_obj_set_style_radius(cancel_btn, 8, 0);
    lv_obj_add_event_cb(cancel_btn, on_tx_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(cancel_btn);
    lv_label_set_text(cl, "Cancel");
    lv_obj_set_style_text_color(cl, lv_color_white(), 0);
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_14, 0);
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
    s_list_spacer = NULL;
    s_empty_lbl = NULL;
    s_status_lbl = NULL;
    s_tx_popup  = NULL;
    s_kb_timer  = NULL;
    memset(s_row_pool, 0, sizeof(s_row_pool));

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
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_style_bg_color(s_list, ui_bg_color(), 0);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_event_cb(s_list, on_tx_list_scroll, LV_EVENT_SCROLL, NULL);

    s_list_spacer = lv_obj_create(s_list);
    lv_obj_set_pos(s_list_spacer, 0, 0);
    lv_obj_set_size(s_list_spacer, 1, 1);
    lv_obj_set_style_bg_opa(s_list_spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list_spacer, 0, 0);
    lv_obj_clear_flag(s_list_spacer, LV_OBJ_FLAG_SCROLLABLE);

    s_empty_lbl = lv_label_create(s_list);
    lv_obj_set_pos(s_empty_lbl, 8, 6);
    lv_obj_set_style_text_color(s_empty_lbl, ui_muted_color(), 0);
    lv_obj_set_style_text_font(s_empty_lbl, &lv_font_montserrat_12, 0);
    lv_label_set_text(s_empty_lbl, "No stored signals");

    for (int i = 0; i < TX_ROW_POOL_SIZE; i++)
        configure_tx_row(&s_row_pool[i]);

    refresh_tx_list_view();

    uart_send_command("subghz_list");
    uart_start_collect("[SUBGHZ_LIST_END]", on_list_received);

    s_kb_timer = lv_timer_create(kb_poll_cb, 50, NULL);

    ESP_LOGI(TAG, "SubGHz Transmit screen ready");
}
