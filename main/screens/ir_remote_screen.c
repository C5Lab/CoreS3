#include "ir_remote_screen.h"
#include "home_screen.h"
#include "ui_helpers.h"
#include "ir_db.h"
#include "ir_tx.h"
#include "led_indicator.h"
#include "esp_log.h"

#include <stdlib.h>

static const char *TAG = "ir_remote";

/* One IR code per timer tick. The send itself blocks the LVGL thread for the
 * duration of the burst (tens of ms), mirroring a universal-remote
 * tick loop; the gap below lets the UI breathe between codes. */
#define IR_TICK_PERIOD_MS 15

static lv_timer_t *s_tx_timer;
static size_t      s_index;
static size_t      s_total;
static bool        s_running;
static bool        s_loop;

static lv_obj_t *s_bar;
static lv_obj_t *s_status;
static lv_obj_t *s_count_lbl;
static lv_obj_t *s_btn;
static lv_obj_t *s_btn_lbl;

static void tx_set_button_idle(void)
{
    if (s_btn) lv_obj_set_style_bg_color(s_btn, UI_ACCENT_GREEN, 0);
    if (s_btn_lbl) lv_label_set_text(s_btn_lbl, LV_SYMBOL_PLAY "  Start");
}

static void tx_set_button_busy(void)
{
    if (s_btn) lv_obj_set_style_bg_color(s_btn, UI_ACCENT_RED, 0);
    if (s_btn_lbl) lv_label_set_text(s_btn_lbl, LV_SYMBOL_STOP "  Stop");
}

static void tx_finish(bool completed)
{
    if (s_tx_timer) {
        lv_timer_delete(s_tx_timer);
        s_tx_timer = NULL;
    }
    s_running = false;

    led_indicator_tx_stop();
    ui_screen_idle_inhibit(false);

    tx_set_button_idle();
    if (s_status) {
        lv_label_set_text(s_status, completed ? "Done - all codes sent" : "Stopped");
        lv_obj_set_style_text_color(s_status,
                                    completed ? UI_ACCENT_GREEN : ui_muted_color(), 0);
    }
}

static void tx_tick_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_running) return;

    if (s_index >= s_total) {
        if (s_loop) {
            s_index = 0;
        } else {
            tx_finish(true);
            return;
        }
    }

    uint32_t carrier = 0;
    uint8_t duty = 33;
    uint32_t *timings = NULL;
    uint16_t count = 0;
    if (ir_db_get(s_index, &carrier, &duty, &timings, &count)) {
        ir_tx_send(timings, count, carrier, duty);
        free(timings);
    }

    s_index++;

    if (s_bar) lv_bar_set_value(s_bar, (int32_t)s_index, LV_ANIM_OFF);
    if (s_count_lbl) {
        lv_label_set_text_fmt(s_count_lbl, "%u / %u", (unsigned)s_index, (unsigned)s_total);
    }
}

static void tx_start(void)
{
    if (s_running) return;

    s_total = ir_db_count();
    if (s_total == 0) {
        if (s_status) {
            lv_label_set_text(s_status, "No IR database");
            lv_obj_set_style_text_color(s_status, UI_ACCENT_RED, 0);
        }
        return;
    }

    if (ir_tx_init() != ESP_OK) {
        if (s_status) {
            lv_label_set_text(s_status, "IR init failed");
            lv_obj_set_style_text_color(s_status, UI_ACCENT_RED, 0);
        }
        return;
    }

    s_index = 0;
    s_running = true;

    if (s_bar) {
        lv_bar_set_range(s_bar, 0, (int32_t)s_total);
        lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    }
    if (s_count_lbl) lv_label_set_text_fmt(s_count_lbl, "0 / %u", (unsigned)s_total);
    if (s_status) {
        lv_label_set_text(s_status, "Sending - point base at TV");
        lv_obj_set_style_text_color(s_status, ui_text_color(), 0);
    }

    tx_set_button_busy();
    led_indicator_tx_start();
    ui_screen_idle_inhibit(true);

    s_tx_timer = lv_timer_create(tx_tick_cb, IR_TICK_PERIOD_MS, NULL);
    ESP_LOGI(TAG, "IR TV power sweep started (%u codes)", (unsigned)s_total);
}

static void on_toggle(lv_event_t *e)
{
    (void)e;
    if (s_running) {
        tx_finish(false);
    } else {
        tx_start();
    }
}

static void on_loop_changed(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    s_loop = lv_obj_has_state(sw, LV_STATE_CHECKED);
}

static void on_back(lv_event_t *e)
{
    (void)e;
    if (s_tx_timer) {
        lv_timer_delete(s_tx_timer);
        s_tx_timer = NULL;
    }
    s_running = false;
    led_indicator_tx_stop();
    ui_screen_idle_inhibit(false);
    show_home_screen();
}

void show_ir_remote_screen(void)
{
    s_tx_timer  = NULL;
    s_index     = 0;
    s_total     = 0;
    s_running   = false;
    s_bar       = NULL;
    s_status    = NULL;
    s_count_lbl = NULL;
    s_btn       = NULL;
    s_btn_lbl   = NULL;

    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "TV Power", on_back, NULL);

    /* Title / description */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, LV_SYMBOL_VIDEO "  Universal TV Power");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, UI_ACCENT_TEAL, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 46);

    lv_obj_t *desc = lv_label_create(scr);
    lv_label_set_text(desc,
        "Sweeps every known TV power\ncode through the base IR LED.");
    lv_obj_set_style_text_font(desc, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(desc, ui_muted_color(), 0);
    lv_obj_set_style_text_align(desc, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(desc, LV_ALIGN_TOP_MID, 0, 70);

    /* Progress bar */
    s_bar = lv_bar_create(scr);
    lv_obj_set_size(s_bar, 260, 14);
    lv_obj_align(s_bar, LV_ALIGN_TOP_MID, 0, 108);
    lv_obj_set_style_bg_color(s_bar, ui_panel_color(), 0);
    lv_obj_set_style_bg_color(s_bar, UI_ACCENT_TEAL, LV_PART_INDICATOR);
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);

    /* Count label */
    s_count_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_count_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_count_lbl, ui_text_color(), 0);
    lv_label_set_text(s_count_lbl, "Ready");
    lv_obj_align(s_count_lbl, LV_ALIGN_TOP_MID, 0, 128);

    /* Start / Stop button */
    s_btn = lv_btn_create(scr);
    lv_obj_set_size(s_btn, 180, 46);
    lv_obj_set_style_bg_color(s_btn, UI_ACCENT_GREEN, 0);
    lv_obj_set_style_radius(s_btn, 8, 0);
    lv_obj_align(s_btn, LV_ALIGN_TOP_MID, 0, 152);
    lv_obj_add_event_cb(s_btn, on_toggle, LV_EVENT_CLICKED, NULL);
    s_btn_lbl = lv_label_create(s_btn);
    lv_label_set_text(s_btn_lbl, LV_SYMBOL_PLAY "  Start");
    lv_obj_set_style_text_font(s_btn_lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(s_btn_lbl);

    /* Repeat switch */
    lv_obj_t *loop_row = lv_obj_create(scr);
    lv_obj_set_size(loop_row, 180, 30);
    lv_obj_align(loop_row, LV_ALIGN_TOP_MID, 0, 204);
    lv_obj_set_flex_flow(loop_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(loop_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(loop_row, 10, 0);
    lv_obj_set_style_bg_opa(loop_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(loop_row, 0, 0);

    lv_obj_t *loop_lbl = lv_label_create(loop_row);
    lv_label_set_text(loop_lbl, "Repeat");
    lv_obj_set_style_text_font(loop_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(loop_lbl, ui_muted_color(), 0);

    lv_obj_t *loop_sw = lv_switch_create(loop_row);
    if (s_loop) lv_obj_add_state(loop_sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(loop_sw, on_loop_changed, LV_EVENT_VALUE_CHANGED, NULL);

    /* Status line */
    s_status = lv_label_create(scr);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_status, ui_muted_color(), 0);
    lv_label_set_text(s_status, "");
    lv_obj_align(s_status, LV_ALIGN_BOTTOM_MID, 0, -2);

    ESP_LOGI(TAG, "IR remote screen ready (%u codes)", (unsigned)ir_db_count());
}
