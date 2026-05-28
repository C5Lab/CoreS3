#include "wifi_scan_screen.h"
#include "attack_select_screen.h"
#include "home_screen.h"
#include "inspect_network.h"
#include "ui_helpers.h"
#include "uart_handler.h"
#include "psram_dynarr.h"
#include "parse_worker.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "wifi_scan";

#define WIFI_NET_HARD_CAP MAX_NETWORKS

#define WIFI_ROWS_PER_TICK 5
#define WIFI_BUILD_TIMER_MS 28

static wifi_network_t *networks = NULL;
static int networks_cap = 0;
static int network_count = 0;

/* ---- public accessors ---- */
wifi_network_t *wifi_scan_get_networks(void)   { return networks; }
int  wifi_scan_get_network_count(void)         { return network_count; }

int wifi_scan_get_selected(int *indices, int max_indices)
{
    int count = 0;
    for (int i = 0; i < network_count && count < max_indices; i++)
        if (networks[i].selected)
            indices[count++] = i;
    return count;
}

/* ---- CSV parsing ---- */
static bool parse_network_line(const char *line, wifi_network_t *net)
{
    if (line[0] != '"') return false;

    char temp[512];
    strncpy(temp, line, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = '\0';

    char *fields[10] = {NULL};
    int field_idx = 0;
    char *p = temp;
    while (*p && field_idx < 10) {
        if (*p == '"') {
            p++;
            fields[field_idx++] = p;
            while (*p && *p != '"') p++;
            if (*p == '"') { *p = '\0'; p++; }
            if (*p == ',') p++;
        } else {
            p++;
        }
    }
    if (field_idx < 8) return false;

    net->index = (uint8_t)atoi(fields[0]);
    strncpy(net->ssid, fields[1], sizeof(net->ssid) - 1);
    net->ssid[sizeof(net->ssid) - 1] = '\0';
    strncpy(net->bssid, fields[3], sizeof(net->bssid) - 1);
    net->bssid[sizeof(net->bssid) - 1] = '\0';
    net->channel = (uint8_t)atoi(fields[4]);
    strncpy(net->security, fields[5], sizeof(net->security) - 1);
    net->security[sizeof(net->security) - 1] = '\0';
    net->rssi = (int8_t)atoi(fields[6]);
    strncpy(net->band, fields[7], sizeof(net->band) - 1);
    net->band[sizeof(net->band) - 1] = '\0';

    net->vendor[0] = '\0';
    if (field_idx >= 3 && fields[2] && fields[2][0] != '\0') {
        strncpy(net->vendor, fields[2], sizeof(net->vendor) - 1);
        net->vendor[sizeof(net->vendor) - 1] = '\0';
    } else if (field_idx >= 9 && fields[8] && fields[8][0] != '\0') {
        strncpy(net->vendor, fields[8], sizeof(net->vendor) - 1);
        net->vendor[sizeof(net->vendor) - 1] = '\0';
    }

    net->mfp_capable = false;
    net->mfp_known   = false;
    net->uptime[0]   = '\0';
    net->info_label  = NULL;
    net->selected    = false;
    return true;
}

/* ---- UI references ---- */
static lv_obj_t *scan_spinner = NULL;
static lv_obj_t *scan_status_lbl = NULL;
static lv_obj_t *list_container = NULL;
static lv_timer_t *wifi_build_timer = NULL;
static int wifi_build_idx;

static void stop_wifi_build_timer(void)
{
    if (wifi_build_timer) {
        lv_timer_delete(wifi_build_timer);
        wifi_build_timer = NULL;
    }
}

/* ---- build network list UI ---- */
static void on_back_to_home(lv_event_t *e)
{
    (void)e;
    wifi_inspect_cancel();
    uart_stop_collect();
    stop_wifi_build_timer();
    if (!ui_display_lock_wait()) {
        ESP_LOGW(TAG, "on_back_to_home: lock failed");
        return;
    }
    show_home_screen();
    ui_display_unlock_safe();
}

static void on_checkbox_toggle(lv_event_t *e)
{
    lv_obj_t *cb = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < network_count)
        networks[idx].selected = lv_obj_has_state(cb, LV_STATE_CHECKED);
}

static void on_next_pressed(lv_event_t *e)
{
    (void)e;
    wifi_inspect_cancel();
    int sel = 0;
    for (int i = 0; i < network_count; i++)
        if (networks[i].selected) sel++;
    ESP_LOGI(TAG, "Next pressed, %d networks selected", sel);
    show_attack_select_screen();
}

static void wifi_build_one_row(int i)
{
    wifi_network_t *net = &networks[i];

    lv_obj_t *row = lv_obj_create(list_container);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, ui_card_color(), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 4, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cb = lv_checkbox_create(row);
    lv_checkbox_set_text(cb, "");
    lv_obj_set_style_pad_all(cb, 0, 0);
    lv_obj_add_event_cb(cb, on_checkbox_toggle, LV_EVENT_VALUE_CHANGED,
                        (void *)(intptr_t)i);

    lv_obj_t *col = lv_obj_create(row);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_grow(col, 1);

    const char *name = net->ssid[0] ? net->ssid : "(hidden)";
    lv_obj_t *name_lbl = lv_label_create(col);
    lv_label_set_text(name_lbl, name);
    lv_obj_set_style_text_color(name_lbl, ui_text_color(), 0);
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_12, 0);
    lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name_lbl, LV_PCT(100));

    char info[160];
    if (net->vendor[0])
        snprintf(info, sizeof(info), "%ddBm ch%d %s %s\nMFP ? | Up: ? | %s",
                 net->rssi, net->channel, net->band, net->security, net->vendor);
    else
        snprintf(info, sizeof(info), "%ddBm ch%d %s %s\nMFP ? | Up: ?",
                 net->rssi, net->channel, net->band, net->security);
    lv_obj_t *info_lbl = lv_label_create(col);
    lv_label_set_text(info_lbl, info);
    lv_obj_set_style_text_color(info_lbl, ui_muted_color(), 0);
    lv_obj_set_style_text_font(info_lbl, &lv_font_montserrat_10, 0);
    lv_label_set_long_mode(info_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(info_lbl, LV_PCT(100));
    net->info_label = info_lbl;
}

static void wifi_build_step(lv_timer_t *t)
{
    (void)t;
    if (!list_container) {
        stop_wifi_build_timer();
        return;
    }
    int target = wifi_build_idx + WIFI_ROWS_PER_TICK;
    if (target > network_count)
        target = network_count;
    for (; wifi_build_idx < target; wifi_build_idx++)
        wifi_build_one_row(wifi_build_idx);
    if (wifi_build_idx >= network_count)
        stop_wifi_build_timer();
}

static void wifi_build_shell_and_start_timer(void)
{
    stop_wifi_build_timer();
    list_container = NULL;

    lv_obj_t *scr = ui_screen_clear();

    ui_create_top_bar(scr, "WiFi Networks", on_back_to_home, NULL);

    lv_obj_t *hdr = lv_label_create(scr);
    char buf[48];
    snprintf(buf, sizeof(buf), "Found %d networks", network_count);
    lv_label_set_text(hdr, buf);
    lv_obj_set_style_text_color(hdr, ui_muted_color(), 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(hdr, 8, 40);

    list_container = lv_obj_create(scr);
    lv_obj_set_size(list_container, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_pos(list_container, 0, 56);
    lv_obj_set_height(list_container, 240 - 56 - 40);
    lv_obj_set_style_bg_opa(list_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_container, 0, 0);
    lv_obj_set_style_pad_all(list_container, 4, 0);
    lv_obj_set_style_pad_row(list_container, 2, 0);
    lv_obj_set_flex_flow(list_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(list_container, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *btn_bar = lv_obj_create(scr);
    lv_obj_set_size(btn_bar, LV_PCT(100), 38);
    lv_obj_align(btn_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(btn_bar, ui_panel_color(), 0);
    lv_obj_set_style_bg_opa(btn_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn_bar, 0, 0);
    lv_obj_set_style_border_width(btn_bar, 0, 0);
    lv_obj_set_style_pad_all(btn_bar, 4, 0);
    lv_obj_clear_flag(btn_bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *next_btn = lv_btn_create(btn_bar);
    lv_obj_set_size(next_btn, 120, 30);
    lv_obj_center(next_btn);
    lv_obj_set_style_bg_color(next_btn, UI_ACCENT_GREEN, 0);
    lv_obj_set_style_radius(next_btn, 8, 0);
    lv_obj_add_event_cb(next_btn, on_next_pressed, LV_EVENT_CLICKED, NULL);

    lv_obj_t *next_lbl = lv_label_create(next_btn);
    lv_label_set_text(next_lbl, "Next " LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(next_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(next_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(next_lbl);

    wifi_build_idx = 0;
    if (network_count > 0)
        wifi_build_timer = lv_timer_create(wifi_build_step, WIFI_BUILD_TIMER_MS, NULL);
}

static int s_scan_line_count;

static void wifi_apply_scan_lvgl(void *unused)
{
    (void)unused;
    int line_count = s_scan_line_count;

    if (network_count > 0) {
        wifi_build_shell_and_start_timer();
        wifi_inspect_start(networks, network_count);
    } else {
        lv_obj_t *scr = ui_screen_clear();
        ui_create_top_bar(scr, "WiFi Scan", on_back_to_home, NULL);

        lv_obj_t *msg = lv_label_create(scr);
        lv_label_set_text(msg, line_count == 0
            ? "No response from board.\nCheck UART connection."
            : "No networks parsed.\nUnexpected data format.");
        lv_obj_set_style_text_color(msg, ui_text_color(), 0);
        lv_obj_set_style_text_font(msg, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(msg, LV_ALIGN_CENTER, 0, 0);
    }
}

static void wifi_lvgl_dispatch_job(void *unused)
{
    (void)unused;
    if (!ui_lvgl_async_call(wifi_apply_scan_lvgl, NULL))
        ESP_LOGW(TAG, "failed to schedule WiFi scan UI");
}

/* ---- scan completion callback (runs on UART task) ---- */
static void on_scan_complete(const char **lines, int line_count)
{
    ESP_LOGI(TAG, "Scan callback: %d collected lines", line_count);
    network_count = 0;
    for (int i = 0; i < line_count; i++) {
        wifi_network_t net;
        if (!parse_network_line(lines[i], &net)) continue;
        if (!psram_dynarr_ensure((void **)&networks, &networks_cap,
                                 network_count + 1, sizeof(*networks),
                                 WIFI_NET_HARD_CAP)) {
            ESP_LOGW(TAG, "wifi cap reached at %d, dropping rest", network_count);
            break;
        }
        networks[network_count++] = net;
        ESP_LOGD(TAG, " #%d %s ch%d %ddBm",
                 net.index, net.ssid[0] ? net.ssid : "(hidden)",
                 net.channel, net.rssi);
    }
    ESP_LOGI(TAG, "Parsed %d networks from %d lines", network_count, line_count);

    s_scan_line_count = line_count;
    if (!parse_worker_post(wifi_lvgl_dispatch_job, NULL)) {
        /* Fallback if worker queue full */
        if (!ui_lvgl_async_call(wifi_apply_scan_lvgl, NULL))
            ESP_LOGE(TAG, "WiFi UI schedule failed completely");
    }
}

/* ---- public entry point ---- */
void show_wifi_scan_screen(void)
{
    lv_obj_t *scr = ui_screen_clear();

    ui_create_top_bar(scr, "WiFi Scan", on_back_to_home, NULL);

    scan_spinner = lv_spinner_create(scr);
    lv_obj_set_size(scan_spinner, 50, 50);
    lv_obj_center(scan_spinner);
    lv_obj_set_y(scan_spinner, 90);

    scan_status_lbl = lv_label_create(scr);
    lv_label_set_text(scan_status_lbl, "Scanning networks...");
    lv_obj_set_style_text_color(scan_status_lbl, ui_text_color(), 0);
    lv_obj_set_style_text_font(scan_status_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(scan_status_lbl, LV_ALIGN_CENTER, 0, 50);

    network_count = 0;
    if (networks && networks_cap > 0) {
        memset(networks, 0, (size_t)networks_cap * sizeof(*networks));
    }
    uart_start_collect("Scan results printed", on_scan_complete);
    uart_send_command("scan_networks");
}
