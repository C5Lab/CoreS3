#include "nfc_list_screen.h"
#include "nfc_screen.h"
#include "nfc_emulate_screen.h"
#include "nfc_parser.h"
#include "ui_helpers.h"
#include "uart_handler.h"
#include "cardkb.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
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
static lv_obj_t  *s_action_popup;
static lv_obj_t  *s_confirm_popup;
static lv_timer_t *s_kb_timer;
static int        s_action_target_idx = -1;
static int        s_pending_delete_idx = -1;
static bool       s_text_input_open;

static void on_back(lv_event_t *e);
static void kb_poll_cb(lv_timer_t *t);
static void on_list_received(const char **lines, int count);
static void rebuild_list_async(void *unused);
static void build_list(void);
static void on_row_tap(lv_event_t *e);
static void show_action_popup(int idx);
static void close_action_popup(void);
static void close_confirm_popup(void);
static void on_action_emulate(lv_event_t *e);
static void on_action_rename(lv_event_t *e);
static void on_action_delete(lv_event_t *e);
static void on_action_cancel(lv_event_t *e);
static void show_delete_confirm_popup(int idx);
static void on_delete_confirmed(lv_event_t *e);
static void on_delete_cancel(lv_event_t *e);
static void do_delete(int idx);
static void open_rename_popup(int idx);
static void on_rename_confirm(const char *text, void *user_data);
static void on_rename_cancel(void *user_data);
static void request_list(void);
static const nfc_row_t *find_row(int idx);

static const nfc_row_t *find_row(int idx)
{
    for (int i = 0; i < s_row_count; i++) {
        if (s_rows[i].idx == idx) return &s_rows[i];
    }
    return NULL;
}

static void request_list(void)
{
    if (s_status_lbl) {
        lv_label_set_text(s_status_lbl, "Loading...");
        lv_obj_set_style_text_color(s_status_lbl, ui_muted_color(), 0);
    }
    uart_send_command("nfc_list");
    uart_start_collect("[NFC] END", on_list_received);
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

        if (strstr(line, "[NFC] renamed: ")) {
            /* feedback after rename — rebuild will refresh status */
        }
        if (strstr(line, "[NFC] deleted: ")) {
            /* ok */
        }
        if (strstr(line, "[NFC] rename failed") ||
            strstr(line, "[NFC] delete failed") ||
            strstr(line, "[NFC] not found")) {
            ESP_LOGW(TAG, "list/mutation: %s", line);
        }
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

static void close_action_popup(void)
{
    if (s_action_popup) {
        lv_obj_delete(s_action_popup);
        s_action_popup = NULL;
    }
}

static void close_confirm_popup(void)
{
    if (s_confirm_popup) {
        lv_obj_delete(s_confirm_popup);
        s_confirm_popup = NULL;
    }
}

static void on_action_emulate(lv_event_t *e)
{
    (void)e;
    int idx = s_action_target_idx;
    close_action_popup();
    if (idx < 0) return;
    uart_stop_collect();
    if (s_kb_timer) { lv_timer_delete(s_kb_timer); s_kb_timer = NULL; }
    s_list = NULL;
    s_status_lbl = NULL;
    show_nfc_emulate_screen(idx);
}

static void request_list_async(void *unused)
{
    (void)unused;
    request_list();
}

static void mutation_then_list_cb(const char **lines, int count)
{
    for (int i = 0; i < count; i++) {
        if (!lines[i]) continue;
        if (strstr(lines[i], "[NFC] rename failed") ||
            strstr(lines[i], "[NFC] delete failed") ||
            strstr(lines[i], "[NFC] not found")) {
            ESP_LOGW(TAG, "%s", lines[i]);
        }
    }
    /* Defer next collect — must not nest uart_start_collect inside collect_cb. */
    if (!ui_lvgl_async_call(request_list_async, NULL))
        ESP_LOGW(TAG, "request_list_async schedule failed");
}

static void on_rename_confirm(const char *text, void *user_data)
{
    s_text_input_open = false;
    int idx = (int)(intptr_t)user_data;
    if (idx < 0 || !text) return;

    if (!text[0] || !nfc_name_is_valid(text)) {
        if (s_status_lbl) {
            lv_label_set_text(s_status_lbl, "Rename: invalid name");
            lv_obj_set_style_text_color(s_status_lbl, UI_ACCENT_RED, 0);
        }
        return;
    }

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "nfc_rename %d %s", idx, text);
    uart_send_command(cmd);

    if (s_status_lbl) {
        lv_label_set_text_fmt(s_status_lbl, "Renaming #%d...", idx);
        lv_obj_set_style_text_color(s_status_lbl, UI_ACCENT_BLUE, 0);
    }

    uart_start_collect("[NFC] END", mutation_then_list_cb);
}

static void on_rename_cancel(void *user_data)
{
    (void)user_data;
    s_text_input_open = false;
}

static void open_rename_popup(int idx)
{
    const nfc_row_t *row = find_row(idx);
    char initial[64] = {0};
    if (row) {
        snprintf(initial, sizeof(initial), "%s", row->name);
        /* strip .nfc for editing convenience */
        size_t n = strlen(initial);
        if (n > 4 && strcmp(initial + n - 4, ".nfc") == 0)
            initial[n - 4] = '\0';
    }

    s_text_input_open = true;
    ui_show_text_input_popup("Rename card",
                             initial,
                             48,
                             UI_ACCENT_BLUE,
                             on_rename_confirm,
                             on_rename_cancel,
                             (void *)(intptr_t)idx);
}

static void on_action_rename(lv_event_t *e)
{
    (void)e;
    int idx = s_action_target_idx;
    close_action_popup();
    if (idx < 0) return;
    open_rename_popup(idx);
}

static void do_delete(int idx)
{
    if (idx < 0) return;
    char cmd[48];
    snprintf(cmd, sizeof(cmd), "nfc_delete %d", idx);
    uart_send_command(cmd);

    if (s_status_lbl) {
        lv_label_set_text_fmt(s_status_lbl, "Deleted #%d", idx);
        lv_obj_set_style_text_color(s_status_lbl, UI_ACCENT_RED, 0);
    }

    uart_start_collect("[NFC] END", mutation_then_list_cb);
}

static void on_delete_confirmed(lv_event_t *e)
{
    (void)e;
    int idx = s_pending_delete_idx;
    close_confirm_popup();
    s_pending_delete_idx = -1;
    do_delete(idx);
}

static void on_delete_cancel(lv_event_t *e)
{
    (void)e;
    close_confirm_popup();
    s_pending_delete_idx = -1;
}

static void show_delete_confirm_popup(int idx)
{
    close_confirm_popup();
    s_pending_delete_idx = idx;

    const nfc_row_t *row = find_row(idx);

    s_confirm_popup = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_confirm_popup, 280, 120);
    lv_obj_center(s_confirm_popup);
    style_popup_card(s_confirm_popup, 10, UI_ACCENT_RED);
    lv_obj_set_flex_flow(s_confirm_popup, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_confirm_popup, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(s_confirm_popup, 10, 0);
    lv_obj_set_style_pad_gap(s_confirm_popup, 8, 0);
    lv_obj_clear_flag(s_confirm_popup, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_confirm_popup);
    if (row)
        lv_label_set_text_fmt(title, "Delete %s?", row->name);
    else
        lv_label_set_text_fmt(title, "Delete #%d?", idx);
    lv_obj_set_style_text_color(title, ui_text_color(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);

    lv_obj_t *brow = lv_obj_create(s_confirm_popup);
    lv_obj_set_size(brow, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(brow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(brow, 0, 0);
    lv_obj_set_style_pad_all(brow, 0, 0);
    lv_obj_set_style_pad_gap(brow, 8, 0);
    lv_obj_set_flex_flow(brow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(brow, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(brow, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *yb = lv_btn_create(brow);
    lv_obj_set_size(yb, 90, 30);
    lv_obj_set_style_bg_color(yb, UI_ACCENT_RED, 0);
    lv_obj_set_style_radius(yb, 6, 0);
    lv_obj_add_event_cb(yb, on_delete_confirmed, LV_EVENT_CLICKED, NULL);
    lv_obj_t *yl = lv_label_create(yb);
    lv_label_set_text(yl, "Delete");
    lv_obj_set_style_text_color(yl, lv_color_white(), 0);
    lv_obj_center(yl);

    lv_obj_t *nb = lv_btn_create(brow);
    lv_obj_set_size(nb, 90, 30);
    lv_obj_set_style_bg_color(nb, ui_muted_color(), 0);
    lv_obj_set_style_radius(nb, 6, 0);
    lv_obj_add_event_cb(nb, on_delete_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t *nl = lv_label_create(nb);
    lv_label_set_text(nl, "Cancel");
    lv_obj_set_style_text_color(nl, lv_color_white(), 0);
    lv_obj_center(nl);
}

static void on_action_delete(lv_event_t *e)
{
    (void)e;
    int idx = s_action_target_idx;
    close_action_popup();
    if (idx < 0) return;
    show_delete_confirm_popup(idx);
}

static void on_action_cancel(lv_event_t *e)
{
    (void)e;
    close_action_popup();
}

static void show_action_popup(int idx)
{
    if (idx < 0) return;
    close_action_popup();

    const nfc_row_t *row = find_row(idx);
    s_action_target_idx = idx;

    s_action_popup = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_action_popup, 310, 160);
    lv_obj_center(s_action_popup);
    style_popup_card(s_action_popup, 10, UI_ACCENT_BLUE);
    lv_obj_set_flex_flow(s_action_popup, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_action_popup, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(s_action_popup, 10, 0);
    lv_obj_set_style_pad_gap(s_action_popup, 8, 0);
    lv_obj_clear_flag(s_action_popup, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_action_popup);
    lv_label_set_text_fmt(title, "Card #%d", idx);
    lv_obj_set_style_text_color(title, ui_text_color(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);

    lv_obj_t *name_lbl = lv_label_create(s_action_popup);
    lv_obj_set_width(name_lbl, 290);
    lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
    lv_label_set_text_fmt(name_lbl, "%s",
                          (row && row->name[0]) ? row->name : "(unnamed)");
    lv_obj_set_style_text_color(name_lbl, ui_muted_color(), 0);
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(name_lbl, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *brow = lv_obj_create(s_action_popup);
    lv_obj_set_size(brow, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(brow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(brow, 0, 0);
    lv_obj_set_style_pad_all(brow, 0, 0);
    lv_obj_set_style_pad_gap(brow, 4, 0);
    lv_obj_set_flex_flow(brow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(brow, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(brow, LV_OBJ_FLAG_SCROLLABLE);

    struct {
        const char   *label;
        lv_color_t    bg;
        lv_event_cb_t cb;
    } btns[] = {
        { "Emulate", UI_ACCENT_ORANGE, on_action_emulate },
        { "Rename",  UI_ACCENT_BLUE,   on_action_rename  },
        { "Delete",  UI_ACCENT_RED,    on_action_delete  },
        { "Cancel",  ui_muted_color(), on_action_cancel  },
    };
    for (int i = 0; i < (int)(sizeof(btns) / sizeof(btns[0])); i++) {
        lv_obj_t *b = lv_btn_create(brow);
        lv_obj_set_size(b, 68, 30);
        lv_obj_set_style_bg_color(b, btns[i].bg, 0);
        lv_obj_set_style_radius(b, 6, 0);
        lv_obj_add_event_cb(b, btns[i].cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, btns[i].label);
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
        lv_obj_center(l);
    }
}

static void on_row_tap(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0) return;
    show_action_popup(idx);
}

static void on_back(lv_event_t *e)
{
    (void)e;
    uart_stop_collect();
    close_confirm_popup();
    close_action_popup();
    if (s_kb_timer) { lv_timer_delete(s_kb_timer); s_kb_timer = NULL; }
    s_pending_delete_idx = -1;
    s_action_target_idx = -1;
    s_list = NULL;
    s_status_lbl = NULL;
    s_text_input_open = false;
    show_nfc_screen();
}

static void kb_poll_cb(lv_timer_t *t)
{
    (void)t;
    if (s_text_input_open || s_confirm_popup || s_action_popup) return;
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
    s_confirm_popup = NULL;
    s_action_popup = NULL;
    s_kb_timer = NULL;
    s_text_input_open = false;
    s_pending_delete_idx = -1;
    s_action_target_idx = -1;

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
