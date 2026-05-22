#include "subghz_screen.h"
#include "subghz_listen_screen.h"
#include "subghz_manage_screen.h"
#include "subghz_jammer_screen.h"
#include "subghz_tesla_screen.h"
#include "subghz_hunter_screen.h"
#include "subghz_scanner_screen.h"
#include "subghz_weather_screen.h"
#include "subghz_settings_screen.h"
#include "home_screen.h"
#include "ui_helpers.h"
#include "device_info.h"
#include "esp_log.h"

static const char *TAG = "subghz";

static void on_back(lv_event_t *e)
{
    (void)e;
    show_home_screen();
}

static void on_listen(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Listen");
    show_subghz_listen_screen();
}

static void on_manage(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Manage");
    show_subghz_manage_screen();
}

static void on_jammer(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Jammer");
    show_subghz_jammer_screen();
}

static void on_tesla(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Tesla");
    show_subghz_tesla_screen();
}

static void on_hunter(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Hunter");
    show_subghz_hunter_screen();
}

static void on_scanner(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Scanner");
    show_subghz_scanner_screen();
}

static void on_weather(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Weather");
    show_subghz_weather_screen();
}

static void on_settings(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Settings");
    show_subghz_settings_screen();
}

void show_subghz_screen(void)
{
    lv_obj_t *scr = ui_screen_clear();

    ui_create_top_bar(scr, device_info_subghz_title(), on_back, NULL);

    lv_obj_t *grid = lv_obj_create(scr);
    lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(grid, 6, 0);
    lv_obj_set_style_pad_column(grid, 6, 0);
    lv_obj_set_style_pad_top(grid, 6, 0);
    lv_obj_set_style_pad_bottom(grid, 6, 0);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_y(grid, 36);

    /* 8 tiles in 2 columns must fit the 320x240 screen below the 36px top bar.
     * Default tile height (48) overflows; shrink to 38 (~20% smaller) so all
     * rows fit without scrolling. */
    lv_obj_t *tiles[] = {
        ui_create_tile(grid, LV_SYMBOL_REFRESH,  "Quick Scan", UI_ACCENT_TEAL,   on_scanner,  NULL),
        ui_create_tile(grid, LV_SYMBOL_GPS,      "Hunter",     UI_ACCENT_PINK,   on_hunter,   NULL),
        ui_create_tile(grid, LV_SYMBOL_EYE_OPEN, "Listen",     UI_ACCENT_CYAN,   on_listen,   NULL),
        ui_create_tile(grid, LV_SYMBOL_LIST,     "SD Signals", UI_ACCENT_ORANGE, on_manage,   NULL),
        ui_create_tile(grid, LV_SYMBOL_TINT,     "Weather",    UI_ACCENT_BLUE,   on_weather,  NULL),
        ui_create_tile(grid, LV_SYMBOL_WARNING,  "Jammer",     UI_ACCENT_RED,    on_jammer,   NULL),
        ui_create_tile(grid, LV_SYMBOL_POWER,    "Tesla",      UI_ACCENT_PURPLE, on_tesla,    NULL),
        ui_create_tile(grid, LV_SYMBOL_SETTINGS, "Settings",   ui_muted_color(), on_settings, NULL),
    };
    for (size_t i = 0; i < sizeof(tiles) / sizeof(tiles[0]); i++)
        lv_obj_set_height(tiles[i], 38);
}
