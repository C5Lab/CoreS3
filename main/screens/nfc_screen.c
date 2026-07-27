#include "nfc_screen.h"
#include "nfc_read_screen.h"
#include "nfc_list_screen.h"
#include "home_screen.h"
#include "ui_helpers.h"
#include "esp_log.h"
#include <stddef.h>

static const char *TAG = "nfc";

static void on_back(lv_event_t *e)
{
    (void)e;
    show_home_screen();
}

static void on_read(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Read");
    show_nfc_read_screen();
}

static void on_list(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "List");
    show_nfc_list_screen();
}

void show_nfc_screen(void)
{
    lv_obj_t *scr = ui_screen_clear();

    ui_create_top_bar(scr, "NFC", on_back, NULL);

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

    lv_obj_t *tiles[] = {
        ui_create_tile(grid, LV_SYMBOL_DOWNLOAD, "Read", UI_ACCENT_CYAN,   on_read, NULL),
        ui_create_tile(grid, LV_SYMBOL_LIST,     "List", UI_ACCENT_ORANGE, on_list, NULL),
    };
    for (size_t i = 0; i < sizeof(tiles) / sizeof(tiles[0]); i++)
        lv_obj_set_height(tiles[i], 48);

    ESP_LOGI(TAG, "NFC hub ready");
}
