#include "subghz_manage_screen.h"
#include "subghz_screen.h"
#include "subghz_parser.h"
#include "ui_helpers.h"
#include "uart_handler.h"
#include "cardkb.h"
#include "esp_log.h"
#include "bsp/m5stack_core_s3.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "subghz_manage";

#define MAX_SIGNALS 64

typedef struct {
    int   idx;
    char  type[32];
    float freq;
    char  serial[32];
    int   btn;
    int   cnt;
    char  mf[32];
} mgmt_signal_t;

static mgmt_signal_t s_sigs[MAX_SIGNALS];
static int           s_sig_count;
static lv_obj_t     *s_list;
static lv_obj_t     *s_status_lbl;
static lv_obj_t     *s_confirm_popup;
static lv_timer_t   *s_kb_timer;

static void on_back(lv_event_t *e);
static void build_list(void);
static void kb_poll_cb(lv_timer_t *t);
static void fill_signal(mgmt_signal_t *dst, const subghz_signal_info_t *src);

static void fill_signal(mgmt_signal_t *dst, const subghz_signal_info_t *src)
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
    for (int i = 0; i < count && s_sig_count < MAX_SIGNALS; i++) {
        subghz_signal_info_t parsed;

        if (!subghz_parse_signal_line(lines[i], &parsed) || parsed.kind != SUBGHZ_SIGNAL_KIND_LIST)
            continue;

        mgmt_signal_t *s = &s_sigs[s_sig_count];
        fill_signal(s, &parsed);
        if (s->idx > 0) {
            s_sig_count++;
        }
    }

    bsp_display_lock(0);
    if (s_status_lbl) {
        lv_label_set_text_fmt(s_status_lbl, "%d signals stored", s_sig_count);
        lv_obj_set_style_text_color(s_status_lbl, ui_muted_color(), 0);
    }
    build_list();
    bsp_display_unlock();
}

static void on_delete_tap(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "subghz_delete %d", idx);
    uart_send_command(cmd);

    if (s_status_lbl) {
        lv_label_set_text_fmt(s_status_lbl, "Deleted signal #%d", idx);
        lv_obj_set_style_text_color(s_status_lbl, UI_ACCENT_RED, 0);
    }

    ESP_LOGI(TAG, "Delete signal idx=%d", idx);

    /* Refresh list after short delay */
    uart_send_command("subghz_list");
    uart_start_collect("[SUBGHZ_LIST_END]", on_list_received);
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
        mgmt_signal_t *sig = &s_sigs[i];

        lv_obj_t *row = lv_obj_create(s_list);
        lv_obj_set_size(row, LV_PCT(100), 30);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(row, 3, 0);
        lv_obj_set_style_pad_gap(row, 4, 0);
        lv_obj_set_style_bg_color(row, ui_card_color(), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 5, 0);

        lv_obj_t *l;

        l = lv_label_create(row);
        lv_obj_set_width(l, 22);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(l, UI_ACCENT_ORANGE, 0);
        lv_label_set_text_fmt(l, "%d", sig->idx);

        l = lv_label_create(row);
        lv_obj_set_width(l, 50);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(l, ui_text_color(), 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        lv_label_set_text(l, sig->type);

        l = lv_label_create(row);
        lv_obj_set_width(l, 45);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(l, ui_muted_color(), 0);
        lv_label_set_text_fmt(l, "%d.%d", (int)sig->freq, ((int)(sig->freq * 10.0f + 0.5f)) % 10);

        l = lv_label_create(row);
        lv_obj_set_flex_grow(l, 1);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(l, ui_text_color(), 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        lv_label_set_text(l, sig->mf[0] ? sig->mf : sig->serial);

        lv_obj_t *del_btn = lv_btn_create(row);
        lv_obj_set_size(del_btn, 24, 22);
        lv_obj_set_style_bg_color(del_btn, UI_ACCENT_RED, 0);
        lv_obj_set_style_radius(del_btn, 4, 0);
        lv_obj_add_event_cb(del_btn, on_delete_tap, LV_EVENT_CLICKED,
                            (void *)(intptr_t)sig->idx);

        l = lv_label_create(del_btn);
        lv_label_set_text(l, LV_SYMBOL_TRASH);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_10, 0);
        lv_obj_center(l);
    }
}

static void close_confirm_popup(void)
{
    if (s_confirm_popup) {
        lv_obj_delete(s_confirm_popup);
        s_confirm_popup = NULL;
    }
}

static void on_clear_confirmed(lv_event_t *e)
{
    (void)e;
    close_confirm_popup();
    uart_send_command("subghz_clear");

    if (s_status_lbl) {
        lv_label_set_text(s_status_lbl, "All signals cleared");
        lv_obj_set_style_text_color(s_status_lbl, UI_ACCENT_RED, 0);
    }

    s_sig_count = 0;
    build_list();
    ESP_LOGI(TAG, "Clear all signals");
}

static void on_clear_cancel(lv_event_t *e)
{
    (void)e;
    close_confirm_popup();
}

static void on_clear_all(lv_event_t *e)
{
    (void)e;
    if (s_confirm_popup) return;

    s_confirm_popup = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_confirm_popup, 220, 100);
    lv_obj_center(s_confirm_popup);
    style_popup_card(s_confirm_popup, 10, UI_ACCENT_RED);
    lv_obj_set_flex_flow(s_confirm_popup, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_confirm_popup, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(s_confirm_popup, 10, 0);
    lv_obj_set_style_pad_gap(s_confirm_popup, 8, 0);
    lv_obj_clear_flag(s_confirm_popup, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *l = lv_label_create(s_confirm_popup);
    lv_label_set_text(l, "Delete ALL signals?");
    lv_obj_set_style_text_color(l, ui_text_color(), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);

    lv_obj_t *brow = lv_obj_create(s_confirm_popup);
    lv_obj_set_size(brow, LV_PCT(100), 32);
    lv_obj_set_flex_flow(brow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(brow, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(brow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(brow, 0, 0);

    lv_obj_t *yes = lv_btn_create(brow);
    lv_obj_set_size(yes, 80, 28);
    lv_obj_set_style_bg_color(yes, UI_ACCENT_RED, 0);
    lv_obj_set_style_radius(yes, 6, 0);
    lv_obj_add_event_cb(yes, on_clear_confirmed, LV_EVENT_CLICKED, NULL);
    l = lv_label_create(yes);
    lv_label_set_text(l, "Yes");
    lv_obj_center(l);

    lv_obj_t *no = lv_btn_create(brow);
    lv_obj_set_size(no, 80, 28);
    lv_obj_set_style_bg_color(no, ui_card_color(), 0);
    lv_obj_set_style_radius(no, 6, 0);
    lv_obj_add_event_cb(no, on_clear_cancel, LV_EVENT_CLICKED, NULL);
    l = lv_label_create(no);
    lv_label_set_text(l, "Cancel");
    lv_obj_set_style_text_color(l, ui_text_color(), 0);
    lv_obj_center(l);
}

static void on_export_all(lv_event_t *e)
{
    (void)e;
    uart_send_command("subghz_export all");
    if (s_status_lbl) {
        lv_label_set_text(s_status_lbl, "Export sent");
        lv_obj_set_style_text_color(s_status_lbl, UI_ACCENT_GREEN, 0);
    }
    ESP_LOGI(TAG, "Export all");
}

static void on_import_done(const char **lines, int count)
{
    int imported = 0;
    for (int i = 0; i < count; i++) {
        if (strstr(lines[i], "[SUBGHZ_IMPORT] "))
            imported++;
    }

    bsp_display_lock(0);
    if (s_status_lbl) {
        lv_label_set_text_fmt(s_status_lbl, "Imported %d signal%s",
                              imported, imported == 1 ? "" : "s");
        lv_obj_set_style_text_color(s_status_lbl, UI_ACCENT_GREEN, 0);
    }
    bsp_display_unlock();

    uart_send_command("subghz_list");
    uart_start_collect("[SUBGHZ_LIST_END]", on_list_received);
}

static void on_import(lv_event_t *e)
{
    (void)e;
    uart_send_command("subghz_import all");
    uart_start_collect("[SUBGHZ_IMPORT_END]", on_import_done);
    if (s_status_lbl) {
        lv_label_set_text(s_status_lbl, "Importing from SD...");
        lv_obj_set_style_text_color(s_status_lbl, UI_ACCENT_BLUE, 0);
    }
    ESP_LOGI(TAG, "Import all from SD");
}

static void on_back(lv_event_t *e)
{
    (void)e;
    uart_stop_collect();
    close_confirm_popup();
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

void show_subghz_manage_screen(void)
{
    s_sig_count     = 0;
    s_list          = NULL;
    s_status_lbl    = NULL;
    s_confirm_popup = NULL;
    s_kb_timer      = NULL;

    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Manage Signals", on_back, NULL);

    /* Action bar */
    lv_obj_t *abar = lv_obj_create(scr);
    lv_obj_set_size(abar, LV_PCT(100), 28);
    lv_obj_set_y(abar, 36);
    lv_obj_set_flex_flow(abar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(abar, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(abar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(abar, 0, 0);
    lv_obj_set_style_pad_all(abar, 2, 0);

    lv_obj_t *btn, *lbl;

    btn = lv_btn_create(abar);
    lv_obj_set_size(btn, 70, 22);
    lv_obj_set_style_bg_color(btn, UI_ACCENT_GREEN, 0);
    lv_obj_set_style_radius(btn, 5, 0);
    lv_obj_add_event_cb(btn, on_export_all, LV_EVENT_CLICKED, NULL);
    lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "Export");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
    lv_obj_center(lbl);

    btn = lv_btn_create(abar);
    lv_obj_set_size(btn, 70, 22);
    lv_obj_set_style_bg_color(btn, UI_ACCENT_BLUE, 0);
    lv_obj_set_style_radius(btn, 5, 0);
    lv_obj_add_event_cb(btn, on_import, LV_EVENT_CLICKED, NULL);
    lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "Import");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
    lv_obj_center(lbl);

    btn = lv_btn_create(abar);
    lv_obj_set_size(btn, 70, 22);
    lv_obj_set_style_bg_color(btn, UI_ACCENT_RED, 0);
    lv_obj_set_style_radius(btn, 5, 0);
    lv_obj_add_event_cb(btn, on_clear_all, LV_EVENT_CLICKED, NULL);
    lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "Clear All");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
    lv_obj_center(lbl);

    s_status_lbl = lv_label_create(scr);
    lv_obj_set_y(s_status_lbl, 66);
    lv_obj_set_x(s_status_lbl, 8);
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_status_lbl, ui_muted_color(), 0);
    lv_label_set_text(s_status_lbl, "Loading...");

    s_list = lv_obj_create(scr);
    lv_obj_set_size(s_list, LV_PCT(100), 240 - 82);
    lv_obj_set_y(s_list, 82);
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

    ESP_LOGI(TAG, "SubGHz Manage screen ready");
}
