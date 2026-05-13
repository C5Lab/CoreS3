#include "bluetooth_screen.h"
#include "home_screen.h"
#include "ui_helpers.h"
#include "uart_handler.h"
#include "cardkb.h"
#include "psram_dynarr.h"
#include "parse_worker.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static const char *TAG = "bluetooth";

/* ================================================================== */
/*  Forward declarations                                               */
/* ================================================================== */

static void show_bt_menu(void);
static void show_airtag_scan(void);
static void show_bt_locator_scanning(void);

/* ================================================================== */
/*  Navigation                                                         */
/* ================================================================== */

static void on_back_home(lv_event_t *e)
{
    (void)e;
    if (!ui_display_lock_wait()) return;
    show_home_screen();
    ui_display_unlock_safe();
}

static void on_back_bt_menu(lv_event_t *e)
{
    (void)e;
    uart_set_line_callback(NULL);
    uart_send_command("stop");
    if (!ui_display_lock_wait()) return;
    show_bt_menu();
    ui_display_unlock_safe();
}

/* ================================================================== */
/*  AirTag Scan                                                        */
/* ================================================================== */

static lv_obj_t *airtag_count_lbl = NULL;
static lv_obj_t *smarttag_count_lbl = NULL;
static lv_timer_t *airtag_kb_timer = NULL;

static void stop_airtag_kb_timer(void)
{
    if (airtag_kb_timer) {
        lv_timer_del(airtag_kb_timer);
        airtag_kb_timer = NULL;
    }
}

static void airtag_line_cb(const char *line)
{
    /* Parse: "N,M" where N=airtag_count, M=smarttag_count */
    int at = 0, st = 0;
    if (sscanf(line, "%d,%d", &at, &st) != 2) return;

    if (!ui_display_lock_wait()) return;
    if (airtag_count_lbl) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", at);
        lv_label_set_text(airtag_count_lbl, buf);
    }
    if (smarttag_count_lbl) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", st);
        lv_label_set_text(smarttag_count_lbl, buf);
    }
    ui_display_unlock_safe();
}

static void poll_airtag_kb(lv_timer_t *t)
{
    (void)t;
    uint8_t key = cardkb_read_key();
    if (key == 0) return;

    /* Esc (0x1B) or Backspace (0x08 / 0x7F) -> stop & go back */
    if (key == 0x1B || key == 0x08 || key == 0x7F) {
        stop_airtag_kb_timer();
        uart_set_line_callback(NULL);
        uart_send_command("stop");
        show_bt_menu();
    }
}

static void show_airtag_scan(void)
{
    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "AirTag Scan", on_back_bt_menu, NULL);

    lv_obj_t *center = lv_obj_create(scr);
    lv_obj_set_size(center, LV_PCT(100), 240 - 36);
    lv_obj_set_pos(center, 0, 36);
    lv_obj_set_style_bg_opa(center, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(center, 0, 0);
    lv_obj_set_flex_flow(center, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(center, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(center, LV_OBJ_FLAG_SCROLLABLE);

    /* AirTags column */
    lv_obj_t *at_col = lv_obj_create(center);
    lv_obj_set_size(at_col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(at_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(at_col, 0, 0);
    lv_obj_set_style_pad_all(at_col, 0, 0);
    lv_obj_set_flex_flow(at_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(at_col, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(at_col, 4, 0);
    lv_obj_clear_flag(at_col, LV_OBJ_FLAG_SCROLLABLE);

    airtag_count_lbl = lv_label_create(at_col);
    lv_label_set_text(airtag_count_lbl, "0");
    lv_obj_set_style_text_color(airtag_count_lbl, UI_ACCENT_CYAN, 0);
    lv_obj_set_style_text_font(airtag_count_lbl, &lv_font_montserrat_20, 0);

    lv_obj_t *at_label = lv_label_create(at_col);
    lv_label_set_text(at_label, "AirTags");
    lv_obj_set_style_text_color(at_label, ui_text_color(), 0);
    lv_obj_set_style_text_font(at_label, &lv_font_montserrat_14, 0);

    /* SmartTags column */
    lv_obj_t *st_col = lv_obj_create(center);
    lv_obj_set_size(st_col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(st_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(st_col, 0, 0);
    lv_obj_set_style_pad_all(st_col, 0, 0);
    lv_obj_set_flex_flow(st_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(st_col, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(st_col, 4, 0);
    lv_obj_clear_flag(st_col, LV_OBJ_FLAG_SCROLLABLE);

    smarttag_count_lbl = lv_label_create(st_col);
    lv_label_set_text(smarttag_count_lbl, "0");
    lv_obj_set_style_text_color(smarttag_count_lbl, UI_ACCENT_PURPLE, 0);
    lv_obj_set_style_text_font(smarttag_count_lbl, &lv_font_montserrat_20, 0);

    lv_obj_t *st_label = lv_label_create(st_col);
    lv_label_set_text(st_label, "SmartTags");
    lv_obj_set_style_text_color(st_label, ui_text_color(), 0);
    lv_obj_set_style_text_font(st_label, &lv_font_montserrat_14, 0);

    /* Start UART monitoring + keyboard polling */
    uart_set_line_callback(airtag_line_cb);
    uart_send_command("scan_airtag");

    airtag_kb_timer = lv_timer_create(poll_airtag_kb, 50, NULL);
}

/* ================================================================== */
/*  BT Locator: scan_bt (one-shot device list)                        */
/* ================================================================== */

#define BT_DEV_HARD_CAP 1024

typedef struct {
    int  index;
    char mac[18];
    int  rssi;
    char name[64];
} bt_device_t;

static bt_device_t *bt_devices;
static int          bt_devices_cap;
static int bt_device_count = 0;
static int bt_total = 0;
static int bt_airtags = 0;
static int bt_smarttags = 0;

#define BT_ROWS_PER_TICK 5
#define BT_BUILD_TIMER_MS 28

static lv_obj_t *bt_list_container = NULL;
static lv_timer_t *bt_build_timer = NULL;
static int bt_build_idx;

static void stop_bt_build_timer(void)
{
    if (bt_build_timer) {
        lv_timer_delete(bt_build_timer);
        bt_build_timer = NULL;
    }
}

static void on_bt_device_clicked(lv_event_t *e);

static void bt_build_one_device_row(int i)
{
    if (!bt_list_container || i < 0 || i >= bt_device_count)
        return;

    lv_obj_t *btn = lv_btn_create(bt_list_container);
    lv_obj_set_size(btn, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(btn, ui_card_color(), 0);
    lv_obj_set_style_bg_color(btn, UI_ACCENT_PURPLE, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_pad_all(btn, 4, 0);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn, 6, 0);
    lv_obj_add_event_cb(btn, on_bt_device_clicked, LV_EVENT_CLICKED,
                        (void *)(intptr_t)i);

    lv_obj_t *rssi_lbl = lv_label_create(btn);
    char rssi_txt[10];
    snprintf(rssi_txt, sizeof(rssi_txt), "%d", bt_devices[i].rssi);
    lv_label_set_text(rssi_lbl, rssi_txt);
    lv_obj_set_style_text_font(rssi_lbl, &lv_font_montserrat_10, 0);
    lv_obj_set_width(rssi_lbl, 30);

    lv_color_t rssi_color;
    if (bt_devices[i].rssi > -50)       rssi_color = UI_ACCENT_GREEN;
    else if (bt_devices[i].rssi > -70)  rssi_color = UI_ACCENT_ORANGE;
    else                                rssi_color = UI_ACCENT_RED;
    lv_obj_set_style_text_color(rssi_lbl, rssi_color, 0);

    lv_obj_t *info = lv_obj_create(btn);
    lv_obj_set_size(info, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(info, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(info, 0, 0);
    lv_obj_set_style_pad_all(info, 0, 0);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(info, 1);
    lv_obj_clear_flag(info, LV_OBJ_FLAG_SCROLLABLE);

    if (bt_devices[i].name[0]) {
        lv_obj_t *name_l = lv_label_create(info);
        lv_label_set_text(name_l, bt_devices[i].name);
        lv_obj_set_style_text_color(name_l, ui_text_color(), 0);
        lv_obj_set_style_text_font(name_l, &lv_font_montserrat_10, 0);
        lv_obj_set_width(name_l, LV_PCT(100));
        lv_label_set_long_mode(name_l, LV_LABEL_LONG_DOT);
    }

    lv_obj_t *mac_l = lv_label_create(info);
    lv_label_set_text(mac_l, bt_devices[i].mac);
    lv_obj_set_style_text_color(mac_l, ui_muted_color(), 0);
    lv_obj_set_style_text_font(mac_l, &lv_font_montserrat_10, 0);
}

static void bt_build_step(lv_timer_t *t)
{
    (void)t;
    if (!bt_list_container) {
        stop_bt_build_timer();
        return;
    }
    int target = bt_build_idx + BT_ROWS_PER_TICK;
    if (target > bt_device_count)
        target = bt_device_count;
    for (; bt_build_idx < target; bt_build_idx++)
        bt_build_one_device_row(bt_build_idx);
    if (bt_build_idx >= bt_device_count)
        stop_bt_build_timer();
}

static lv_obj_t *track_rssi_lbl = NULL;
static lv_obj_t *track_name_lbl = NULL;
static lv_timer_t *track_kb_timer = NULL;

static void stop_track_kb_timer(void)
{
    if (track_kb_timer) {
        lv_timer_del(track_kb_timer);
        track_kb_timer = NULL;
    }
}

static void on_back_bt_list(lv_event_t *e);
static void show_bt_device_list(void);

static void track_line_cb(const char *line)
{
    /* Parse: "MAC  RSSI: -NN dBm  Name: ..." or "MAC  RSSI: -NN dBm" */
    const char *rssi_p = strstr(line, "RSSI:");
    if (!rssi_p) return;

    int rssi = atoi(rssi_p + 5);
    char name[64] = {0};

    const char *name_p = strstr(line, "Name:");
    if (name_p) {
        name_p += 5;
        while (*name_p == ' ') name_p++;
        snprintf(name, sizeof(name), "%s", name_p);
        int len = strlen(name);
        while (len > 0 && (name[len - 1] == ' ' || name[len - 1] == '\r'))
            name[--len] = '\0';
    }

    if (!ui_display_lock_wait()) return;
    if (track_rssi_lbl) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d dBm", rssi);
        lv_label_set_text(track_rssi_lbl, buf);

        lv_color_t color;
        if (rssi > -50)       color = UI_ACCENT_GREEN;
        else if (rssi > -70)  color = UI_ACCENT_ORANGE;
        else                  color = UI_ACCENT_RED;
        lv_obj_set_style_text_color(track_rssi_lbl, color, 0);
    }
    if (track_name_lbl && name[0]) {
        lv_label_set_text(track_name_lbl, name);
    }
    ui_display_unlock_safe();
}

static void poll_track_kb(lv_timer_t *t)
{
    (void)t;
    uint8_t key = cardkb_read_key();
    if (key == 0) return;

    if (key == 0x1B || key == 0x08 || key == 0x7F) {
        stop_track_kb_timer();
        uart_set_line_callback(NULL);
        uart_send_command("stop");
        show_bt_device_list();
    }
}

static void show_tracking_screen(int idx)
{
    bt_device_t *dev = &bt_devices[idx];
    const char *display_name = dev->name[0] ? dev->name : dev->mac;

    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "BT Locator", on_back_bt_list, NULL);

    lv_obj_t *center = lv_obj_create(scr);
    lv_obj_set_size(center, LV_PCT(100), 240 - 36);
    lv_obj_set_pos(center, 0, 36);
    lv_obj_set_style_bg_opa(center, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(center, 0, 0);
    lv_obj_set_flex_flow(center, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(center, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(center, 10, 0);
    lv_obj_clear_flag(center, LV_OBJ_FLAG_SCROLLABLE);

    track_name_lbl = lv_label_create(center);
    lv_label_set_text(track_name_lbl, display_name);
    lv_obj_set_style_text_color(track_name_lbl, UI_ACCENT_CYAN, 0);
    lv_obj_set_style_text_font(track_name_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(track_name_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(track_name_lbl, LV_PCT(90));
    lv_label_set_long_mode(track_name_lbl, LV_LABEL_LONG_DOT);

    lv_obj_t *mac_lbl = lv_label_create(center);
    lv_label_set_text(mac_lbl, dev->mac);
    lv_obj_set_style_text_color(mac_lbl, ui_muted_color(), 0);
    lv_obj_set_style_text_font(mac_lbl, &lv_font_montserrat_10, 0);

    track_rssi_lbl = lv_label_create(center);
    lv_label_set_text(track_rssi_lbl, "...");
    lv_obj_set_style_text_color(track_rssi_lbl, UI_ACCENT_GREEN, 0);
    lv_obj_set_style_text_font(track_rssi_lbl, &lv_font_montserrat_20, 0);

    lv_obj_t *rssi_caption = lv_label_create(center);
    lv_label_set_text(rssi_caption, "RSSI");
    lv_obj_set_style_text_color(rssi_caption, ui_muted_color(), 0);
    lv_obj_set_style_text_font(rssi_caption, &lv_font_montserrat_12, 0);

    char cmd[40];
    snprintf(cmd, sizeof(cmd), "scan_bt %s", dev->mac);
    uart_set_line_callback(track_line_cb);
    uart_send_command(cmd);

    track_kb_timer = lv_timer_create(poll_track_kb, 50, NULL);
}

static void on_back_bt_list(lv_event_t *e)
{
    (void)e;
    stop_track_kb_timer();
    uart_set_line_callback(NULL);
    uart_send_command("stop");
    if (!ui_display_lock_wait()) return;
    show_bt_device_list();
    ui_display_unlock_safe();
}

/* ---- Device list click ---- */

static void on_bt_device_clicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= bt_device_count) return;
    ESP_LOGI(TAG, "Track device: %s (%s)", bt_devices[idx].mac, bt_devices[idx].name);
    show_tracking_screen(idx);
}

/* ---- Build device list UI ---- */

static lv_timer_t *locator_kb_timer = NULL;

static void stop_locator_kb_timer(void)
{
    if (locator_kb_timer) {
        lv_timer_del(locator_kb_timer);
        locator_kb_timer = NULL;
    }
}

static void poll_locator_kb(lv_timer_t *t)
{
    (void)t;
    uint8_t key = cardkb_read_key();
    if (key == 0) return;

    if (key == 0x1B || key == 0x08 || key == 0x7F) {
        stop_locator_kb_timer();
        show_bt_menu();
    }
}

static void show_bt_device_list(void)
{
    stop_track_kb_timer();
    stop_locator_kb_timer();
    stop_bt_build_timer();
    bt_list_container = NULL;

    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "BT Locator", on_back_bt_menu, NULL);

    if (bt_device_count == 0) {
        lv_obj_t *lbl = lv_label_create(scr);
        lv_label_set_text(lbl, "No devices found.");
        lv_obj_set_style_text_color(lbl, ui_muted_color(), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_center(lbl);
        locator_kb_timer = lv_timer_create(poll_locator_kb, 50, NULL);
        return;
    }

    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_set_size(list, LV_PCT(100), 240 - 36);
    lv_obj_set_pos(list, 0, 36);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 4, 0);
    lv_obj_set_style_pad_row(list, 2, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    bt_list_container = list;

    lv_obj_t *summary = lv_obj_create(list);
    lv_obj_set_size(summary, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(summary, ui_card_color(), 0);
    lv_obj_set_style_bg_opa(summary, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(summary, 6, 0);
    lv_obj_set_style_border_width(summary, 0, 0);
    lv_obj_set_style_pad_all(summary, 4, 0);
    lv_obj_clear_flag(summary, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *sum_lbl = lv_label_create(summary);
    char sum_txt[80];
    snprintf(sum_txt, sizeof(sum_txt), "%d devices  |  %d AirTags  |  %d SmartTags",
             bt_total, bt_airtags, bt_smarttags);
    lv_label_set_text(sum_lbl, sum_txt);
    lv_obj_set_style_text_color(sum_lbl, UI_ACCENT_CYAN, 0);
    lv_obj_set_style_text_font(sum_lbl, &lv_font_montserrat_10, 0);
    lv_obj_set_width(sum_lbl, LV_PCT(100));
    lv_obj_set_style_text_align(sum_lbl, LV_TEXT_ALIGN_CENTER, 0);

    bt_build_idx = 0;
    bt_build_timer = lv_timer_create(bt_build_step, BT_BUILD_TIMER_MS, NULL);

    locator_kb_timer = lv_timer_create(poll_locator_kb, 50, NULL);
}

static void bt_apply_scan_lvgl(void *unused)
{
    (void)unused;
    show_bt_device_list();
}

static void bt_lvgl_dispatch_job(void *unused)
{
    (void)unused;
    if (!ui_lvgl_async_call(bt_apply_scan_lvgl, NULL))
        ESP_LOGW(TAG, "failed to schedule BT device list UI");
}

/* ---- Parse scan_bt output (collected) ---- */

static bool parse_bt_device_line(const char *line, bt_device_t *dev)
{
    const char *p = line;
    while (*p == ' ') p++;
    if (!isdigit((unsigned char)*p)) return false;

    dev->index = atoi(p);
    while (isdigit((unsigned char)*p)) p++;
    if (*p == '.') p++;
    while (*p == ' ') p++;

    if (strlen(p) < 17) return false;
    memcpy(dev->mac, p, 17);
    dev->mac[17] = '\0';
    p += 17;

    const char *rssi_p = strstr(p, "RSSI:");
    if (!rssi_p) return false;
    dev->rssi = atoi(rssi_p + 5);

    dev->name[0] = '\0';
    const char *name_p = strstr(p, "Name:");
    if (name_p) {
        name_p += 5;
        while (*name_p == ' ') name_p++;
        snprintf(dev->name, sizeof(dev->name), "%s", name_p);
        int len = strlen(dev->name);
        while (len > 0 && (dev->name[len - 1] == ' ' || dev->name[len - 1] == '\r'))
            dev->name[--len] = '\0';
    }
    return true;
}

static void bt_scan_complete(const char **lines, int line_count)
{
    bt_device_count = 0;
    bt_total = 0;
    bt_airtags = 0;
    bt_smarttags = 0;

    for (int i = 0; i < line_count; i++) {
        const char *line = lines[i];

        if (strstr(line, "Summary:")) {
            sscanf(strstr(line, "Summary:") + 8, "%d AirTags, %d SmartTags, %d total",
                   &bt_airtags, &bt_smarttags, &bt_total);
            continue;
        }

        bt_device_t dev;
        if (!parse_bt_device_line(line, &dev)) continue;
        if (!psram_dynarr_ensure((void **)&bt_devices, &bt_devices_cap,
                                 bt_device_count + 1, sizeof(*bt_devices),
                                 BT_DEV_HARD_CAP)) {
            ESP_LOGW(TAG, "BT cap reached at %d, dropping rest", bt_device_count);
            continue;
        }
        bt_devices[bt_device_count++] = dev;
    }

    if (bt_total == 0) bt_total = bt_device_count;

    ESP_LOGI(TAG, "BT scan: %d devices, %d AirTags, %d SmartTags",
             bt_device_count, bt_airtags, bt_smarttags);

    if (!parse_worker_post(bt_lvgl_dispatch_job, NULL)) {
        if (!ui_lvgl_async_call(bt_apply_scan_lvgl, NULL))
            ESP_LOGE(TAG, "BT UI schedule failed");
    }
}

static void show_bt_locator_scanning(void)
{
    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "BT Locator", on_back_bt_menu, NULL);

    lv_obj_t *center = lv_obj_create(scr);
    lv_obj_set_size(center, LV_PCT(100), 240 - 36);
    lv_obj_set_pos(center, 0, 36);
    lv_obj_set_style_bg_opa(center, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(center, 0, 0);
    lv_obj_set_flex_flow(center, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(center, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(center, 10, 0);
    lv_obj_clear_flag(center, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *spinner = lv_spinner_create(center);
    lv_obj_set_size(spinner, 40, 40);
    lv_spinner_set_anim_params(spinner, 1000, 200);

    lv_obj_t *lbl = lv_label_create(center);
    lv_label_set_text(lbl, "Scanning BLE devices...");
    lv_obj_set_style_text_color(lbl, ui_muted_color(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);

    uart_start_collect("Summary:", bt_scan_complete);
    uart_send_command("scan_bt");
}

/* ================================================================== */
/*  BT Menu tiles                                                      */
/* ================================================================== */

static void on_airtag(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "AirTag Scan selected");
    show_airtag_scan();
}

static void on_locator(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "BT Locator selected");
    show_bt_locator_scanning();
}

static void show_bt_menu(void)
{
    stop_airtag_kb_timer();
    stop_locator_kb_timer();
    stop_track_kb_timer();
    stop_bt_build_timer();

    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Bluetooth", on_back_home, NULL);

    lv_obj_t *grid = lv_obj_create(scr);
    lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(grid, 10, 0);
    lv_obj_set_style_pad_column(grid, 8, 0);
    lv_obj_set_style_pad_top(grid, 10, 0);
    lv_obj_set_style_pad_bottom(grid, 10, 0);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_y(grid, 36);

    ui_create_tile(grid, LV_SYMBOL_WIFI,      "AirTag\nScan",   UI_ACCENT_CYAN,   on_airtag,  NULL);
    ui_create_tile(grid, LV_SYMBOL_BLUETOOTH, "BT\nLocator",    UI_ACCENT_PURPLE, on_locator, NULL);
}

/* ================================================================== */
/*  Public entry point                                                 */
/* ================================================================== */

void show_bluetooth_screen(void)
{
    show_bt_menu();
}
