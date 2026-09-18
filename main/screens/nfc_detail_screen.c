#include "nfc_detail_screen.h"
#include "nfc_list_screen.h"
#include "nfc_emulate_screen.h"
#include "nfc_parser.h"
#include "ui_helpers.h"
#include "uart_handler.h"
#include "cardkb.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "nfc_detail";

static int          s_idx = -1;
static char         s_name[64];
static nfc_ui_card_t s_card;
static bool         s_loaded;
static bool         s_text_input_open;
static lv_obj_t    *s_status_lbl;
static lv_obj_t    *s_detail_lbl;
static lv_obj_t    *s_emu_btn;
static lv_obj_t    *s_confirm_popup;
static lv_timer_t  *s_kb_timer;

typedef struct {
    char status[64];
    char detail[192];
    lv_color_t color;
    bool enable_emu;
} detail_snap_t;

static detail_snap_t s_snap;

static void on_back(lv_event_t *e);
static void kb_poll_cb(lv_timer_t *t);
static void set_emu_enabled(bool en);
static void apply_snap_async(void *unused);
static void on_load_collected(const char **lines, int count);
static void start_load(void);
static void on_emulate(lv_event_t *e);
static void on_rename(lv_event_t *e);
static void on_delete(lv_event_t *e);
static void on_rename_confirm(const char *text, void *user_data);
static void on_rename_cancel(void *user_data);
static void on_rename_collected(const char **lines, int count);
static void close_confirm_popup(void);
static void show_delete_confirm_popup(void);
static void on_delete_confirmed(lv_event_t *e);
static void on_delete_cancel(lv_event_t *e);
static void on_delete_collected(const char **lines, int count);
static void go_list_async(void *unused);
static void teardown(void);

static void teardown(void)
{
    uart_stop_collect();
    close_confirm_popup();
    if (s_kb_timer) { lv_timer_delete(s_kb_timer); s_kb_timer = NULL; }
    s_status_lbl = NULL;
    s_detail_lbl = NULL;
    s_emu_btn = NULL;
    s_text_input_open = false;
}

static void set_emu_enabled(bool en)
{
    if (!s_emu_btn) return;
    if (en)
        lv_obj_clear_state(s_emu_btn, LV_STATE_DISABLED);
    else
        lv_obj_add_state(s_emu_btn, LV_STATE_DISABLED);
}

static void apply_snap_async(void *unused)
{
    (void)unused;
    if (s_status_lbl) {
        lv_label_set_text(s_status_lbl, s_snap.status);
        lv_obj_set_style_text_color(s_status_lbl, s_snap.color, 0);
    }
    if (s_detail_lbl)
        lv_label_set_text(s_detail_lbl, s_snap.detail);
    set_emu_enabled(s_snap.enable_emu);
}

static void apply_loaded_card(void)
{
    if (s_card.load_failed || s_card.not_detected || s_card.not_initialized) {
        snprintf(s_snap.status, sizeof(s_snap.status), "Load failed");
        s_snap.detail[0] = '\0';
        s_snap.color = UI_ACCENT_RED;
        s_snap.enable_emu = false;
        s_loaded = false;
        return;
    }
    if (!s_card.have_loaded && !s_card.type[0] && !s_card.uid[0]) {
        snprintf(s_snap.status, sizeof(s_snap.status), "Load failed");
        s_snap.detail[0] = '\0';
        s_snap.color = UI_ACCENT_RED;
        s_snap.enable_emu = false;
        s_loaded = false;
        return;
    }

    if (s_card.have_loaded)
        nfc_path_basename(s_card.loaded_path, s_name, sizeof(s_name), true);

    nfc_format_card_detail(&s_card, s_snap.status, sizeof(s_snap.status),
                           s_snap.detail, sizeof(s_snap.detail));
    s_snap.color = UI_ACCENT_GREEN;
    s_snap.enable_emu = true;
    s_loaded = true;
}

static void on_load_collected(const char **lines, int count)
{
    nfc_card_reset(&s_card);
    for (int i = 0; i < count; i++) {
        if (!lines[i]) continue;
        nfc_parse_card_line(lines[i], &s_card);
    }
    apply_loaded_card();
    if (!ui_lvgl_async_call(apply_snap_async, NULL))
        ESP_LOGW(TAG, "load UI schedule failed");
}

static void start_load(void)
{
    char cmd[48];
    nfc_card_reset(&s_card);
    s_loaded = false;
    set_emu_enabled(false);
    snprintf(s_snap.status, sizeof(s_snap.status), "Loading...");
    s_snap.detail[0] = '\0';
    s_snap.color = ui_muted_color();
    s_snap.enable_emu = false;
    apply_snap_async(NULL);

    snprintf(cmd, sizeof(cmd), "nfc_load %d", s_idx);
    uart_start_collect("[NFC] END", on_load_collected);
    uart_send_command(cmd);
}

static void on_emulate(lv_event_t *e)
{
    (void)e;
    if (!s_loaded || s_text_input_open || s_confirm_popup) return;
    int idx = s_idx;
    teardown();
    nfc_emulate_set_card_hint(s_card.have_data, s_card.type);
    show_nfc_emulate_screen(idx);
}

static void on_rename_collected(const char **lines, int count)
{
    bool failed = false;
    char renamed[160] = {0};

    for (int i = 0; i < count; i++) {
        if (!lines[i]) continue;
        if (strstr(lines[i], "[NFC] rename failed") ||
            strstr(lines[i], "[NFC] not found")) {
            failed = true;
        }
        if (strncmp(lines[i], "[NFC] renamed: ", 15) == 0) {
            snprintf(renamed, sizeof(renamed), "%s", lines[i] + 15);
            const char *arrow = strstr(renamed, " -> ");
            if (arrow) {
                nfc_path_basename(arrow + 4, s_name, sizeof(s_name), true);
                snprintf(s_card.loaded_path, sizeof(s_card.loaded_path),
                         "%s", arrow + 4);
                s_card.have_loaded = true;
            }
        }
    }

    if (failed) {
        snprintf(s_snap.status, sizeof(s_snap.status), "Rename failed");
        s_snap.color = UI_ACCENT_RED;
    } else {
        nfc_format_card_detail(&s_card, s_snap.status, sizeof(s_snap.status),
                               s_snap.detail, sizeof(s_snap.detail));
        if (s_name[0]) {
            snprintf(s_snap.status, sizeof(s_snap.status), "%s",
                     s_card.type[0] ? s_card.type : s_name);
        }
        s_snap.color = UI_ACCENT_GREEN;
    }
    s_snap.enable_emu = s_loaded;
    if (!ui_lvgl_async_call(apply_snap_async, NULL))
        ESP_LOGW(TAG, "rename UI schedule failed");
}

static void on_rename_confirm(const char *text, void *user_data)
{
    (void)user_data;
    s_text_input_open = false;
    if (s_idx < 0 || !text) return;

    if (!text[0] || !nfc_name_is_valid(text)) {
        if (s_status_lbl) {
            lv_label_set_text(s_status_lbl, "Rename: invalid name");
            lv_obj_set_style_text_color(s_status_lbl, UI_ACCENT_RED, 0);
        }
        return;
    }

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "nfc_rename %d %s", s_idx, text);
    if (s_status_lbl) {
        lv_label_set_text_fmt(s_status_lbl, "Renaming...");
        lv_obj_set_style_text_color(s_status_lbl, UI_ACCENT_BLUE, 0);
    }
    uart_start_collect("[NFC] END", on_rename_collected);
    uart_send_command(cmd);
}

static void on_rename_cancel(void *user_data)
{
    (void)user_data;
    s_text_input_open = false;
}

static void on_rename(lv_event_t *e)
{
    (void)e;
    if (s_idx < 0 || s_text_input_open || s_confirm_popup) return;

    char initial[64];
    snprintf(initial, sizeof(initial), "%s", s_name);
    s_text_input_open = true;
    ui_show_text_input_popup("Rename card",
                             initial,
                             48,
                             UI_ACCENT_BLUE,
                             on_rename_confirm,
                             on_rename_cancel,
                             NULL);
}

static void go_list_async(void *unused)
{
    (void)unused;
    teardown();
    show_nfc_list_screen();
}

static void on_delete_collected(const char **lines, int count)
{
    (void)lines;
    (void)count;
    if (!ui_lvgl_async_call(go_list_async, NULL))
        ESP_LOGW(TAG, "delete UI schedule failed");
}

static void close_confirm_popup(void)
{
    if (s_confirm_popup) {
        lv_obj_delete(s_confirm_popup);
        s_confirm_popup = NULL;
    }
}

static void on_delete_confirmed(lv_event_t *e)
{
    (void)e;
    close_confirm_popup();
    if (s_idx < 0) return;

    char cmd[48];
    snprintf(cmd, sizeof(cmd), "nfc_delete %d", s_idx);
    if (s_status_lbl) {
        lv_label_set_text_fmt(s_status_lbl, "Deleting...");
        lv_obj_set_style_text_color(s_status_lbl, UI_ACCENT_RED, 0);
    }
    uart_start_collect("[NFC] END", on_delete_collected);
    uart_send_command(cmd);
}

static void on_delete_cancel(lv_event_t *e)
{
    (void)e;
    close_confirm_popup();
}

static void show_delete_confirm_popup(void)
{
    close_confirm_popup();

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
    if (s_name[0])
        lv_label_set_text_fmt(title, "Delete %s?", s_name);
    else
        lv_label_set_text_fmt(title, "Delete #%d?", s_idx);
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

static void on_delete(lv_event_t *e)
{
    (void)e;
    if (s_idx < 0 || s_text_input_open) return;
    show_delete_confirm_popup();
}

static void on_back(lv_event_t *e)
{
    (void)e;
    teardown();
    show_nfc_list_screen();
}

static void kb_poll_cb(lv_timer_t *t)
{
    (void)t;
    if (s_text_input_open || s_confirm_popup) return;
    uint8_t key = cardkb_read_key();
    if (key == 0) return;
    if (key == 0x1B || key == 0x08 || key == 0x7F)
        on_back(NULL);
}

void show_nfc_detail_screen(int idx)
{
    s_idx = idx;
    s_name[0] = '\0';
    s_loaded = false;
    s_text_input_open = false;
    s_status_lbl = NULL;
    s_detail_lbl = NULL;
    s_emu_btn = NULL;
    s_confirm_popup = NULL;
    s_kb_timer = NULL;
    nfc_card_reset(&s_card);
    snprintf(s_name, sizeof(s_name), "#%d", idx);

    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "NFC Card", on_back, NULL);

    s_status_lbl = lv_label_create(scr);
    lv_obj_set_width(s_status_lbl, 300);
    lv_obj_align(s_status_lbl, LV_ALIGN_TOP_MID, 0, 42);
    lv_obj_set_style_text_align(s_status_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_status_lbl, ui_muted_color(), 0);
    lv_label_set_text(s_status_lbl, "Loading...");

    s_detail_lbl = lv_label_create(scr);
    lv_obj_set_width(s_detail_lbl, 300);
    lv_obj_align(s_detail_lbl, LV_ALIGN_TOP_MID, 0, 66);
    lv_obj_set_style_text_align(s_detail_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_detail_lbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_detail_lbl, ui_muted_color(), 0);
    lv_label_set_long_mode(s_detail_lbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_detail_lbl, "");

    lv_obj_t *brow = lv_obj_create(scr);
    lv_obj_set_size(brow, LV_PCT(100), 40);
    lv_obj_align(brow, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_opa(brow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(brow, 0, 0);
    lv_obj_set_style_pad_all(brow, 0, 0);
    lv_obj_set_style_pad_gap(brow, 6, 0);
    lv_obj_set_flex_flow(brow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(brow, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(brow, LV_OBJ_FLAG_SCROLLABLE);

    s_emu_btn = lv_btn_create(brow);
    lv_obj_set_size(s_emu_btn, 96, 36);
    lv_obj_set_style_bg_color(s_emu_btn, UI_ACCENT_ORANGE, 0);
    lv_obj_set_style_radius(s_emu_btn, 6, 0);
    lv_obj_add_event_cb(s_emu_btn, on_emulate, LV_EVENT_CLICKED, NULL);
    lv_obj_t *el = lv_label_create(s_emu_btn);
    lv_label_set_text(el, "Emulate");
    lv_obj_set_style_text_color(el, lv_color_white(), 0);
    lv_obj_set_style_text_font(el, &lv_font_montserrat_12, 0);
    lv_obj_center(el);
    set_emu_enabled(false);

    lv_obj_t *rb = lv_btn_create(brow);
    lv_obj_set_size(rb, 96, 36);
    lv_obj_set_style_bg_color(rb, UI_ACCENT_BLUE, 0);
    lv_obj_set_style_radius(rb, 6, 0);
    lv_obj_add_event_cb(rb, on_rename, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rl = lv_label_create(rb);
    lv_label_set_text(rl, "Rename");
    lv_obj_set_style_text_color(rl, lv_color_white(), 0);
    lv_obj_set_style_text_font(rl, &lv_font_montserrat_12, 0);
    lv_obj_center(rl);

    lv_obj_t *db = lv_btn_create(brow);
    lv_obj_set_size(db, 96, 36);
    lv_obj_set_style_bg_color(db, UI_ACCENT_RED, 0);
    lv_obj_set_style_radius(db, 6, 0);
    lv_obj_add_event_cb(db, on_delete, LV_EVENT_CLICKED, NULL);
    lv_obj_t *dl = lv_label_create(db);
    lv_label_set_text(dl, "Delete");
    lv_obj_set_style_text_color(dl, lv_color_white(), 0);
    lv_obj_set_style_text_font(dl, &lv_font_montserrat_12, 0);
    lv_obj_center(dl);

    start_load();
    s_kb_timer = lv_timer_create(kb_poll_cb, 50, NULL);
    ESP_LOGI(TAG, "NFC detail idx=%d", idx);
}
