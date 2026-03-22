#include "home_screen.h"
#include "wifi_scan_screen.h"
#include "global_attacks_screen.h"
#include "network_observer_screen.h"
#include "compromised_data_screen.h"
#include "bluetooth_screen.h"
#include "deauth_detector_screen.h"
#include "ui_helpers.h"
#include "esp_log.h"

static const char *TAG = "home";

/* Tile click handlers ------------------------------------------------- */

static void on_wifi_scan(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "WiFi Scan & Attack");
    show_wifi_scan_screen();
}

static void on_global_wifi(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Global WiFi Attacks");
    show_global_attacks_screen();
}

static void on_wifi_sniff(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Network Observer & Karma");
    show_network_observer_screen();
}

static void on_compromised(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Compromised Data");
    show_compromised_data_screen();
}

static void on_bluetooth(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Bluetooth");
    show_bluetooth_screen();
}

static void on_deauth_detector(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Deauth Detector");
    show_deauth_detector_screen();
}

static void on_settings(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Settings");
    show_settings_screen();
}

/* Build the home screen ---------------------------------------------- */

void show_home_screen(void)
{
    lv_obj_t *scr = ui_screen_clear();

    /* top bar (no back button on home) */
    ui_create_top_bar(scr, "LABORATORIUM", NULL, NULL);

    /* scrollable tile grid */
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
    lv_obj_align_to(grid, lv_obj_get_child(scr, 0), LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
    lv_obj_set_y(grid, 36);

    ui_create_tile(grid, LV_SYMBOL_WIFI,      "WiFi Scan\n& Attack",        UI_ACCENT_BLUE,           on_wifi_scan,       NULL);
    ui_create_tile(grid, LV_SYMBOL_WARNING,   "Global WiFi\nAttacks",      UI_ACCENT_RED,            on_global_wifi,     NULL);
    ui_create_tile(grid, LV_SYMBOL_EYE_OPEN,  "Network Observer\n& Karma", UI_ACCENT_ORANGE,         on_wifi_sniff,      NULL);
    ui_create_tile(grid, LV_SYMBOL_DOWNLOAD,  "Compromised\nData",         UI_ACCENT_GREEN,          on_compromised,     NULL);
    ui_create_tile(grid, LV_SYMBOL_BLUETOOTH, "Bluetooth",                 UI_ACCENT_PURPLE,         on_bluetooth,       NULL);
    ui_create_tile(grid, LV_SYMBOL_CHARGE,    "Deauth\nDetector",          UI_ACCENT_CYAN,           on_deauth_detector, NULL);
    ui_create_tile(grid, LV_SYMBOL_SETTINGS,  "Settings",                  lv_color_hex(0x607D8B),   on_settings,        NULL);
}
