#include "subghz_weather_screen.h"
#include "subghz_screen.h"
#include "ui_helpers.h"
#include "cardkb.h"
#include "esp_log.h"

static const char *TAG = "subghz_weather";

static lv_timer_t *s_kb_timer;

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
}

void show_subghz_weather_screen(void)
{
    s_kb_timer = NULL;

    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Weather", on_back, NULL);

    lv_obj_t *l = lv_label_create(scr);
    lv_label_set_text(l, "Coming soon");
    lv_obj_set_style_text_color(l, ui_muted_color(), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_center(l);

    s_kb_timer = lv_timer_create(kb_poll_cb, 50, NULL);

    ESP_LOGI(TAG, "SubGHz Weather screen ready (placeholder)");
}
