#include "nfc_list_screen.h"
#include "nfc_screen.h"
#include "nfc_detail_screen.h"
#include "nfc_parser.h"
#include "ui_helpers.h"
#include "uart_handler.h"
#include "cardkb.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static const char *TAG = "nfc_list";

#define NFC_LIST_CAP 128

typedef struct {
    int  idx;
    char name[64];
} nfc_row_t;

static nfc_row_t  s_rows[NFC_LIST_CAP];
static int        s_row_count;
static lv_obj_t  *s_list;
static lv_obj_t  *s_status_lbl;
static lv_timer_t *s_kb_timer;

static void on_back(lv_event_t *e);
static void kb_poll_cb(lv_timer_t *t);
static void on_list_received(const char **lines, int count);
static void rebuild_list_async(void *unused);
static void build_list(void);
static void on_row_tap(lv_event_t *e);
static void request_list(void);

static void request_list(void)
{
    if (s_status_lbl) {
        lv_label_set_text(s_status_lbl, "Loading...");
        lv_obj_set_style_text_color(s_status_lbl, ui_muted_color(), 0);
    }
    uart_start_collect("[NFC] END", on_list_received);
    uart_send_command("nfc_list");
}

static void on_list_received(const char **lines, int count)
{
    s_row_count = 0;
    int reported = -1;

    for (int i = 0; i < count; i++) {
        const char *line = lines[i];
        if (!line) continue;

        nfc_list_entry_t ent;
        if (nfc_parse_list_entry(line, &ent)) {
            if (s_row_count < NFC_LIST_CAP) {
                s_rows[s_row_count].idx = ent.idx;
                snprintf(s_rows[s_row_count].name, sizeof(s_rows[0].name),
                         "%s", ent.name);
                s_row_count++;
            }
            continue;
        }

        int n = 0;
        if (nfc_parse_card_count(line, &n))
            reported = n;
    }

    if (reported >= 0 && reported != s_row_count) {
        ESP_LOGI(TAG, "card(s)=%d parsed=%d", reported, s_row_count);
    }

    if (!ui_lvgl_async_call(rebuild_list_async, NULL))
        ESP_LOGW(TAG, "rebuild_list_async schedule failed");
}

static void rebuild_list_async(void *unused)
{
    (void)unused;
    build_list();
    if (s_status_lbl) {
        if (s_row_count == 0) {
            lv_label_set_text(s_status_lbl, "No saved cards");
            lv_obj_set_style_text_color(s_status_lbl, ui_muted_color(), 0);
        } else {
            lv_label_set_text_fmt(s_status_lbl, "%d card(s)", s_row_count);
            lv_obj_set_style_text_color(s_status_lbl, ui_muted_color(), 0);
        }
    }
}

static void build_list(void)
{
    if (!s_list) return;
    lv_obj_clean(s_list);

    if (s_row_count == 0) {
        lv_obj_t *l = lv_label_create(s_list);
        lv_label_set_text(l, "No saved cards");
        lv_obj_set_style_text_color(l, ui_muted_color(), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
        return;
    }

    for (int i = 0; i < s_row_count; i++) {
        nfc_row_t *row = &s_rows[i];

        lv_obj_t *obj = lv_obj_create(s_list);
        lv_obj_set_size(obj, LV_PCT(100), 28);
        lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(obj, 3, 0);
        lv_obj_set_style_pad_gap(obj, 6, 0);
        lv_obj_set_style_bg_color(obj, ui_card_color(), 0);
        lv_obj_set_style_bg_color(obj, ui_card_pressed_color(), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(obj, 0, 0);
        lv_obj_set_style_radius(obj, 5, 0);
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(obj, on_row_tap, LV_EVENT_CLICKED,
                            (void *)(intptr_t)row->idx);

        lv_obj_t *info = lv_label_create(obj);
        lv_obj_set_flex_grow(info, 1);
        lv_obj_set_style_text_font(info, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(info, ui_text_color(), 0);
        lv_label_set_long_mode(info, LV_LABEL_LONG_DOT);
        lv_label_set_text_fmt(info, "%d  %s", row->idx, row->name);
    }
}

static void on_row_tap(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0) return;
    uart_stop_collect();
    if (s_kb_timer) { lv_timer_delete(s_kb_timer); s_kb_timer = NULL; }
    s_list = NULL;
    s_status_lbl = NULL;
    show_nfc_detail_screen(idx);
}

static void on_back(lv_event_t *e)
{
    (void)e;
    uart_stop_collect();
    if (s_kb_timer) { lv_timer_delete(s_kb_timer); s_kb_timer = NULL; }
    s_list = NULL;
    s_status_lbl = NULL;
    show_nfc_screen();
}

static void kb_poll_cb(lv_timer_t *t)
{
    (void)t;
    uint8_t key = cardkb_read_key();
    if (key == 0) return;
    if (key == 0x1B || key == 0x08 || key == 0x7F)
        on_back(NULL);
}

void show_nfc_list_screen(void)
{
    s_row_count = 0;
    s_list = NULL;
    s_status_lbl = NULL;
    s_kb_timer = NULL;

    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "NFC List", on_back, NULL);

    s_status_lbl = lv_label_create(scr);
    lv_obj_set_y(s_status_lbl, 40);
    lv_obj_set_x(s_status_lbl, 8);
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_status_lbl, ui_muted_color(), 0);
    lv_label_set_text(s_status_lbl, "Loading...");

    s_list = lv_obj_create(scr);
    lv_obj_set_size(s_list, LV_PCT(100), 240 - 56);
    lv_obj_set_y(s_list, 56);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_list, 4, 0);
    lv_obj_set_style_pad_gap(s_list, 3, 0);
    lv_obj_set_style_bg_color(s_list, ui_bg_color(), 0);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_AUTO);

    request_list();
    s_kb_timer = lv_timer_create(kb_poll_cb, 50, NULL);
    ESP_LOGI(TAG, "NFC List screen ready");
}
