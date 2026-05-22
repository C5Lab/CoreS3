#include "subghz_settings_screen.h"
#include "subghz_screen.h"
#include "ui_helpers.h"
#include "uart_handler.h"
#include "cardkb.h"
#include "esp_log.h"
#include "bsp/m5stack_core_s3.h"
#include "freertos/FreeRTOS.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "sg_settings";

static lv_obj_t *s_rollers[4];          /* [0]=sign, [1]=int, [2]=dec1, [3]=dec2 */
static lv_obj_t *s_status_lbl;
static lv_obj_t *s_set_btn;
static lv_timer_t *s_ui_timer;
static lv_timer_t *s_kb_timer;
static lv_timer_t *s_status_clear_timer;

static volatile bool s_correction_pending;
static float         s_correction_loaded;
static portMUX_TYPE  s_state_lock = portMUX_INITIALIZER_UNLOCKED;

static const char *s_sign_opts   = "+\n-";
static const char *s_int_opts    = "0\n1\n2\n3\n4\n5";
static const char *s_digit_opts  = "0\n1\n2\n3\n4\n5\n6\n7\n8\n9";

static void on_back(lv_event_t *e);
static void on_set(lv_event_t *e);
static void line_cb(const char *line);
static void ui_tick_cb(lv_timer_t *t);
static void kb_poll_cb(lv_timer_t *t);
static void settings_teardown(void);

static void decompose(float v, int *sign_idx, int d[3])
{
    int centi = (int)lroundf(fabsf(v) * 100.0f);
    if (centi > 500) centi = 500;
    *sign_idx = (v < 0.0f) ? 1 : 0;
    d[0] = (centi / 100) % 10;
    d[1] = (centi / 10) % 10;
    d[2] = centi % 10;
}

static float compose_from_rollers(void)
{
    int sign_idx = (int)lv_roller_get_selected(s_rollers[0]);
    int d0 = (int)lv_roller_get_selected(s_rollers[1]);
    int d1 = (int)lv_roller_get_selected(s_rollers[2]);
    int d2 = (int)lv_roller_get_selected(s_rollers[3]);
    float v = d0 + d1 * 0.1f + d2 * 0.01f;
    if (v > 5.0f) v = 5.0f;
    return sign_idx ? -v : v;
}

static void apply_value_to_rollers(float v)
{
    int sign_idx;
    int d[3];
    decompose(v, &sign_idx, d);
    if (s_rollers[0]) lv_roller_set_selected(s_rollers[0], sign_idx, LV_ANIM_OFF);
    if (s_rollers[1]) lv_roller_set_selected(s_rollers[1], d[0], LV_ANIM_OFF);
    if (s_rollers[2]) lv_roller_set_selected(s_rollers[2], d[1], LV_ANIM_OFF);
    if (s_rollers[3]) lv_roller_set_selected(s_rollers[3], d[2], LV_ANIM_OFF);
}

static void set_status_label(const char *text, lv_color_t color)
{
    if (!s_status_lbl) return;
    lv_label_set_text(s_status_lbl, text);
    lv_obj_set_style_text_color(s_status_lbl, color, 0);
}

static void status_clear_timer_cb(lv_timer_t *t)
{
    (void)t;
    s_status_clear_timer = NULL;
    set_status_label("", ui_muted_color());
}

static void schedule_status_clear(uint32_t ms)
{
    if (s_status_clear_timer) {
        lv_timer_delete(s_status_clear_timer);
        s_status_clear_timer = NULL;
    }
    s_status_clear_timer = lv_timer_create(status_clear_timer_cb, ms, NULL);
    lv_timer_set_repeat_count(s_status_clear_timer, 1);
}

static void line_cb(const char *line)
{
    if (!line) return;
    const char *tag = strstr(line, "[SUBGHZ_FREQ_CORRECTION]");
    if (!tag) return;

    const char *p = tag + strlen("[SUBGHZ_FREQ_CORRECTION]");
    while (*p == ' ') p++;

    float v = strtof(p, NULL);
    if (v >  5.0f) v =  5.0f;
    if (v < -5.0f) v = -5.0f;

    portENTER_CRITICAL(&s_state_lock);
    s_correction_loaded = v;
    s_correction_pending = true;
    portEXIT_CRITICAL(&s_state_lock);
}

static void ui_tick_cb(lv_timer_t *t)
{
    (void)t;
    bool pending;
    float v;

    portENTER_CRITICAL(&s_state_lock);
    pending = s_correction_pending;
    v = s_correction_loaded;
    s_correction_pending = false;
    portEXIT_CRITICAL(&s_state_lock);

    if (!pending) return;

    bsp_display_lock(0);
    apply_value_to_rollers(v);
    char msg[40];
    snprintf(msg, sizeof(msg), "Loaded %+.2f MHz", v);
    set_status_label(msg, UI_ACCENT_CYAN);
    schedule_status_clear(2500);
    if (s_set_btn) lv_obj_clear_state(s_set_btn, LV_STATE_DISABLED);
    bsp_display_unlock();
}

static void kb_poll_cb(lv_timer_t *t)
{
    (void)t;
    uint8_t key = cardkb_read_key();
    if (key == 0) return;
    if (key == 0x1B || key == 0x08 || key == 0x7F)
        on_back(NULL);
}

static void on_set(lv_event_t *e)
{
    (void)e;
    float v = compose_from_rollers();

    char cmd[40];
    snprintf(cmd, sizeof(cmd), "subghz_set_freq_correction %+.2f", v);
    uart_send_command(cmd);

    char msg[40];
    snprintf(msg, sizeof(msg), "Saved %+.2f MHz", v);
    set_status_label(msg, UI_ACCENT_GREEN);
    schedule_status_clear(2500);

    ESP_LOGI(TAG, "Set freq correction %+.2f MHz", v);
}

static void settings_teardown(void)
{
    uart_set_line_callback(NULL);
    if (s_ui_timer) { lv_timer_delete(s_ui_timer); s_ui_timer = NULL; }
    if (s_kb_timer) { lv_timer_delete(s_kb_timer); s_kb_timer = NULL; }
    if (s_status_clear_timer) {
        lv_timer_delete(s_status_clear_timer);
        s_status_clear_timer = NULL;
    }
    portENTER_CRITICAL(&s_state_lock);
    s_correction_pending = false;
    portEXIT_CRITICAL(&s_state_lock);
    for (int i = 0; i < 4; i++) s_rollers[i] = NULL;
    s_status_lbl = NULL;
    s_set_btn = NULL;
}

static void on_back(lv_event_t *e)
{
    (void)e;
    settings_teardown();
    show_subghz_screen();
}

static void style_roller(lv_obj_t *r, lv_coord_t width)
{
    lv_obj_set_width(r, width);
    lv_obj_set_style_bg_color(r, ui_card_color(), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(r, ui_text_color(), 0);
    lv_obj_set_style_text_font(r, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(r, UI_ACCENT_CYAN, LV_PART_SELECTED);
    lv_obj_set_style_bg_color(r, ui_panel_color(), LV_PART_SELECTED);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_radius(r, 6, 0);
}

void show_subghz_settings_screen(void)
{
    for (int i = 0; i < 4; i++) s_rollers[i] = NULL;
    s_status_lbl = NULL;
    s_set_btn = NULL;
    s_ui_timer = NULL;
    s_kb_timer = NULL;
    s_status_clear_timer = NULL;
    s_correction_pending = false;
    s_correction_loaded  = 0.0f;

    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "SubGHz Settings", on_back, NULL);

    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_set_size(card, LV_PCT(92), LV_SIZE_CONTENT);
    lv_obj_set_y(card, 42);
    lv_obj_set_x(card, (320 - (320 * 92 / 100)) / 2);
    lv_obj_set_style_bg_color(card, ui_card_color(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, ui_border_color(), 0);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_set_style_pad_row(card, 6, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Freq Correction (MHz)");
    lv_obj_set_style_text_color(title, ui_text_color(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);

    lv_obj_t *roller_row = lv_obj_create(card);
    lv_obj_set_size(roller_row, LV_PCT(100), 76);
    lv_obj_set_flex_flow(roller_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(roller_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(roller_row, 0, 0);
    lv_obj_set_style_pad_gap(roller_row, 2, 0);
    lv_obj_set_style_bg_opa(roller_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(roller_row, 0, 0);
    lv_obj_clear_flag(roller_row, LV_OBJ_FLAG_SCROLLABLE);

    s_rollers[0] = lv_roller_create(roller_row);
    lv_roller_set_options(s_rollers[0], s_sign_opts, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(s_rollers[0], 3);
    style_roller(s_rollers[0], 30);

    s_rollers[1] = lv_roller_create(roller_row);
    lv_roller_set_options(s_rollers[1], s_int_opts, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(s_rollers[1], 3);
    style_roller(s_rollers[1], 36);

    lv_obj_t *dot = lv_label_create(roller_row);
    lv_label_set_text(dot, ".");
    lv_obj_set_style_text_font(dot, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(dot, ui_text_color(), 0);

    s_rollers[2] = lv_roller_create(roller_row);
    lv_roller_set_options(s_rollers[2], s_digit_opts, LV_ROLLER_MODE_INFINITE);
    lv_roller_set_visible_row_count(s_rollers[2], 3);
    style_roller(s_rollers[2], 36);

    s_rollers[3] = lv_roller_create(roller_row);
    lv_roller_set_options(s_rollers[3], s_digit_opts, LV_ROLLER_MODE_INFINITE);
    lv_roller_set_visible_row_count(s_rollers[3], 3);
    style_roller(s_rollers[3], 36);

    apply_value_to_rollers(0.0f);

    s_set_btn = lv_btn_create(card);
    lv_obj_set_size(s_set_btn, 110, 30);
    lv_obj_set_style_bg_color(s_set_btn, UI_ACCENT_GREEN, 0);
    lv_obj_set_style_radius(s_set_btn, 6, 0);
    lv_obj_add_event_cb(s_set_btn, on_set, LV_EVENT_CLICKED, NULL);
    lv_obj_add_state(s_set_btn, LV_STATE_DISABLED);
    lv_obj_t *sl = lv_label_create(s_set_btn);
    lv_label_set_text(sl, "Set");
    lv_obj_set_style_text_color(sl, lv_color_white(), 0);
    lv_obj_set_style_text_font(sl, &lv_font_montserrat_14, 0);
    lv_obj_center(sl);

    s_status_lbl = lv_label_create(card);
    lv_label_set_text(s_status_lbl, "Loading...");
    lv_obj_set_style_text_color(s_status_lbl, ui_muted_color(), 0);
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_10, 0);

    uart_set_line_callback(line_cb);
    uart_send_command("subghz_get_freq_correction");

    s_ui_timer = lv_timer_create(ui_tick_cb, 100, NULL);
    s_kb_timer = lv_timer_create(kb_poll_cb, 50, NULL);

    ESP_LOGI(TAG, "SubGHz Settings screen ready");
}
