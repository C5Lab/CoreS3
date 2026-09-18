#include "nfc_screen.h"
#include "nfc_read_screen.h"
#include "nfc_list_screen.h"
#include "nfc_parser.h"
#include "home_screen.h"
#include "ui_helpers.h"
#include "uart_handler.h"
#include "device_info.h"
#include "esp_log.h"
#include <stddef.h>
#include <string.h>

static const char *TAG = "nfc";

#define NFC_INIT_UI_TIMEOUT_MS 3000

static lv_obj_t   *s_status_lbl;
static lv_obj_t   *s_read_tile;
static lv_obj_t   *s_list_tile;
static lv_timer_t *s_timeout_timer;
static bool        s_busy;
static bool        s_nfc_ready;

static void on_back(lv_event_t *e);
static void on_read(lv_event_t *e);
static void on_list(lv_event_t *e);
static void set_read_enabled(bool en);
static void stop_timeout(void);
static void apply_init_result(bool detected);
static void on_init_collected(const char **lines, int count);
static void init_timeout_cb(lv_timer_t *t);
static void start_init_probe(void);

static void stop_timeout(void)
{
    if (s_timeout_timer) {
        lv_timer_delete(s_timeout_timer);
        s_timeout_timer = NULL;
    }
}

static void set_read_enabled(bool en)
{
    if (!s_read_tile) return;
    if (en)
        lv_obj_clear_state(s_read_tile, LV_STATE_DISABLED);
    else
        lv_obj_add_state(s_read_tile, LV_STATE_DISABLED);
}

static void set_list_enabled(bool en)
{
    if (!s_list_tile) return;
    if (en)
        lv_obj_clear_state(s_list_tile, LV_STATE_DISABLED);
    else
        lv_obj_add_state(s_list_tile, LV_STATE_DISABLED);
}

static void apply_init_result(bool detected)
{
    s_busy = false;
    s_nfc_ready = detected;
    device_info_set_has_nfc(detected);
    stop_timeout();
    uart_stop_collect();
    set_read_enabled(detected);
    set_list_enabled(true);

    if (!s_status_lbl) return;
    if (detected) {
        lv_label_set_text(s_status_lbl, "NFC ready");
        lv_obj_set_style_text_color(s_status_lbl, UI_ACCENT_GREEN, 0);
    } else {
        lv_label_set_text(s_status_lbl, "NFC not detected");
        lv_obj_set_style_text_color(s_status_lbl, UI_ACCENT_RED, 0);
    }
}

static void apply_init_async(void *user_data)
{
    if (!s_busy) return;
    apply_init_result(user_data != NULL);
}

static void on_init_collected(const char **lines, int count)
{
    nfc_ui_card_t card;
    nfc_card_reset(&card);

    for (int i = 0; i < count; i++) {
        if (!lines[i]) continue;
        nfc_parse_card_line(lines[i], &card);
    }

    bool detected = card.detected && !card.not_detected;
    if (!ui_lvgl_async_call(apply_init_async, detected ? (void *)1 : NULL))
        ESP_LOGW(TAG, "init UI schedule failed");
}

static void init_timeout_cb(lv_timer_t *t)
{
    (void)t;
    s_timeout_timer = NULL;
    if (!s_busy) return;
    ESP_LOGW(TAG, "init_nfc UI timeout");
    apply_init_result(false);
}

static void start_init_probe(void)
{
    s_busy = true;
    s_nfc_ready = false;
    set_read_enabled(false);
    set_list_enabled(false);
    if (s_status_lbl) {
        lv_label_set_text(s_status_lbl, "Probing...");
        lv_obj_set_style_text_color(s_status_lbl, ui_muted_color(), 0);
    }

    stop_timeout();
    s_timeout_timer = lv_timer_create(init_timeout_cb, NFC_INIT_UI_TIMEOUT_MS, NULL);
    lv_timer_set_repeat_count(s_timeout_timer, 1);

    uart_start_collect("[NFC] END", on_init_collected);
    uart_send_command("init_nfc");
}

static void on_back(lv_event_t *e)
{
    (void)e;
    stop_timeout();
    uart_stop_collect();
    s_status_lbl = NULL;
    s_read_tile = NULL;
    s_list_tile = NULL;
    s_busy = false;
    show_home_screen();
}

static void on_read(lv_event_t *e)
{
    (void)e;
    if (s_busy || !s_nfc_ready) return;
    ESP_LOGI(TAG, "Read");
    stop_timeout();
    uart_stop_collect();
    s_status_lbl = NULL;
    s_read_tile = NULL;
    s_list_tile = NULL;
    show_nfc_read_screen();
}

static void on_list(lv_event_t *e)
{
    (void)e;
    if (s_busy) return;
    ESP_LOGI(TAG, "List");
    stop_timeout();
    uart_stop_collect();
    s_status_lbl = NULL;
    s_read_tile = NULL;
    s_list_tile = NULL;
    show_nfc_list_screen();
}

void show_nfc_screen(void)
{
    s_status_lbl = NULL;
    s_read_tile = NULL;
    s_list_tile = NULL;
    s_timeout_timer = NULL;
    s_busy = false;
    s_nfc_ready = false;

    lv_obj_t *scr = ui_screen_clear();

    ui_create_top_bar(scr, "NFC", on_back, NULL);

    s_status_lbl = lv_label_create(scr);
    lv_obj_set_width(s_status_lbl, 300);
    lv_obj_align(s_status_lbl, LV_ALIGN_TOP_MID, 0, 38);
    lv_obj_set_style_text_align(s_status_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_status_lbl, ui_muted_color(), 0);
    lv_label_set_text(s_status_lbl, "Probing...");

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
    lv_obj_set_y(grid, 54);

    s_read_tile = ui_create_tile(grid, LV_SYMBOL_DOWNLOAD, "Read",
                                 UI_ACCENT_CYAN, on_read, NULL);
    s_list_tile = ui_create_tile(grid, LV_SYMBOL_LIST, "List",
                                 UI_ACCENT_ORANGE, on_list, NULL);
    lv_obj_set_height(s_read_tile, 48);
    lv_obj_set_height(s_list_tile, 48);
    set_read_enabled(false);
    set_list_enabled(false);

    start_init_probe();
    ESP_LOGI(TAG, "NFC hub ready");
}
