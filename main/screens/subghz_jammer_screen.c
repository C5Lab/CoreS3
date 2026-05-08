#include "subghz_jammer_screen.h"
#include "subghz_screen.h"
#include "ui_helpers.h"
#include "uart_handler.h"
#include "cardkb.h"
#include "led_indicator.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "subghz_jam";

static bool        s_jamming;
static float       s_freq_mhz = 433.92f;
static lv_obj_t   *s_status_lbl;
static lv_obj_t   *s_freq_lbl;
static lv_obj_t   *s_big_btn;
static lv_obj_t   *s_big_btn_lbl;
static lv_obj_t   *s_freq_popup;
static lv_obj_t   *s_freq_backdrop;
static lv_timer_t *s_kb_timer;

static void on_back(lv_event_t *e);
static void kb_poll_cb(lv_timer_t *t);

static void close_freq_popup(void)
{
    if (s_freq_popup) {
        lv_obj_delete(s_freq_popup);
        s_freq_popup = NULL;
    }
    if (s_freq_backdrop) {
        lv_obj_delete(s_freq_backdrop);
        s_freq_backdrop = NULL;
    }
}

static float s_p315  = 315.00f;
static float s_p433  = 433.92f;
static float s_p868  = 868.00f;
static float s_p915  = 915.00f;

static void on_freq_preset(lv_event_t *e)
{
    float *freq = (float *)lv_event_get_user_data(e);
    s_freq_mhz = *freq;
    if (s_freq_lbl) {
        int whole = (int)s_freq_mhz;
        int frac  = ((int)(s_freq_mhz * 100.0f + 0.5f)) % 100;
        lv_label_set_text_fmt(s_freq_lbl, "%d.%02d MHz", whole, frac);
    }
    close_freq_popup();
}

static void on_freq_popup_close(lv_event_t *e)
{
    (void)e;
    close_freq_popup();
}

static void on_backdrop_tap(lv_event_t *e)
{
    (void)e;
    close_freq_popup();
}

static void on_freq_tap(lv_event_t *e)
{
    (void)e;
    if (s_jamming) return;
    if (s_freq_popup) { close_freq_popup(); return; }

    /* Full-screen dim backdrop so the popup reads as a modal overlay. */
    s_freq_backdrop = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_freq_backdrop);
    lv_obj_set_size(s_freq_backdrop, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_freq_backdrop, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_freq_backdrop, LV_OPA_60, 0);
    lv_obj_add_flag(s_freq_backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_freq_backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_freq_backdrop, on_backdrop_tap, LV_EVENT_CLICKED, NULL);

    s_freq_popup = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_freq_popup, 200, 130);
    lv_obj_align(s_freq_popup, LV_ALIGN_CENTER, 0, 0);
    style_popup_card(s_freq_popup, 10, UI_ACCENT_RED);
    lv_obj_set_flex_flow(s_freq_popup, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_freq_popup, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(s_freq_popup, 8, 0);
    lv_obj_set_style_pad_gap(s_freq_popup, 6, 0);
    lv_obj_clear_flag(s_freq_popup, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_freq_popup);
    lv_label_set_text(title, "Frequency (MHz)");
    lv_obj_set_width(title, LV_PCT(100));
    lv_obj_set_style_text_color(title, ui_text_color(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    static const struct { const char *txt; float *val; } presets[] = {
        {"315",    &s_p315},
        {"433.92", &s_p433},
        {"868",    &s_p868},
        {"915",    &s_p915},
    };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *btn = lv_btn_create(s_freq_popup);
        lv_obj_set_size(btn, 80, 28);
        lv_obj_set_style_bg_color(btn, ui_card_color(), 0);
        lv_obj_set_style_bg_color(btn, ui_card_pressed_color(), LV_STATE_PRESSED);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_add_event_cb(btn, on_freq_preset, LV_EVENT_CLICKED, presets[i].val);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, presets[i].txt);
        lv_obj_set_style_text_color(lbl, ui_text_color(), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_center(lbl);
    }

    lv_obj_t *close_btn = lv_btn_create(s_freq_popup);
    lv_obj_set_size(close_btn, 170, 24);
    lv_obj_set_style_bg_color(close_btn, ui_muted_color(), 0);
    lv_obj_set_style_radius(close_btn, 6, 0);
    lv_obj_add_event_cb(close_btn, on_freq_popup_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(close_btn);
    lv_label_set_text(cl, "Close");
    lv_obj_set_style_text_color(cl, lv_color_white(), 0);
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_12, 0);
    lv_obj_center(cl);
}

static void stop_jamming(void)
{
    if (!s_jamming) return;
    s_jamming = false;
    uart_send_command("subghz_stop");
    led_indicator_tx_stop();

    if (s_status_lbl) {
        lv_label_set_text(s_status_lbl, "Idle");
        lv_obj_set_style_text_color(s_status_lbl, ui_muted_color(), 0);
    }
    if (s_big_btn)
        lv_obj_set_style_bg_color(s_big_btn, UI_ACCENT_RED, 0);
    if (s_big_btn_lbl)
        lv_label_set_text(s_big_btn_lbl, LV_SYMBOL_WARNING " START JAM");

    ESP_LOGI(TAG, "Jammer stopped");
}

static void on_big_btn(lv_event_t *e)
{
    (void)e;
    if (s_jamming) {
        stop_jamming();
        return;
    }

    s_jamming = true;

    char cmd[32];
    snprintf(cmd, sizeof(cmd), "subghz_freq %.2f", s_freq_mhz);
    uart_send_command(cmd);
    uart_send_command("subghz_jam");
    led_indicator_tx_start();

    if (s_status_lbl) {
        lv_label_set_text_fmt(s_status_lbl, "Jamming %d.%02d MHz...",
                              (int)s_freq_mhz, ((int)(s_freq_mhz * 100.0f + 0.5f)) % 100);
        lv_obj_set_style_text_color(s_status_lbl, UI_ACCENT_RED, 0);
    }
    if (s_big_btn)
        lv_obj_set_style_bg_color(s_big_btn, lv_color_hex(0x8B0000), 0);
    if (s_big_btn_lbl)
        lv_label_set_text(s_big_btn_lbl, LV_SYMBOL_STOP " STOP");

    ESP_LOGI(TAG, "Jammer started on %.2f MHz", s_freq_mhz);
}

static void on_back(lv_event_t *e)
{
    (void)e;
    stop_jamming();
    close_freq_popup();
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
    if (key == ' ') on_big_btn(NULL);
}

void show_subghz_jammer_screen(void)
{
    s_jamming      = false;
    s_status_lbl   = NULL;
    s_freq_lbl     = NULL;
    s_big_btn      = NULL;
    s_big_btn_lbl  = NULL;
    s_freq_popup   = NULL;
    s_freq_backdrop = NULL;
    s_kb_timer     = NULL;

    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Jammer", on_back, NULL);

    /* Frequency label (tappable) — sits just below the 36 px top bar */
    s_freq_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_freq_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_freq_lbl, UI_ACCENT_RED, 0);
    {
        int whole = (int)s_freq_mhz;
        int frac  = ((int)(s_freq_mhz * 100.0f + 0.5f)) % 100;
        lv_label_set_text_fmt(s_freq_lbl, "%d.%02d MHz", whole, frac);
    }
    lv_obj_align(s_freq_lbl, LV_ALIGN_TOP_MID, 0, 46);
    lv_obj_add_flag(s_freq_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_freq_lbl, on_freq_tap, LV_EVENT_CLICKED, NULL);

    /* Big start/stop button — anchored to the screen center */
    s_big_btn = lv_btn_create(scr);
    lv_obj_set_size(s_big_btn, 180, 60);
    lv_obj_align(s_big_btn, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_style_bg_color(s_big_btn, UI_ACCENT_RED, 0);
    lv_obj_set_style_radius(s_big_btn, 12, 0);
    lv_obj_add_event_cb(s_big_btn, on_big_btn, LV_EVENT_CLICKED, NULL);

    s_big_btn_lbl = lv_label_create(s_big_btn);
    lv_label_set_text(s_big_btn_lbl, LV_SYMBOL_WARNING " START JAM");
    lv_obj_set_style_text_font(s_big_btn_lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(s_big_btn_lbl);

    /* Status — pinned to the bottom edge */
    s_status_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_status_lbl, ui_muted_color(), 0);
    lv_label_set_text(s_status_lbl, "Idle");
    lv_obj_align(s_status_lbl, LV_ALIGN_BOTTOM_MID, 0, -10);

    s_kb_timer = lv_timer_create(kb_poll_cb, 50, NULL);

    ESP_LOGI(TAG, "SubGHz Jammer screen ready");
}
