#include "nfc_emulate_screen.h"
#include "nfc_list_screen.h"
#include "ui_helpers.h"
#include "uart_handler.h"
#include "cardkb.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "nfc_emulate";

static lv_obj_t   *s_status_lbl;
static lv_obj_t   *s_detail_lbl;
static lv_timer_t *s_kb_timer;
static bool        s_running;

static void on_back(lv_event_t *e);
static void kb_poll_cb(lv_timer_t *t);
static void emulate_collect_cb(const char **lines, int count);

typedef struct {
    char status[80];
    char detail[120];
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

static void emulate_collect_cb(const char **lines, int count)
{
    bool ok = false;
    bool failed = false;
    char summary[96] = {0};

    for (int i = 0; i < count; i++) {
        const char *line = lines[i];
        if (!line) continue;
        if (strncmp(line, "[NFC] emulating ", 16) == 0) {
            snprintf(summary, sizeof(summary), "%s", line + 16);
            char *use = strstr(summary, ". Use ");
            if (use) *use = '\0';
            ok = true;
        }
        if (strstr(line, "[NFC] emulate failed") ||
            strstr(line, "[NFC] no card loaded") ||
            strstr(line, "[NFC] load failed") ||
            strstr(line, "[NFC] not found")) {
            failed = true;
        }
    }

    if (ok) {
        s_running = true;
        snprintf(s_snap.status, sizeof(s_snap.status), "Emulating");
        snprintf(s_snap.detail, sizeof(s_snap.detail),
                 "%s\nUID/ATQA/SAK only", summary[0] ? summary : "");
        s_snap.color = UI_ACCENT_GREEN;
    } else if (failed) {
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
    teardown();
    show_nfc_list_screen();
}

static void kb_poll_cb(lv_timer_t *t)
{
    (void)t;
    uint8_t key = cardkb_read_key();
    if (key == 0) return;
    if (key == 0x1B || key == 0x08 || key == 0x7F)
        on_back(NULL);
}

void show_nfc_emulate_screen(int idx)
{
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

    char cmd[48];
    snprintf(cmd, sizeof(cmd), "start_nfc_emulate %d", idx);
    uart_send_command(cmd);
    uart_start_collect("[NFC] END", emulate_collect_cb);

    s_kb_timer = lv_timer_create(kb_poll_cb, 50, NULL);
    ESP_LOGI(TAG, "NFC Emulate idx=%d", idx);
}
