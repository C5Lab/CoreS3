#include "home_screen.h"
#include "wifi_scan_screen.h"
#include "global_attacks_screen.h"
#include "wardrive_screen.h"
#include "network_observer_screen.h"
#include "compromised_data_screen.h"
#include "bluetooth_screen.h"
#include "deauth_detector_screen.h"
#include "subghz_screen.h"
#include "ir_remote_screen.h"
#include "device_info.h"
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

static void on_subghz(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Sub-GHz");
    show_subghz_screen();
}

static void on_ir(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "IR TV Power");
    show_ir_remote_screen();
}

static void on_wardrive(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Wardrive");
    show_wardrive_screen();
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

    /* Fixed top bar, identical to the Settings screen: it carries the title
     * and, when the feature is enabled, the status-bar "Lock" button on the
     * right (added by ui_create_top_bar itself). No back button on the root. */
    lv_obj_t *top_bar = ui_create_top_bar(scr, "Lab5", NULL, NULL);

    /* Center the title the way the Settings bar does. ui_create_top_bar only
     * centers it when a Back button is present, so do it here for the root
     * (the title is the bar's first child). */
    lv_obj_t *title_lbl = lv_obj_get_child(top_bar, 0);
    if (title_lbl) {
        lv_obj_set_style_text_align(title_lbl, LV_TEXT_ALIGN_CENTER, 0);
    }

    /* Scrollable tile grid below the bar. A fixed height scrolls cleanly;
     * an LV_SIZE_CONTENT box under a fixed bar miscomputes its scroll range. */
    lv_obj_t *grid = lv_obj_create(scr);
    {
        lv_display_t *disp = lv_display_get_default();
        int32_t vres = disp ? (int32_t)lv_display_get_vertical_resolution(disp) : 240;
        int32_t grid_h = vres - 36 - 4;   /* 36 = top bar height */
        if (grid_h < 80) {
            grid_h = 80;
        }
        lv_obj_set_size(grid, LV_PCT(100), grid_h);
    }
    lv_obj_align_to(grid, top_bar, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);
    lv_obj_add_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(grid, 6, 0);
    lv_obj_set_style_pad_column(grid, 6, 0);
    lv_obj_set_style_pad_top(grid, 4, 0);
    lv_obj_set_style_pad_bottom(grid, 4, 0);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);

    ui_create_tile(grid, LV_SYMBOL_WIFI,      "WiFi Scan\n& Attack",        UI_ACCENT_BLUE,           on_wifi_scan,       NULL);
    ui_create_tile(grid, LV_SYMBOL_WARNING,   "Global WiFi\nAttacks",      UI_ACCENT_RED,            on_global_wifi,     NULL);
    ui_create_tile(grid, LV_SYMBOL_EYE_OPEN,  "Network Observer\n& Karma", UI_ACCENT_ORANGE,         on_wifi_sniff,      NULL);
    ui_create_tile(grid, LV_SYMBOL_DOWNLOAD,  "Compromised\nData",         UI_ACCENT_GREEN,          on_compromised,     NULL);
    ui_create_tile(grid, LV_SYMBOL_BLUETOOTH, "Bluetooth",                 UI_ACCENT_PURPLE,         on_bluetooth,       NULL);
    ui_create_tile(grid, LV_SYMBOL_CHARGE,    "Deauth\nDetector",          UI_ACCENT_CYAN,           on_deauth_detector, NULL);
    if (device_info_has_subghz()) {
        ui_create_tile(grid, LV_SYMBOL_BARS,  "Sub-GHz",                   UI_ACCENT_PINK,           on_subghz,          NULL);
    }
    ui_create_tile(grid, LV_SYMBOL_VIDEO,     "IR TV\nPower",              UI_ACCENT_TEAL,           on_ir,              NULL);
    ui_create_tile(grid, LV_SYMBOL_GPS,       "Wardrive",                  UI_ACCENT_TEAL,           on_wardrive,        NULL);
    ui_create_tile(grid, LV_SYMBOL_SETTINGS,  "Settings",                  lv_color_hex(0x607D8B),   on_settings,        NULL);
}
