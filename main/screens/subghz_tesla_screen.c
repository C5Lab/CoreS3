#include "subghz_tesla_screen.h"
#include "subghz_screen.h"
#include "ui_helpers.h"
#include "uart_handler.h"
#include "cardkb.h"
#include "led_indicator.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "subghz_tesla";

static lv_obj_t   *s_status_lbl;
static lv_timer_t  *s_kb_timer;

static void on_back(lv_event_t *e);
static void kb_poll_cb(lv_timer_t *t);

static void on_open_port(lv_event_t *e)
{
    (void)e;

    uart_send_command("subghz_freq 315.00");
    uart_send_command("subghz_tx tesla");
    led_indicator_tx_pulse(1200);

    if (s_status_lbl) {
        lv_label_set_text(s_status_lbl, "Signal sent!");
        lv_obj_set_style_text_color(s_status_lbl, UI_ACCENT_GREEN, 0);
    }

    ESP_LOGI(TAG, "Tesla charge port signal sent (315 MHz)");
}

static void on_back(lv_event_t *e)
{
    (void)e;
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
    if (key == ' ' || key == '\r')
        on_open_port(NULL);
}

void show_subghz_tesla_screen(void)
{
    s_status_lbl = NULL;
    s_kb_timer   = NULL;

    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Tesla", on_back, NULL);

    /* Icon — just below 36 px top bar (same idea as Jammer screen) */
    lv_obj_t *icon = lv_label_create(scr);
    lv_label_set_text(icon, LV_SYMBOL_POWER);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(icon, UI_ACCENT_PURPLE, 0);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 40);

    /* Title */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Charge Port Opener");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, ui_text_color(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 64);

    /* Freq info */
    lv_obj_t *freq = lv_label_create(scr);
    lv_label_set_text(freq, "315.00 MHz OOK");
    lv_obj_set_style_text_font(freq, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(freq, ui_muted_color(), 0);
    lv_obj_align(freq, LV_ALIGN_TOP_MID, 0, 84);

    /* Big button */
    lv_obj_t *btn = lv_btn_create(scr);
    lv_obj_set_size(btn, 200, 55);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, 118);
    lv_obj_set_style_bg_color(btn, UI_ACCENT_PURPLE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x7B1FA2), LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_add_event_cb(btn, on_open_port, LV_EVENT_CLICKED, NULL);

    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, LV_SYMBOL_POWER " Open Port");
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_16, 0);
    lv_obj_center(bl);

    /* Status */
    s_status_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_status_lbl, ui_muted_color(), 0);
    lv_label_set_text(s_status_lbl, "Ready");
    lv_obj_align(s_status_lbl, LV_ALIGN_TOP_MID, 0, 182);

    s_kb_timer = lv_timer_create(kb_poll_cb, 50, NULL);

    ESP_LOGI(TAG, "Tesla screen ready");
}
