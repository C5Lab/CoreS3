#include "nfc_read_screen.h"
#include "nfc_screen.h"
#include "nfc_parser.h"
#include "ui_helpers.h"
#include "uart_handler.h"
#include "cardkb.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "nfc_read";

#define NFC_READ_UI_TIMEOUT_MS 20000

static lv_obj_t   *s_status_lbl;
static lv_obj_t   *s_detail_lbl;
static lv_obj_t   *s_read_btn;
static lv_obj_t   *s_save_btn;
static lv_timer_t *s_kb_timer;
static lv_timer_t *s_timeout_timer;
static bool        s_busy;
static bool        s_have_card;
static bool        s_text_input_open;
static nfc_ui_card_t s_card;

typedef struct {
    char status[64];
    char detail[160];
    lv_color_t color;
    bool enable_save;
} nfc_read_ui_snap_t;

static nfc_read_ui_snap_t s_snap;

static void on_back(lv_event_t *e);
static void on_read(lv_event_t *e);
static void on_save(lv_event_t *e);
static void kb_poll_cb(lv_timer_t *t);
static void read_timeout_cb(lv_timer_t *t);
static void nfc_line_cb(const char *line);
static void finish_read_ui(void);
static void set_save_enabled(bool en);
static void on_save_confirm(const char *text, void *user_data);
static void on_save_cancel(void *user_data);
static void save_collect_cb(const char **lines, int count);

static void stop_timeout(void)
{
    if (s_timeout_timer) {
        lv_timer_delete(s_timeout_timer);
        s_timeout_timer = NULL;
    }
}

static void set_save_enabled(bool en)
{
    if (!s_save_btn) return;
    if (en)
        lv_obj_clear_state(s_save_btn, LV_STATE_DISABLED);
    else
        lv_obj_add_state(s_save_btn, LV_STATE_DISABLED);
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
    set_save_enabled(s_snap.enable_save);
}

static void apply_card_to_snap(void)
{
    if (s_card.no_card) {
        snprintf(s_snap.status, sizeof(s_snap.status), "No card detected");
        snprintf(s_snap.detail, sizeof(s_snap.detail), "Try again");
        s_snap.color = UI_ACCENT_ORANGE;
        s_snap.enable_save = false;
        s_have_card = false;
        return;
    }
    if (s_card.not_detected || s_card.not_initialized) {
        snprintf(s_snap.status, sizeof(s_snap.status), "NFC not ready");
        snprintf(s_snap.detail, sizeof(s_snap.detail), "Check wiring / power");
        s_snap.color = UI_ACCENT_RED;
        s_snap.enable_save = false;
        s_have_card = false;
        return;
    }
    if (!s_card.type[0] && !s_card.uid[0]) {
        snprintf(s_snap.status, sizeof(s_snap.status), "No result");
        s_snap.detail[0] = '\0';
        s_snap.color = ui_muted_color();
        s_snap.enable_save = false;
        s_have_card = false;
        return;
    }

    nfc_format_card_detail(&s_card, s_snap.status, sizeof(s_snap.status),
                           s_snap.detail, sizeof(s_snap.detail));
    s_snap.color = UI_ACCENT_GREEN;
    s_snap.enable_save = true;
    s_have_card = true;
}

static void finish_read_ui(void)
{
    if (!s_busy) return;
    s_busy = false;
    stop_timeout();
    uart_set_line_callback(NULL);
    if (s_read_btn)
        lv_obj_clear_state(s_read_btn, LV_STATE_DISABLED);
    apply_card_to_snap();
    apply_snap_async(NULL);
}

static void finish_read_async(void *unused)
{
    (void)unused;
    finish_read_ui();
}

static void nfc_line_cb(const char *line)
{
    if (!line) return;

    if (nfc_line_is_end(line)) {
        ui_lvgl_async_call(finish_read_async, NULL);
        return;
    }

    nfc_parse_card_line(line, &s_card);

    if (strstr(line, "[NFC] present a card")) {
        snprintf(s_snap.status, sizeof(s_snap.status), "Present a card...");
        s_snap.detail[0] = '\0';
        s_snap.color = UI_ACCENT_CYAN;
        s_snap.enable_save = false;
        ui_lvgl_async_call(apply_snap_async, NULL);
        return;
    }

    if (strncmp(line, "[NFC] type: ", 12) == 0) {
        snprintf(s_snap.status, sizeof(s_snap.status), "%s", s_card.type);
        snprintf(s_snap.detail, sizeof(s_snap.detail), "Reading...");
        s_snap.color = UI_ACCENT_CYAN;
        s_snap.enable_save = false;
        ui_lvgl_async_call(apply_snap_async, NULL);
    }
}

static void read_timeout_cb(lv_timer_t *t)
{
    (void)t;
    s_timeout_timer = NULL;
    if (!s_busy) return;
    ESP_LOGW(TAG, "nfc_read UI timeout");
    s_card.no_card = true;
    finish_read_ui();
}

static void on_read(lv_event_t *e)
{
    (void)e;
    if (s_busy || s_text_input_open) return;

    uart_stop_collect();
    nfc_card_reset(&s_card);
    s_have_card = false;
    s_busy = true;
    set_save_enabled(false);

    if (s_status_lbl) {
        lv_label_set_text(s_status_lbl, "Present a card...");
        lv_obj_set_style_text_color(s_status_lbl, UI_ACCENT_CYAN, 0);
    }
    if (s_detail_lbl)
        lv_label_set_text(s_detail_lbl, "");
    if (s_read_btn)
        lv_obj_add_state(s_read_btn, LV_STATE_DISABLED);

    stop_timeout();
    s_timeout_timer = lv_timer_create(read_timeout_cb, NFC_READ_UI_TIMEOUT_MS, NULL);
    lv_timer_set_repeat_count(s_timeout_timer, 1);

    uart_set_line_callback(nfc_line_cb);
    uart_send_command("nfc_read");
    ESP_LOGI(TAG, "nfc_read started");
}

static void save_collect_cb(const char **lines, int count)
{
    bool ok = false;
    bool nothing = false;
    char path[96] = {0};

    for (int i = 0; i < count; i++) {
        if (!lines[i]) continue;
        if (strncmp(lines[i], "[NFC] saved: ", 13) == 0) {
            snprintf(path, sizeof(path), "%s", lines[i] + 13);
            ok = true;
        }
        if (strstr(lines[i], "[NFC] nothing to save"))
            nothing = true;
    }

    if (ok) {
        snprintf(s_snap.status, sizeof(s_snap.status), "Saved");
        snprintf(s_snap.detail, sizeof(s_snap.detail), "%s", path);
        s_snap.color = UI_ACCENT_GREEN;
        s_snap.enable_save = s_have_card;
    } else if (nothing) {
        snprintf(s_snap.status, sizeof(s_snap.status), "Nothing to save");
        s_snap.detail[0] = '\0';
        s_snap.color = UI_ACCENT_ORANGE;
        s_snap.enable_save = false;
    } else {
        snprintf(s_snap.status, sizeof(s_snap.status), "Save failed");
        s_snap.detail[0] = '\0';
        s_snap.color = UI_ACCENT_RED;
        s_snap.enable_save = s_have_card;
    }
    ui_lvgl_async_call(apply_snap_async, NULL);
}

static void on_save_confirm(const char *text, void *user_data)
{
    (void)user_data;
    s_text_input_open = false;
    if (!text || !text[0]) {
        if (s_status_lbl) {
            lv_label_set_text(s_status_lbl, "Save: empty name");
            lv_obj_set_style_text_color(s_status_lbl, UI_ACCENT_RED, 0);
        }
        return;
    }
    if (!nfc_name_is_valid(text)) {
        if (s_status_lbl) {
            lv_label_set_text(s_status_lbl, "Save: invalid name");
            lv_obj_set_style_text_color(s_status_lbl, UI_ACCENT_RED, 0);
        }
        return;
    }

    char cmd[96];
    snprintf(cmd, sizeof(cmd), "nfc_save %s", text);
    if (s_status_lbl) {
        lv_label_set_text_fmt(s_status_lbl, "Saving %s...", text);
        lv_obj_set_style_text_color(s_status_lbl, UI_ACCENT_BLUE, 0);
    }
    uart_send_command(cmd);
    uart_start_collect("[NFC] END", save_collect_cb);
    ESP_LOGI(TAG, "nfc_save %s", text);
}

static void on_save_cancel(void *user_data)
{
    (void)user_data;
    s_text_input_open = false;
}

static void on_save(lv_event_t *e)
{
    (void)e;
    if (s_busy || s_text_input_open || !s_have_card) return;

    s_text_input_open = true;
    ui_show_text_input_popup("Save card",
                             "",
                             48,
                             UI_ACCENT_BLUE,
                             on_save_confirm,
                             on_save_cancel,
                             NULL);
}

static void on_back(lv_event_t *e)
{
    (void)e;
    stop_timeout();
    uart_stop_collect();
    uart_set_line_callback(NULL);
    if (s_kb_timer) { lv_timer_delete(s_kb_timer); s_kb_timer = NULL; }
    s_status_lbl = NULL;
    s_detail_lbl = NULL;
    s_read_btn = NULL;
    s_save_btn = NULL;
    s_busy = false;
    s_text_input_open = false;
    show_nfc_screen();
}

static void kb_poll_cb(lv_timer_t *t)
{
    (void)t;
    if (s_text_input_open) return;
    uint8_t key = cardkb_read_key();
    if (key == 0) return;
    if (key == 0x1B || key == 0x08 || key == 0x7F)
        on_back(NULL);
}

void show_nfc_read_screen(void)
{
    s_status_lbl = NULL;
    s_detail_lbl = NULL;
    s_read_btn = NULL;
    s_save_btn = NULL;
    s_kb_timer = NULL;
    s_timeout_timer = NULL;
    s_busy = false;
    s_have_card = false;
    s_text_input_open = false;
    nfc_card_reset(&s_card);

    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "NFC Read", on_back, NULL);

    s_status_lbl = lv_label_create(scr);
    lv_obj_set_width(s_status_lbl, 300);
    lv_obj_align(s_status_lbl, LV_ALIGN_TOP_MID, 0, 42);
    lv_obj_set_style_text_align(s_status_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_status_lbl, ui_muted_color(), 0);
    lv_label_set_text(s_status_lbl, "Ready");

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
    lv_obj_set_style_pad_gap(brow, 10, 0);
    lv_obj_set_flex_flow(brow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(brow, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(brow, LV_OBJ_FLAG_SCROLLABLE);

    s_read_btn = lv_btn_create(brow);
    lv_obj_set_size(s_read_btn, 110, 36);
    lv_obj_set_style_bg_color(s_read_btn, UI_ACCENT_CYAN, 0);
    lv_obj_set_style_radius(s_read_btn, 6, 0);
    lv_obj_add_event_cb(s_read_btn, on_read, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rl = lv_label_create(s_read_btn);
    lv_label_set_text(rl, LV_SYMBOL_REFRESH " Read");
    lv_obj_set_style_text_color(rl, lv_color_white(), 0);
    lv_obj_center(rl);

    s_save_btn = lv_btn_create(brow);
    lv_obj_set_size(s_save_btn, 110, 36);
    lv_obj_set_style_bg_color(s_save_btn, UI_ACCENT_GREEN, 0);
    lv_obj_set_style_radius(s_save_btn, 6, 0);
    lv_obj_add_event_cb(s_save_btn, on_save, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(s_save_btn);
    lv_label_set_text(sl, LV_SYMBOL_SAVE " Save");
    lv_obj_set_style_text_color(sl, lv_color_white(), 0);
    lv_obj_center(sl);
    set_save_enabled(false);

    s_kb_timer = lv_timer_create(kb_poll_cb, 50, NULL);
    ESP_LOGI(TAG, "NFC Read screen ready");
}
