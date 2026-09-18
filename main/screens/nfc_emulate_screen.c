#include "nfc_emulate_screen.h"
#include "nfc_detail_screen.h"
#include "nfc_parser.h"
#include "ui_helpers.h"
#include "uart_handler.h"
#include "cardkb.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "nfc_emulate";

static int         s_idx = -1;
static bool        s_hint_have_data;
static char        s_hint_type[32];
static lv_obj_t   *s_status_lbl;
static lv_obj_t   *s_detail_lbl;
static lv_timer_t *s_kb_timer;
static bool        s_running;

static void on_back(lv_event_t *e);
static void kb_poll_cb(lv_timer_t *t);
static void emulate_collect_cb(const char **lines, int count);

typedef struct {
    char status[80];
    char detail[160];
    lv_color_t color;
} emu_snap_t;

static emu_snap_t s_snap;

static void apply_snap_async(void *unused)
{
    (void)unused;
    if (s_status_lbl) {
        lv_label_set_text(s_status_lbl, s_snap.status);
        lv_obj_set_style_text_color(s_status_lbl, s_snap.color, 0);
    }
    if (s_detail_lbl)
        lv_label_set_text(s_detail_lbl, s_snap.detail);
}

static void fill_capability_copy(const nfc_ui_card_t *card, const char *summary)
{
    const char *type = card->type[0] ? card->type :
                       (s_hint_type[0] ? s_hint_type : summary);
    bool classic = nfc_type_is_classic(type);
    bool ul = nfc_type_is_ultralight(type);
    bool have_data = card->have_data || s_hint_have_data;

    if (card->emulate_full_ul) {
        snprintf(s_snap.detail, sizeof(s_snap.detail),
                 "%s\nNTAG/Ultralight pages",
                 summary[0] ? summary : "");
        return;
    }
    if (card->emulate_uid_only) {
        snprintf(s_snap.detail, sizeof(s_snap.detail),
                 "%s\nUID/ATQA/SAK only%s",
                 summary[0] ? summary : "",
                 classic ? "\nClassic Crypto1 not emulated" : "");
        return;
    }

    /* Firmware omitted the capability line — infer from bus + type. */
    if (nfc_bus_mode == NFC_BUS_MODE_I2C) {
        snprintf(s_snap.detail, sizeof(s_snap.detail),
                 "%s\nUID/ATQA/SAK only%s",
                 summary[0] ? summary : "",
                 classic ? "\nClassic Crypto1 not emulated" : "");
        return;
    }

    if (ul && have_data) {
        snprintf(s_snap.detail, sizeof(s_snap.detail),
                 "%s\nNTAG/Ultralight pages",
                 summary[0] ? summary : "");
        return;
    }

    snprintf(s_snap.detail, sizeof(s_snap.detail),
             "%s\nUID/ATQA/SAK only%s",
             summary[0] ? summary : "",
             classic ? "\nClassic Crypto1 not emulated" : "");
}

static void emulate_collect_cb(const char **lines, int count)
{
    nfc_ui_card_t card;
    nfc_card_reset(&card);

    for (int i = 0; i < count; i++) {
        if (!lines[i]) continue;
        nfc_parse_card_line(lines[i], &card);
    }

    const char *summary = card.have_emulating ? card.emulating_summary : "";

    if (card.have_emulating) {
        s_running = true;
        snprintf(s_snap.status, sizeof(s_snap.status), "Emulating");
        fill_capability_copy(&card, summary);
        s_snap.color = UI_ACCENT_GREEN;
    } else if (card.emulate_failed || card.no_card_loaded || card.load_failed) {
        s_running = false;
        snprintf(s_snap.status, sizeof(s_snap.status), "Emulate failed");
        snprintf(s_snap.detail, sizeof(s_snap.detail),
                 "Only NFC-A UID emulation");
        s_snap.color = UI_ACCENT_RED;
    } else {
        s_running = false;
        snprintf(s_snap.status, sizeof(s_snap.status), "Start failed");
        s_snap.detail[0] = '\0';
        s_snap.color = UI_ACCENT_RED;
    }
    ui_lvgl_async_call(apply_snap_async, NULL);
}

static void teardown(void)
{
    uart_stop_collect();
    if (s_running) {
        uart_send_command("stop");
        s_running = false;
    }
    if (s_kb_timer) { lv_timer_delete(s_kb_timer); s_kb_timer = NULL; }
    s_status_lbl = NULL;
    s_detail_lbl = NULL;
}

static void on_back(lv_event_t *e)
{
    (void)e;
    int idx = s_idx;
    teardown();
    show_nfc_detail_screen(idx);
}

static void kb_poll_cb(lv_timer_t *t)
{
    (void)t;
    uint8_t key = cardkb_read_key();
    if (key == 0) return;
    if (key == 0x1B || key == 0x08 || key == 0x7F)
        on_back(NULL);
}

void nfc_emulate_set_card_hint(bool have_data, const char *type)
{
    s_hint_have_data = have_data;
    snprintf(s_hint_type, sizeof(s_hint_type), "%s", type ? type : "");
}

void show_nfc_emulate_screen(int idx)
{
    s_idx = idx;
    s_running = false;
    s_status_lbl = NULL;
    s_detail_lbl = NULL;
    s_kb_timer = NULL;

    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "NFC Emulate", on_back, NULL);

    lv_obj_t *icon = lv_label_create(scr);
    lv_label_set_text(icon, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(icon, UI_ACCENT_ORANGE, 0);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 48);

    s_status_lbl = lv_label_create(scr);
    lv_obj_set_width(s_status_lbl, 300);
    lv_obj_align(s_status_lbl, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_set_style_text_align(s_status_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_status_lbl, UI_ACCENT_CYAN, 0);
    lv_label_set_text(s_status_lbl, "Starting...");

    s_detail_lbl = lv_label_create(scr);
    lv_obj_set_width(s_detail_lbl, 300);
    lv_obj_align(s_detail_lbl, LV_ALIGN_TOP_MID, 0, 110);
    lv_obj_set_style_text_align(s_detail_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_detail_lbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_detail_lbl, ui_muted_color(), 0);
    lv_label_set_long_mode(s_detail_lbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text_fmt(s_detail_lbl, "Card #%d", idx);

    lv_obj_t *hint = lv_label_create(scr);
    lv_obj_set_width(hint, 300);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(hint, ui_muted_color(), 0);
    lv_label_set_text(hint, "Back sends stop");

    uart_start_collect("[NFC] END", emulate_collect_cb);
    uart_send_command("start_nfc_emulate");

    s_kb_timer = lv_timer_create(kb_poll_cb, 50, NULL);
    ESP_LOGI(TAG, "NFC Emulate idx=%d (loaded slot)", idx);
}
