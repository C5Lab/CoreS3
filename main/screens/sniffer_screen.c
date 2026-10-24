#include "sniffer_screen.h"
#include "attack_select_screen.h"
#include "karma_screen.h"
#include "wifi_scan_screen.h"
#include "ui_helpers.h"
#include "uart_handler.h"
#include "janos_sniffer_session.h"
#include "bsp/m5stack_core_s3.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>

static const char *TAG = "sniffer";

#define MAX_VISIBLE_CLIENTS_PER_AP 8

static JsSession s_js;
static SemaphoreHandle_t s_js_mutex = NULL;
static lv_timer_t *s_js_tick_timer = NULL;
static JsPhase s_js_seen_phase = JS_EMPTY;
static lv_obj_t *clients_status = NULL;
static lv_obj_t *deauth_popup = NULL;
static lv_obj_t *deauth_btn_lbl = NULL;
static unsigned deauth_client_idx = 0;
static bool deauth_active = false;
static bool clients_view = false;

/* ---- sniffer state ---- */

static lv_obj_t *pkt_label    = NULL;
static bool      sniff_running = false;
static int       pkt_count     = 0;

/* ---- forward declarations ---- */
static void show_sniffer_main(void);
static void show_clients_screen(void);

/* ================================================================== */
/*  UART callback -- normal sniffer mode (packet counter)             */
/* ================================================================== */

static void sniff_uart_line_cb(const char *line)
{
    if (!sniff_running) return;
    const char *p = strstr(line, "Sniffer packet count:");
    if (p) {
        pkt_count = atoi(p + strlen("Sniffer packet count:"));
        bsp_display_lock(0);
        if (pkt_label) {
            char txt[48];
            snprintf(txt, sizeof(txt), "Packets: %d", pkt_count);
            lv_label_set_text(pkt_label, txt);
        }
        bsp_display_unlock();
    }
}

static uint32_t sniff_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool sniff_js_send(void *ctx, const char *command)
{
    (void)ctx;
    uart_send_command(command);
    return true;
}

static void sniff_js_ensure(void)
{
    if (!s_js_mutex) {
        s_js_mutex = xSemaphoreCreateMutex();
        js_init(&s_js, sniff_js_send, NULL);
    }
}

static bool sniff_js_lock(void)
{
    sniff_js_ensure();
    return s_js_mutex && xSemaphoreTake(s_js_mutex, pdMS_TO_TICKS(50)) == pdTRUE;
}

static void sniff_js_unlock(void)
{
    if (s_js_mutex) xSemaphoreGive(s_js_mutex);
}

static void sniff_js_line_cb(const char *line)
{
    const char *p = strstr(line, "Sniffer packet count:");
    if (p) {
        pkt_count = atoi(p + strlen("Sniffer packet count:"));
        if (pkt_label) {
            bsp_display_lock(0);
            if (pkt_label) {
                char txt[48];
                snprintf(txt, sizeof(txt), "Packets: %d", pkt_count);
                lv_label_set_text(pkt_label, txt);
            }
            bsp_display_unlock();
        }
    }
    if (!sniff_js_lock()) return;
    js_line(&s_js, line, sniff_now_ms());
    sniff_js_unlock();
}

static void sniff_apply_phase_ui(void);

static void sniff_js_tick_cb(lv_timer_t *t)
{
    (void)t;
    if (!sniff_js_lock()) return;
    js_tick(&s_js, sniff_now_ms());
    JsPhase phase = s_js.phase;
    bool changed = phase != s_js_seen_phase;
    s_js_seen_phase = phase;
    sniff_js_unlock();
    if (changed)
        sniff_apply_phase_ui();
}

static void sniff_js_start_tick(void)
{
    if (!s_js_tick_timer)
        s_js_tick_timer = lv_timer_create(sniff_js_tick_cb, 250, NULL);
}

static void sniff_js_stop_tick(void)
{
    if (s_js_tick_timer) {
        lv_timer_delete(s_js_tick_timer);
        s_js_tick_timer = NULL;
    }
}

static void start_collect_clients(void)
{
    sniff_js_ensure();
    uart_send_command("stop");
    uart_set_line_callback(sniff_js_line_cb);
    clients_view = true;
    if (sniff_js_lock()) {
        js_load(&s_js, sniff_now_ms());
        s_js_seen_phase = s_js.phase;
        sniff_js_unlock();
    }
    sniff_js_start_tick();
    show_clients_screen();
}

/* ---- Clients screen UI ---- */

static void on_clients_back(lv_event_t *e)
{
    (void)e;
    clients_view = false;
    sniff_js_stop_tick();
    if (sniff_js_lock()) {
        if (s_js.phase == JS_STARTING || s_js.phase == JS_ACTIVE)
            js_cancel(&s_js);
        sniff_js_unlock();
    }
    uart_send_command("start_sniffer");
    bsp_display_lock(0);
    show_sniffer_main();
    bsp_display_unlock();
}

static void close_sniff_deauth(void)
{
    if (sniff_js_lock()) {
        if (s_js.phase == JS_STARTING || s_js.phase == JS_ACTIVE)
            js_cancel(&s_js);
        sniff_js_unlock();
    }
    deauth_active = false;
    if (deauth_popup) {
        lv_obj_del(deauth_popup);
        deauth_popup = NULL;
    }
    deauth_btn_lbl = NULL;
    uart_set_line_callback(sniff_js_line_cb);
    show_clients_screen();
}

static void sniff_deauth_btn_cb(lv_event_t *e)
{
    int action = (int)(intptr_t)lv_event_get_user_data(e);
    if (action == 1) {
        close_sniff_deauth();
        return;
    }
    if (deauth_active) {
        close_sniff_deauth();
        return;
    }
    uart_set_line_callback(sniff_js_line_cb);
    if (sniff_js_lock()) {
        js_start(&s_js, deauth_client_idx, sniff_now_ms());
        s_js_seen_phase = s_js.phase;
        sniff_js_unlock();
    }
    if (deauth_btn_lbl)
        lv_label_set_text(deauth_btn_lbl, "Starting...");
}

static void show_sniff_deauth(unsigned client_idx)
{
    if (!sniff_js_lock()) return;
    bool ready = s_js.phase == JS_READY && client_idx < s_js.model.client_count;
    sc_client_t client = {0};
    sc_ap_t ap = {0};
    if (ready) {
        client = s_js.model.clients[client_idx];
        ap = s_js.model.aps[client.ap];
    }
    sniff_js_unlock();
    if (!ready) return;

    deauth_client_idx = client_idx;
    deauth_active = false;
    if (deauth_popup) {
        lv_obj_del(deauth_popup);
        deauth_popup = NULL;
    }

    char label[40], mac[18], bssid[18];
    sc_ap_label(&ap, label, sizeof(label));
    sc_mac_text(mac, client.mac);
    sc_mac_text(bssid, ap.bssid);

    lv_obj_t *scr = lv_scr_act();
    deauth_popup = lv_obj_create(scr);
    lv_obj_set_size(deauth_popup, 290, 170);
    lv_obj_center(deauth_popup);
    lv_obj_set_style_bg_color(deauth_popup, ui_card_color(), 0);
    lv_obj_set_style_border_color(deauth_popup, UI_ACCENT_RED, 0);
    lv_obj_set_style_border_width(deauth_popup, 2, 0);
    lv_obj_set_style_radius(deauth_popup, 12, 0);
    lv_obj_set_style_pad_all(deauth_popup, 10, 0);
    lv_obj_set_flex_flow(deauth_popup, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(deauth_popup, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(deauth_popup);
    lv_label_set_text(title, "Deauth Station");
    lv_obj_set_style_text_color(title, UI_ACCENT_RED, 0);

    lv_obj_t *l1 = lv_label_create(deauth_popup);
    lv_label_set_text(l1, label);
    lv_obj_t *l2 = lv_label_create(deauth_popup);
    lv_label_set_text(l2, mac);
    lv_obj_set_style_text_color(l2, UI_ACCENT_RED, 0);
    lv_obj_t *l3 = lv_label_create(deauth_popup);
    lv_label_set_text(l3, bssid);
    lv_obj_set_style_text_color(l3, ui_muted_color(), 0);
    lv_obj_set_style_text_font(l3, &lv_font_montserrat_10, 0);

    lv_obj_t *row = lv_obj_create(deauth_popup);
    lv_obj_set_size(row, LV_PCT(100), 34);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);

    lv_obj_t *go = lv_btn_create(row);
    lv_obj_set_size(go, 150, 32);
    lv_obj_set_style_bg_color(go, UI_ACCENT_RED, 0);
    lv_obj_add_event_cb(go, sniff_deauth_btn_cb, LV_EVENT_CLICKED, (void*)(intptr_t)0);
    deauth_btn_lbl = lv_label_create(go);
    lv_label_set_text(deauth_btn_lbl, "Deauth Station");
    lv_obj_center(deauth_btn_lbl);

    lv_obj_t *x = lv_btn_create(row);
    lv_obj_set_size(x, 70, 32);
    lv_obj_add_event_cb(x, sniff_deauth_btn_cb, LV_EVENT_CLICKED, (void*)(intptr_t)1);
    lv_obj_t *xl = lv_label_create(x);
    lv_label_set_text(xl, "Close");
    lv_obj_center(xl);
}

static void on_sniff_cli_click(lv_event_t *e)
{
    unsigned idx = (unsigned)(uintptr_t)lv_event_get_user_data(e);
    show_sniff_deauth(idx);
}

static void show_clients_screen(void)
{
    lv_obj_t *scr = ui_screen_clear();
    pkt_label = NULL;
    deauth_popup = NULL;
    deauth_btn_lbl = NULL;
    ui_create_top_bar(scr, "Clients", on_clients_back, NULL);

    clients_status = lv_label_create(scr);
    lv_obj_set_pos(clients_status, 8, 38);
    lv_obj_set_style_text_font(clients_status, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(clients_status, ui_muted_color(), 0);

    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_set_size(list, LV_PCT(100), 240 - 52);
    lv_obj_set_pos(list, 0, 52);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 6, 0);
    lv_obj_set_style_pad_row(list, 2, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    if (!sniff_js_lock()) return;
    JsPhase phase = s_js.phase;
    unsigned aps = s_js.model.ap_count;
    unsigned clients = s_js.model.client_count;
    char err[48];
    snprintf(err, sizeof(err), "%s", s_js.error);
    if (phase == JS_LOADING)
        lv_label_set_text(clients_status, "Loading...");
    else if (phase == JS_ERROR || phase == JS_RELOAD_REQUIRED)
        lv_label_set_text(clients_status, err[0] ? err : "Reload required");
    else {
        char st[48];
        snprintf(st, sizeof(st), "%u APs, %u clients", aps, clients);
        lv_label_set_text(clients_status, st);
    }
    if (phase != JS_READY || !s_js.model.complete) {
        lv_obj_t *lbl = lv_label_create(list);
        lv_label_set_text(lbl, phase == JS_LOADING
            ? "Loading client snapshot..."
            : "No clients. Refresh from Sniffer.");
        lv_obj_set_style_text_color(lbl, ui_muted_color(), 0);
        sniff_js_unlock();
        return;
    }
    for (unsigned i = 0; i < aps; i++) {
        char label[40];
        unsigned shown = 0, ap_clients = 0;
        sc_ap_label(&s_js.model.aps[i], label, sizeof(label));
        for (unsigned j = 0; j < clients; j++)
            if (s_js.model.clients[j].ap == (uint8_t)i) ap_clients++;
        lv_obj_t *net_lbl = lv_label_create(list);
        char net_txt[72];
        snprintf(net_txt, sizeof(net_txt), "%s (%u)", label, ap_clients);
        lv_label_set_text(net_lbl, net_txt);
        lv_obj_set_style_text_color(net_lbl, UI_ACCENT_CYAN, 0);
        lv_obj_set_style_text_font(net_lbl, &lv_font_montserrat_12, 0);
        for (unsigned j = 0; j < clients; j++) {
            if (s_js.model.clients[j].ap != (uint8_t)i) continue;
            if (shown >= MAX_VISIBLE_CLIENTS_PER_AP) break;
            char mac[18];
            sc_mac_text(mac, s_js.model.clients[j].mac);
            lv_obj_t *btn = lv_btn_create(list);
            lv_obj_set_size(btn, LV_PCT(100), 20);
            lv_obj_set_style_bg_color(btn, ui_card_color(), 0);
            lv_obj_add_event_cb(btn, on_sniff_cli_click, LV_EVENT_CLICKED, (void*)(uintptr_t)j);
            lv_obj_t *c_lbl = lv_label_create(btn);
            lv_label_set_text(c_lbl, mac);
            lv_obj_set_style_text_font(c_lbl, &lv_font_montserrat_10, 0);
            shown++;
        }
    }
    sniff_js_unlock();
}

static void sniff_apply_phase_ui(void)
{
    JsPhase phase = JS_EMPTY;
    if (sniff_js_lock()) {
        phase = s_js.phase;
        sniff_js_unlock();
    }
    if (phase == JS_ACTIVE) {
        deauth_active = true;
        if (deauth_btn_lbl)
            lv_label_set_text(deauth_btn_lbl, LV_SYMBOL_CLOSE " STOP");
        return;
    }
    if (clients_view && !deauth_popup)
        show_clients_screen();
}

/* ================================================================== */
/*  Main sniffer screen                                               */
/* ================================================================== */

static void on_stop(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Sniffer STOP");
    sniff_running = false;
    clients_view = false;
    sniff_js_stop_tick();
    uart_set_line_callback(NULL);
    if (sniff_js_lock()) {
        if (s_js.phase == JS_STARTING || s_js.phase == JS_ACTIVE)
            js_cancel(&s_js);
        js_free(&s_js);
        js_init(&s_js, sniff_js_send, NULL);
        sniff_js_unlock();
    }
    uart_send_command("stop");
    bsp_display_lock(0);
    show_attack_select_screen();
    bsp_display_unlock();
}

static void on_clients(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Show Clients");
    start_collect_clients();
}

static void on_karma(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Karma from sniffer");
    sniff_running = false;
    uart_set_line_callback(NULL);
    bsp_display_lock(0);
    show_karma_screen();
    bsp_display_unlock();
}

static void on_back(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Sniffer back");
    sniff_running = false;
    clients_view = false;
    sniff_js_stop_tick();
    uart_set_line_callback(NULL);
    if (sniff_js_lock()) {
        if (s_js.phase == JS_STARTING || s_js.phase == JS_ACTIVE)
            js_cancel(&s_js);
        js_free(&s_js);
        js_init(&s_js, sniff_js_send, NULL);
        sniff_js_unlock();
    }
    uart_send_command("stop");
    bsp_display_lock(0);
    show_attack_select_screen();
    bsp_display_unlock();
}

static void show_sniffer_main(void)
{
    sniff_running = true;
    uart_set_line_callback(sniff_uart_line_cb);

    int indices[MAX_NETWORKS];
    int sel_count = wifi_scan_get_selected(indices, MAX_NETWORKS);
    wifi_network_t *nets = wifi_scan_get_networks();

    lv_obj_t *scr = ui_screen_clear();

    char title[40];
    snprintf(title, sizeof(title), "Sniffer (%d net%s)", sel_count, sel_count == 1 ? "" : "s");
    ui_create_top_bar(scr, title, on_back, NULL);

    /* content area */
    lv_obj_t *content = lv_obj_create(scr);
    lv_obj_set_size(content, LV_PCT(100), 240 - 36 - 44);
    lv_obj_set_pos(content, 0, 36);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 6, 0);
    lv_obj_set_style_pad_row(content, 2, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);

    /* selected networks list */
    lv_obj_t *hdr = lv_label_create(content);
    lv_label_set_text(hdr, "Sniffing:");
    lv_obj_set_style_text_color(hdr, ui_muted_color(), 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_12, 0);

    for (int i = 0; i < sel_count && i < 6; i++) {
        wifi_network_t *net = &nets[indices[i]];
        lv_obj_t *lbl = lv_label_create(content);
        char txt[64];
        snprintf(txt, sizeof(txt), " " LV_SYMBOL_RIGHT " %s  ch%d  %ddBm",
                 net->ssid[0] ? net->ssid : "(hidden)",
                 net->channel, net->rssi);
        lv_label_set_text(lbl, txt);
        lv_obj_set_style_text_color(lbl, UI_ACCENT_GREEN, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
    }
    if (sel_count > 6) {
        lv_obj_t *more = lv_label_create(content);
        char mtxt[24];
        snprintf(mtxt, sizeof(mtxt), "  +%d more...", sel_count - 6);
        lv_label_set_text(more, mtxt);
        lv_obj_set_style_text_color(more, ui_muted_color(), 0);
        lv_obj_set_style_text_font(more, &lv_font_montserrat_10, 0);
    }

    /* packet counter */
    pkt_label = lv_label_create(content);
    char pkt_txt[48];
    snprintf(pkt_txt, sizeof(pkt_txt), "Packets: %d", pkt_count);
    lv_label_set_text(pkt_label, pkt_txt);
    lv_obj_set_style_text_color(pkt_label, ui_text_color(), 0);
    lv_obj_set_style_text_font(pkt_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_pad_top(pkt_label, 8, 0);

    /* bottom button row */
    lv_obj_t *btn_row = lv_obj_create(scr);
    lv_obj_set_size(btn_row, LV_PCT(100), 40);
    lv_obj_align(btn_row, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(btn_row, ui_panel_color(), 0);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_radius(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 4, 0);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    /* Karma button */
    lv_obj_t *karma_btn = lv_btn_create(btn_row);
    lv_obj_set_size(karma_btn, 90, 32);
    lv_obj_set_style_bg_color(karma_btn, UI_ACCENT_ORANGE, 0);
    lv_obj_set_style_radius(karma_btn, 8, 0);
    lv_obj_add_event_cb(karma_btn, on_karma, LV_EVENT_CLICKED, NULL);
    lv_obj_t *k_lbl = lv_label_create(karma_btn);
    lv_label_set_text(k_lbl, "Karma");
    lv_obj_set_style_text_color(k_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(k_lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(k_lbl);

    /* Clients button */
    lv_obj_t *clients_btn = lv_btn_create(btn_row);
    lv_obj_set_size(clients_btn, 90, 32);
    lv_obj_set_style_bg_color(clients_btn, UI_ACCENT_CYAN, 0);
    lv_obj_set_style_radius(clients_btn, 8, 0);
    lv_obj_add_event_cb(clients_btn, on_clients, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl_lbl = lv_label_create(clients_btn);
    lv_label_set_text(cl_lbl, "Clients");
    lv_obj_set_style_text_color(cl_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(cl_lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(cl_lbl);

    /* Stop button */
    lv_obj_t *stop_btn = lv_btn_create(btn_row);
    lv_obj_set_size(stop_btn, 90, 32);
    lv_obj_set_style_bg_color(stop_btn, UI_ACCENT_RED, 0);
    lv_obj_set_style_radius(stop_btn, 8, 0);
    lv_obj_add_event_cb(stop_btn, on_stop, LV_EVENT_CLICKED, NULL);
    lv_obj_t *s_lbl = lv_label_create(stop_btn);
    lv_label_set_text(s_lbl, LV_SYMBOL_CLOSE " Stop");
    lv_obj_set_style_text_color(s_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(s_lbl);
}

/* ================================================================== */
/*  Entry point -- called from attack_select_screen                   */
/* ================================================================== */

void show_sniffer_screen(void)
{
    send_select_networks();
    uart_send_command("start_sniffer");
    ESP_LOGI(TAG, "Sniffer started");

    pkt_count = 0;
    sniff_running = true;

    show_sniffer_main();
}
