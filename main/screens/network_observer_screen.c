#include "network_observer_screen.h"
#include "home_screen.h"
#include "ui_helpers.h"
#include "uart_handler.h"
#include "psram_dynarr.h"
#include "parse_worker.h"
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

static const char *TAG = "observer";

/* ================================================================== */
/*  Data structures                                                    */
/* ================================================================== */

#define OBS_NET_HARD_CAP    256
#define MAX_CLIENTS_PER_NET 32
#define OBS_PROBE_HARD_CAP  1024
#define MAX_SD_FILES        32
#define MAX_VISIBLE_CLIENTS_PER_AP 8
#define JS_TICK_MS          250

typedef struct {
    char ssid[33];
    char bssid[18];
    int  scan_index;
    int  channel;
    int  rssi;
    char band[8];
    char security[24];
    int  client_count;
    char clients[MAX_CLIENTS_PER_NET][18];
    bool matched_this_poll;   /* claimed during current poll cycle */
} obs_network_t;

typedef struct {
    int  index;
    char ssid[33];
} obs_probe_t;

typedef struct {
    int  number;
    char filename[64];
} obs_sd_file_t;

/* ================================================================== */
/*  State                                                              */
/* ================================================================== */

static obs_network_t *obs_nets = NULL;
static int  obs_nets_cap     = 0;
static int  obs_net_count    = 0;
static bool obs_running      = false;

static esp_timer_handle_t poll_timer    = NULL;
static esp_timer_handle_t line_timer    = NULL;
static bool poll_collecting = false;

static int  focused_net_idx  = -1;
static bool deauth_active    = false;
static unsigned deauth_client_idx = 0;
static char deauth_label[40];
static char deauth_mac[18];
static char deauth_bssid[18];
static char deauth_status[48];

static JsSession s_js;
static SemaphoreHandle_t s_js_mutex = NULL;
static lv_timer_t *s_js_tick_timer = NULL;
static JsPhase s_js_seen_phase = JS_EMPTY;

static obs_probe_t *probes = NULL;
static int  probes_cap         = 0;
static int  probe_count        = 0;
static bool probe_collecting   = false;
static int  selected_probe_idx = -1;
static esp_timer_handle_t probe_timer = NULL;

static obs_sd_file_t sd_files[MAX_SD_FILES];
static int  sd_file_count    = 0;
static bool sd_collecting    = false;
static esp_timer_handle_t sd_timer = NULL;

static bool karma_running    = false;
static lv_obj_t *karma_status_lbl  = NULL;
static lv_obj_t *karma_capture_lbl = NULL;

static lv_obj_t *status_label = NULL;
static lv_obj_t *popup_obj    = NULL;
static lv_obj_t *obs_list     = NULL;

#define OBS_MAIN_NETS_PER_TICK 2
#define OBS_MAIN_BUILD_MS      25

static lv_timer_t *obs_main_build_timer = NULL;
static int         obs_main_build_idx   = 0;

/* ---- forward declarations ---- */
static void sort_networks(void);
static void stop_obs_main_build_timer(void);
static void show_main_view(void);
static void rebuild_list_content(void);
static void show_probe_picker(void);
static void show_html_picker(void);
static void show_karma_running(void);
static void start_poll_timer(int64_t interval);
static void stop_all(void);
static void on_scan_complete(const char **lines, int count);
static void obs_load_clients(void);
static void obs_js_ensure(void);

/* ================================================================== */
/*  CSV parsing (scan_networks output)                                 */
/* ================================================================== */

static const char *parse_quoted(const char *p, char *out, int max)
{
    if (*p != '"') return NULL;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < max - 1) out[i++] = *p++;
    out[i] = '\0';
    if (*p != '"') return NULL;
    p++;
    if (*p == ',') p++;
    return p;
}

static bool parse_scan_line(const char *line, obs_network_t *net)
{
    if (line[0] != '"') return false;
    const char *p = line;
    char field[64];

    p = parse_quoted(p, field, sizeof(field));  if (!p) return false;
    net->scan_index = atoi(field);

    p = parse_quoted(p, net->ssid, sizeof(net->ssid));   if (!p) return false;
    p = parse_quoted(p, field, sizeof(field));            if (!p) return false; /* empty */
    p = parse_quoted(p, net->bssid, sizeof(net->bssid)); if (!p) return false;

    p = parse_quoted(p, field, sizeof(field));  if (!p) return false;
    net->channel = atoi(field);

    p = parse_quoted(p, net->security, sizeof(net->security)); if (!p) return false;

    p = parse_quoted(p, field, sizeof(field));  if (!p) return false;
    net->rssi = atoi(field);

    parse_quoted(p, net->band, sizeof(net->band));
    net->client_count = 0;
    memset(net->clients, 0, sizeof(net->clients));
    return true;
}

static uint32_t obs_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool obs_js_send(void *ctx, const char *command)
{
    (void)ctx;
    uart_send_command(command);
    return true;
}

static void obs_js_ensure(void)
{
    if (!s_js_mutex) {
        s_js_mutex = xSemaphoreCreateMutex();
        js_init(&s_js, obs_js_send, NULL);
    }
}

static bool obs_js_lock(void)
{
    obs_js_ensure();
    return s_js_mutex && xSemaphoreTake(s_js_mutex, pdMS_TO_TICKS(50)) == pdTRUE;
}

static void obs_js_unlock(void)
{
    if (s_js_mutex) xSemaphoreGive(s_js_mutex);
}

/* ================================================================== */
/*  Timer helpers                                                      */
/* ================================================================== */

static void ensure_timer(esp_timer_handle_t *handle, const char *name,
                         esp_timer_cb_t cb)
{
    if (*handle) return;
    esp_timer_create_args_t a = { .callback = cb, .name = name };
    esp_timer_create(&a, handle);
}

/* ================================================================== */
/*  Client snapshot (show_sniffer_clients)                             */
/* ================================================================== */

static void obs_js_line_cb(const char *line)
{
    if (!obs_js_lock()) return;
    js_line(&s_js, line, obs_now_ms());
    obs_js_unlock();
}

static void obs_apply_phase_ui(void);

static void obs_js_tick_cb(lv_timer_t *t)
{
    (void)t;
    if (!obs_js_lock()) return;
    js_tick(&s_js, obs_now_ms());
    JsPhase phase = s_js.phase;
    bool phase_changed = phase != s_js_seen_phase;
    s_js_seen_phase = phase;
    obs_js_unlock();
    if (phase_changed)
        obs_apply_phase_ui();
}

static void obs_js_start_tick_timer(void)
{
    if (!s_js_tick_timer)
        s_js_tick_timer = lv_timer_create(obs_js_tick_cb, JS_TICK_MS, NULL);
}

static void obs_js_stop_tick_timer(void)
{
    if (s_js_tick_timer) {
        lv_timer_delete(s_js_tick_timer);
        s_js_tick_timer = NULL;
    }
}

static void obs_load_clients(void)
{
    obs_js_ensure();
    uart_set_line_callback(obs_js_line_cb);
    if (!obs_js_lock()) return;
    bool ok = js_load(&s_js, obs_now_ms());
    s_js_seen_phase = s_js.phase;
    obs_js_unlock();
    poll_collecting = ok;
    obs_js_start_tick_timer();
    if (status_label)
        lv_label_set_text(status_label, ok ? "Loading clients..." : "UART TX failed");
}

static void start_poll_timer(int64_t interval)
{
    (void)interval;
    obs_load_clients();
}

/* ================================================================== */
/*  Stop everything                                                    */
/* ================================================================== */

static void stop_all(void)
{
    if (bsp_display_lock(portMAX_DELAY)) {
        stop_obs_main_build_timer();
        ui_display_unlock_safe();
    }

    obs_running = false;
    poll_collecting = false;
    probe_collecting = false;
    sd_collecting = false;
    karma_running = false;
    deauth_active = false;
    focused_net_idx = -1;
    obs_list = NULL;
    status_label = NULL;

    obs_js_stop_tick_timer();
    uart_set_line_callback(NULL);
    if (poll_timer) esp_timer_stop(poll_timer);
    if (line_timer) esp_timer_stop(line_timer);
    if (probe_timer) esp_timer_stop(probe_timer);
    if (sd_timer) esp_timer_stop(sd_timer);

    if (obs_js_lock()) {
        if (s_js.phase == JS_STARTING || s_js.phase == JS_ACTIVE)
            js_cancel(&s_js);
        js_free(&s_js);
        js_init(&s_js, obs_js_send, NULL);
        s_js_seen_phase = JS_EMPTY;
        obs_js_unlock();
    }

    uart_send_command("stop");
}

/* ================================================================== */
/*  Navigation helpers                                                 */
/* ================================================================== */

static void on_back_home(lv_event_t *e)
{
    (void)e;
    stop_all();
    if (!ui_display_lock_wait()) return;
    show_home_screen();
    ui_display_unlock_safe();
}

static void resume_sniffer(void)
{
    focused_net_idx = -1;
    deauth_active   = false;
    obs_running     = true;

    uart_set_line_callback(NULL);
    uart_send_command("stop");
    uart_send_command("unselect_networks");

    /* Fast path: reuse the firmware's existing scan results. If that store was
     * lost (e.g. a previous full sniff/scan was aborted), the firmware replies
     * "Please scan_networks first." -- in that case re-scan to self-heal. */
    char resp[UART_MAX_LINE_LEN];
    bool need_rescan = uart_send_wait_line("start_sniffer_noscan",
                                           "scan_networks first",
                                           resp, sizeof(resp), 1500);
    if (need_rescan) {
        uart_start_collect("Scan results printed", on_scan_complete);
        uart_send_command("scan_networks");
        return; /* on_scan_complete resumes sniffer + snapshot */
    }

    obs_load_clients();
}

/* ================================================================== */
/*  Deauth popup                                                       */
/* ================================================================== */

static lv_obj_t *deauth_btn_lbl = NULL;

static void close_deauth_popup(void)
{
    if (obs_js_lock()) {
        if (s_js.phase == JS_STARTING || s_js.phase == JS_ACTIVE)
            js_cancel(&s_js);
        obs_js_unlock();
    }
    deauth_active = false;
    if (popup_obj) { lv_obj_del(popup_obj); popup_obj = NULL; }
    deauth_btn_lbl = NULL;
    uart_set_line_callback(obs_js_line_cb);
    if (!ui_display_lock_wait()) return;
    show_main_view();
    ui_display_unlock_safe();
}

static void deauth_btn_cb(lv_event_t *e)
{
    int action = (int)(intptr_t)lv_event_get_user_data(e);

    if (action == 1) {          /* close X */
        close_deauth_popup();
        return;
    }

    if (!deauth_active) {
        uart_set_line_callback(obs_js_line_cb);
        bool started = false;
        if (obs_js_lock()) {
            started = js_start(&s_js, deauth_client_idx, obs_now_ms());
            if (!started && s_js.error[0])
                snprintf(deauth_status, sizeof(deauth_status), "%s", s_js.error);
            s_js_seen_phase = s_js.phase;
            obs_js_unlock();
        }
        if (deauth_btn_lbl)
            lv_label_set_text(deauth_btn_lbl, started ? "Starting..." : "Retry");
        obs_js_start_tick_timer();
    } else {
        close_deauth_popup();
    }
}

static void show_deauth_popup(unsigned client_idx)
{
    if (!obs_js_lock()) return;
    bool ready = s_js.phase == JS_READY && s_js.model.complete && !s_js.model.failed
                 && client_idx < s_js.model.client_count;
    sc_ap_t ap = {0};
    sc_client_t client = {0};
    if (ready) {
        client = s_js.model.clients[client_idx];
        ap = s_js.model.aps[client.ap];
    }
    obs_js_unlock();
    if (!ready || popup_obj) return;

    deauth_client_idx = client_idx;
    deauth_active = false;
    deauth_status[0] = 0;
    sc_ap_label(&ap, deauth_label, sizeof(deauth_label));
    sc_mac_text(deauth_mac, client.mac);
    sc_mac_text(deauth_bssid, ap.bssid);

    if (poll_timer) esp_timer_stop(poll_timer);

    lv_obj_t *scr = lv_scr_act();
    popup_obj = lv_obj_create(scr);
    lv_obj_set_size(popup_obj, 290, 175);
    lv_obj_center(popup_obj);
    lv_obj_set_style_bg_color(popup_obj, ui_card_color(), 0);
    lv_obj_set_style_border_color(popup_obj, UI_ACCENT_RED, 0);
    lv_obj_set_style_border_width(popup_obj, 2, 0);
    lv_obj_set_style_radius(popup_obj, 12, 0);
    lv_obj_set_style_pad_all(popup_obj, 10, 0);
    lv_obj_set_flex_flow(popup_obj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(popup_obj, 6, 0);
    lv_obj_clear_flag(popup_obj, LV_OBJ_FLAG_SCROLLABLE);

    /* header row */
    lv_obj_t *hdr = lv_obj_create(popup_obj);
    lv_obj_set_size(hdr, LV_PCT(100), 24);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(hdr);
    lv_label_set_text(title, "Deauth Station");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, UI_ACCENT_RED, 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *x_btn = lv_btn_create(hdr);
    lv_obj_set_size(x_btn, 28, 24);
    lv_obj_align(x_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(x_btn, ui_muted_color(), 0);
    lv_obj_set_style_radius(x_btn, 6, 0);
    lv_obj_add_event_cb(x_btn, deauth_btn_cb, LV_EVENT_CLICKED, (void*)(intptr_t)1);
    lv_obj_t *x_lbl = lv_label_create(x_btn);
    lv_label_set_text(x_lbl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(x_lbl, lv_color_white(), 0);
    lv_obj_center(x_lbl);

    /* info */
    lv_obj_t *lbl1 = lv_label_create(popup_obj);
    char t1[64];
    snprintf(t1, sizeof(t1), "Net: %s", deauth_label);
    lv_label_set_text(lbl1, t1);
    lv_obj_set_style_text_color(lbl1, ui_text_color(), 0);
    lv_obj_set_style_text_font(lbl1, &lv_font_montserrat_12, 0);

    lv_obj_t *lbl2 = lv_label_create(popup_obj);
    char t2[48];
    snprintf(t2, sizeof(t2), "Client: %s", deauth_mac);
    lv_label_set_text(lbl2, t2);
    lv_obj_set_style_text_color(lbl2, UI_ACCENT_RED, 0);
    lv_obj_set_style_text_font(lbl2, &lv_font_montserrat_12, 0);

    lv_obj_t *lbl3 = lv_label_create(popup_obj);
    char t3[48];
    snprintf(t3, sizeof(t3), "AP: %s", deauth_bssid);
    lv_label_set_text(lbl3, t3);
    lv_obj_set_style_text_color(lbl3, ui_muted_color(), 0);
    lv_obj_set_style_text_font(lbl3, &lv_font_montserrat_10, 0);

    /* deauth button */
    lv_obj_t *d_btn = lv_btn_create(popup_obj);
    lv_obj_set_size(d_btn, LV_PCT(100), 34);
    lv_obj_set_style_bg_color(d_btn, UI_ACCENT_RED, 0);
    lv_obj_set_style_radius(d_btn, 8, 0);
    lv_obj_add_event_cb(d_btn, deauth_btn_cb, LV_EVENT_CLICKED, (void*)(intptr_t)0);
    deauth_btn_lbl = lv_label_create(d_btn);
    lv_label_set_text(deauth_btn_lbl, "Deauth Station");
    lv_obj_set_style_text_color(deauth_btn_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(deauth_btn_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(deauth_btn_lbl);
}

/* ================================================================== */
/*  Network popup (focus on single network)                            */
/* ================================================================== */

static lv_obj_t *net_popup_clients = NULL;

static void close_net_popup(lv_event_t *e)
{
    (void)e;
    if (popup_obj) { lv_obj_del(popup_obj); popup_obj = NULL; }
    net_popup_clients = NULL;
    focused_net_idx = -1;
}

static void net_popup_cli_click(lv_event_t *e)
{
    unsigned cli_idx = (unsigned)(uintptr_t)lv_event_get_user_data(e);
    if (popup_obj) { lv_obj_del(popup_obj); popup_obj = NULL; }
    net_popup_clients = NULL;

    if (!ui_display_lock_wait()) return;
    show_deauth_popup(cli_idx);
    ui_display_unlock_safe();
}

static void show_network_popup(int ap_idx)
{
    if (!obs_js_lock()) return;
    bool ready = s_js.phase == JS_READY && s_js.model.complete && !s_js.model.failed
                 && ap_idx >= 0 && (unsigned)ap_idx < s_js.model.ap_count;
    sc_ap_t ap = {0};
    unsigned client_count = s_js.model.client_count;
    if (ready) ap = s_js.model.aps[ap_idx];
    obs_js_unlock();
    if (!ready || popup_obj) return;

    focused_net_idx = ap_idx;

    lv_obj_t *scr = lv_scr_act();
    popup_obj = lv_obj_create(scr);
    lv_obj_set_size(popup_obj, 300, 210);
    lv_obj_center(popup_obj);
    lv_obj_set_style_bg_color(popup_obj, ui_card_color(), 0);
    lv_obj_set_style_border_color(popup_obj, UI_ACCENT_TEAL, 0);
    lv_obj_set_style_border_width(popup_obj, 2, 0);
    lv_obj_set_style_radius(popup_obj, 12, 0);
    lv_obj_set_style_pad_all(popup_obj, 8, 0);
    lv_obj_set_flex_flow(popup_obj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(popup_obj, 4, 0);

    /* header row */
    lv_obj_t *hdr = lv_obj_create(popup_obj);
    lv_obj_set_size(hdr, LV_PCT(100), 22);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    char ap_label[40];
    sc_ap_label(&ap, ap_label, sizeof(ap_label));
    lv_obj_t *tl = lv_label_create(hdr);
    lv_label_set_text(tl, ap_label);
    lv_obj_set_style_text_font(tl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(tl, UI_ACCENT_TEAL, 0);
    lv_obj_align(tl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *x_btn = lv_btn_create(hdr);
    lv_obj_set_size(x_btn, 28, 22);
    lv_obj_align(x_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(x_btn, UI_ACCENT_RED, 0);
    lv_obj_set_style_radius(x_btn, 6, 0);
    lv_obj_add_event_cb(x_btn, close_net_popup, LV_EVENT_CLICKED, NULL);
    lv_obj_t *x_lbl = lv_label_create(x_btn);
    lv_label_set_text(x_lbl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(x_lbl, lv_color_white(), 0);
    lv_obj_center(x_lbl);

    /* info */
    lv_obj_t *info1 = lv_label_create(popup_obj);
    char i1[64];
    char bssid_txt[18];
    sc_mac_text(bssid_txt, ap.bssid);
    snprintf(i1, sizeof(i1), "BSSID: %s", bssid_txt);
    lv_label_set_text(info1, i1);
    lv_obj_set_style_text_color(info1, ui_muted_color(), 0);
    lv_obj_set_style_text_font(info1, &lv_font_montserrat_10, 0);

    lv_obj_t *info2 = lv_label_create(popup_obj);
    char i2[32];
    snprintf(i2, sizeof(i2), "CH%u", ap.channel);
    lv_label_set_text(info2, i2);
    lv_obj_set_style_text_color(info2, ui_muted_color(), 0);
    lv_obj_set_style_text_font(info2, &lv_font_montserrat_10, 0);

    unsigned ap_clients = 0;
    if (obs_js_lock()) {
        for (unsigned i = 0; i < s_js.model.client_count; ++i)
            if (s_js.model.clients[i].ap == (uint8_t)ap_idx) ap_clients++;
        obs_js_unlock();
    }
    lv_obj_t *cl_hdr = lv_label_create(popup_obj);
    char ch[32];
    snprintf(ch, sizeof(ch), "Clients (%u):", ap_clients);
    lv_label_set_text(cl_hdr, ch);
    lv_obj_set_style_text_color(cl_hdr, UI_ACCENT_TEAL, 0);
    lv_obj_set_style_text_font(cl_hdr, &lv_font_montserrat_12, 0);

    /* scrollable clients */
    net_popup_clients = lv_obj_create(popup_obj);
    lv_obj_set_size(net_popup_clients, LV_PCT(100), 80);
    lv_obj_set_style_bg_color(net_popup_clients, ui_bg_color(), 0);
    lv_obj_set_style_border_width(net_popup_clients, 0, 0);
    lv_obj_set_style_radius(net_popup_clients, 6, 0);
    lv_obj_set_style_pad_all(net_popup_clients, 4, 0);
    lv_obj_set_style_pad_row(net_popup_clients, 2, 0);
    lv_obj_set_flex_flow(net_popup_clients, LV_FLEX_FLOW_COLUMN);

    if (ap_clients == 0) {
        lv_obj_t *nc = lv_label_create(net_popup_clients);
        lv_label_set_text(nc, "No clients yet...");
        lv_obj_set_style_text_color(nc, ui_muted_color(), 0);
        lv_obj_set_style_text_font(nc, &lv_font_montserrat_10, 0);
    } else if (obs_js_lock()) {
        for (unsigned j = 0; j < client_count; j++) {
            if (s_js.model.clients[j].ap != (uint8_t)ap_idx) continue;
            char mac[18];
            sc_mac_text(mac, s_js.model.clients[j].mac);
            lv_obj_t *cb = lv_btn_create(net_popup_clients);
            lv_obj_set_size(cb, LV_PCT(100), 22);
            lv_obj_set_style_bg_color(cb, ui_bg_color(), 0);
            lv_obj_set_style_bg_color(cb, UI_ACCENT_RED, LV_STATE_PRESSED);
            lv_obj_set_style_radius(cb, 4, 0);
            lv_obj_set_style_pad_hor(cb, 4, 0);
            lv_obj_add_event_cb(cb, net_popup_cli_click, LV_EVENT_CLICKED,
                                (void*)(uintptr_t)j);
            lv_obj_t *ml = lv_label_create(cb);
            lv_label_set_text(ml, mac);
            lv_obj_set_style_text_color(ml, ui_text_color(), 0);
            lv_obj_set_style_text_font(ml, &lv_font_montserrat_10, 0);
            lv_obj_align(ml, LV_ALIGN_LEFT_MID, 0, 0);
        }
        obs_js_unlock();
    }
}

/* ================================================================== */
/*  Sort networks by RSSI (strongest first)                            */
/* ================================================================== */

static int rssi_cmp(const void *a, const void *b)
{
    const obs_network_t *na = (const obs_network_t *)a;
    const obs_network_t *nb = (const obs_network_t *)b;
    return nb->rssi - na->rssi;
}

static void sort_networks(void)
{
    if (obs_net_count > 1)
        qsort(obs_nets, obs_net_count, sizeof(obs_network_t), rssi_cmp);
}

/* ================================================================== */
/*  Main view: network list with clients                               */
/* ================================================================== */

static void on_net_click(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (!ui_display_lock_wait()) return;
    show_network_popup(idx);
    ui_display_unlock_safe();
}

static void on_cli_click(lv_event_t *e)
{
    unsigned cli_i = (unsigned)(uintptr_t)lv_event_get_user_data(e);
    if (!ui_display_lock_wait()) return;
    show_deauth_popup(cli_i);
    ui_display_unlock_safe();
}

static void stop_obs_main_build_timer(void)
{
    if (obs_main_build_timer) {
        lv_timer_delete(obs_main_build_timer);
        obs_main_build_timer = NULL;
    }
}

static unsigned obs_ap_client_count(unsigned ap_i)
{
    unsigned n = 0;
    if (!obs_js_lock()) return 0;
    for (unsigned i = 0; i < s_js.model.client_count; ++i)
        if (s_js.model.clients[i].ap == (uint8_t)ap_i) n++;
    obs_js_unlock();
    return n;
}

static void obs_main_append_network(int net_i)
{
    if (!obs_list || !obs_js_lock())
        return;
    if (s_js.phase != JS_READY || !s_js.model.complete || (unsigned)net_i >= s_js.model.ap_count) {
        obs_js_unlock();
        return;
    }
    sc_ap_t ap = s_js.model.aps[net_i];
    unsigned client_count = s_js.model.client_count;
    obs_js_unlock();

    lv_obj_t *nr = lv_btn_create(obs_list);
    lv_obj_set_size(nr, LV_PCT(100), 26);
    lv_obj_set_style_bg_color(nr, ui_card_color(), 0);
    lv_obj_set_style_bg_color(nr, UI_ACCENT_TEAL, LV_STATE_PRESSED);
    lv_obj_set_style_radius(nr, 4, 0);
    lv_obj_set_style_pad_hor(nr, 6, 0);
    lv_obj_add_event_cb(nr, on_net_click, LV_EVENT_CLICKED, (void *)(intptr_t)net_i);

    lv_obj_t *nl = lv_label_create(nr);
    char ntxt[72];
    char label[40];
    sc_ap_label(&ap, label, sizeof(label));
    unsigned ap_clients = obs_ap_client_count((unsigned)net_i);
    snprintf(ntxt, sizeof(ntxt), "%s  %uc", label, ap_clients);
    lv_label_set_text(nl, ntxt);
    lv_label_set_long_mode(nl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(nl, 280);
    lv_obj_set_style_text_color(nl, UI_ACCENT_CYAN, 0);
    lv_obj_set_style_text_font(nl, &lv_font_montserrat_10, 0);
    lv_obj_align(nl, LV_ALIGN_LEFT_MID, 0, 0);

    unsigned shown = 0;
    if (!obs_js_lock()) return;
    for (unsigned j = 0; j < client_count; j++) {
        if (s_js.model.clients[j].ap != (uint8_t)net_i) continue;
        if (shown >= MAX_VISIBLE_CLIENTS_PER_AP) break;
        char mac[18];
        sc_mac_text(mac, s_js.model.clients[j].mac);
        lv_obj_t *cr = lv_btn_create(obs_list);
        lv_obj_set_size(cr, LV_PCT(100), 20);
        lv_obj_set_style_bg_color(cr, ui_card_color(), 0);
        lv_obj_set_style_bg_color(cr, UI_ACCENT_RED, LV_STATE_PRESSED);
        lv_obj_set_style_radius(cr, 2, 0);
        lv_obj_set_style_pad_hor(cr, 16, 0);
        lv_obj_add_event_cb(cr, on_cli_click, LV_EVENT_CLICKED, (void *)(uintptr_t)j);

        lv_obj_t *cl = lv_label_create(cr);
        lv_label_set_text(cl, mac);
        lv_obj_set_style_text_color(cl, ui_muted_color(), 0);
        lv_obj_set_style_text_font(cl, &lv_font_montserrat_10, 0);
        lv_obj_align(cl, LV_ALIGN_LEFT_MID, 0, 0);
        shown++;
    }
    obs_js_unlock();
}

static int obs_model_ap_count(void)
{
    int n = 0;
    if (obs_js_lock()) {
        if (s_js.phase == JS_READY && s_js.model.complete)
            n = (int)s_js.model.ap_count;
        obs_js_unlock();
    }
    return n;
}

static void obs_main_build_step(lv_timer_t *t)
{
    (void)t;
    if (!obs_list) {
        stop_obs_main_build_timer();
        return;
    }
    int aps = obs_model_ap_count();
    int target = obs_main_build_idx + OBS_MAIN_NETS_PER_TICK;
    if (target > aps)
        target = aps;
    for (; obs_main_build_idx < target; obs_main_build_idx++)
        obs_main_append_network(obs_main_build_idx);
    if (obs_main_build_idx >= aps)
        stop_obs_main_build_timer();
}

static void on_karma_btn(lv_event_t *e);
static void on_refresh_btn(lv_event_t *e);
static void on_stop_btn(lv_event_t *e);

static void show_main_view(void)
{
    stop_obs_main_build_timer();
    sort_networks();

    lv_obj_t *scr = ui_screen_clear();
    popup_obj = NULL;
    obs_list = NULL;
    ui_create_top_bar(scr, "Observer", on_back_home, NULL);

    /* status line */
    status_label = lv_label_create(scr);
    char st[64];
    unsigned aps = 0, clients = 0;
    JsPhase phase = JS_EMPTY;
    if (obs_js_lock()) {
        phase = s_js.phase;
        aps = s_js.model.ap_count;
        clients = s_js.model.client_count;
        if (s_js.error[0] && (phase == JS_ERROR || phase == JS_RELOAD_REQUIRED))
            snprintf(st, sizeof(st), "%s", s_js.error);
        else if (phase == JS_LOADING)
            snprintf(st, sizeof(st), "Loading clients...");
        else
            snprintf(st, sizeof(st), "%u APs, %u clients", aps, clients);
        obs_js_unlock();
    } else {
        snprintf(st, sizeof(st), "Observer");
    }
    lv_label_set_text(status_label, st);
    lv_obj_set_style_text_color(status_label, ui_muted_color(), 0);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_10, 0);
    lv_obj_set_pos(status_label, 8, 38);

    /* scrollable list */
    obs_list = lv_obj_create(scr);
    lv_obj_set_size(obs_list, LV_PCT(100), 240 - 52 - 40);
    lv_obj_set_pos(obs_list, 0, 52);
    lv_obj_set_style_bg_opa(obs_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obs_list, 0, 0);
    lv_obj_set_style_pad_all(obs_list, 4, 0);
    lv_obj_set_style_pad_row(obs_list, 2, 0);
    lv_obj_set_flex_flow(obs_list, LV_FLEX_FLOW_COLUMN);

    if (obs_model_ap_count() == 0) {
        lv_obj_t *nl = lv_label_create(obs_list);
        lv_label_set_text(nl, phase == JS_LOADING
            ? "Loading client snapshot..."
            : "No APs with clients.\nRefresh after sniffing.");
        lv_obj_set_style_text_color(nl, ui_muted_color(), 0);
        lv_obj_set_style_text_font(nl, &lv_font_montserrat_12, 0);
    } else {
        obs_main_build_idx = 0;
        obs_main_build_timer = lv_timer_create(obs_main_build_step, OBS_MAIN_BUILD_MS, NULL);
    }

    /* bottom bar */
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_set_size(bar, LV_PCT(100), 38);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, ui_panel_color(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 3, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* Karma button */
    lv_obj_t *kb = lv_btn_create(bar);
    lv_obj_set_size(kb, 86, 30);
    lv_obj_set_style_bg_color(kb, UI_ACCENT_ORANGE, 0);
    lv_obj_set_style_radius(kb, 8, 0);
    lv_obj_add_event_cb(kb, on_karma_btn, LV_EVENT_CLICKED, NULL);
    lv_obj_t *kl = lv_label_create(kb);
    lv_label_set_text(kl, "Karma");
    lv_obj_set_style_text_color(kl, lv_color_white(), 0);
    lv_obj_set_style_text_font(kl, &lv_font_montserrat_12, 0);
    lv_obj_center(kl);

    /* Refresh button */
    lv_obj_t *rb = lv_btn_create(bar);
    lv_obj_set_size(rb, 86, 30);
    lv_obj_set_style_bg_color(rb, UI_ACCENT_CYAN, 0);
    lv_obj_set_style_radius(rb, 8, 0);
    lv_obj_add_event_cb(rb, on_refresh_btn, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rl = lv_label_create(rb);
    lv_label_set_text(rl, LV_SYMBOL_REFRESH " Refresh");
    lv_obj_set_style_text_color(rl, lv_color_white(), 0);
    lv_obj_set_style_text_font(rl, &lv_font_montserrat_12, 0);
    lv_obj_center(rl);

    /* Stop button */
    lv_obj_t *sb = lv_btn_create(bar);
    lv_obj_set_size(sb, 86, 30);
    lv_obj_set_style_bg_color(sb, UI_ACCENT_RED, 0);
    lv_obj_set_style_radius(sb, 8, 0);
    lv_obj_add_event_cb(sb, on_stop_btn, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(sb);
    lv_label_set_text(sl, LV_SYMBOL_CLOSE " Stop");
    lv_obj_set_style_text_color(sl, lv_color_white(), 0);
    lv_obj_set_style_text_font(sl, &lv_font_montserrat_12, 0);
    lv_obj_center(sl);
}

/* ---- in-place list update (preserves scroll) ---- */
static void rebuild_list_content(void)
{
    if (!obs_list) return;

    int scroll_y = lv_obj_get_scroll_y(obs_list);

    lv_obj_clean(obs_list);

    /* update status label */
    if (status_label) {
        char st[64];
        unsigned aps = 0, clients = 0;
        if (obs_js_lock()) {
            aps = s_js.model.ap_count;
            clients = s_js.model.client_count;
            if (s_js.phase == JS_LOADING)
                snprintf(st, sizeof(st), "Loading clients...");
            else if (s_js.error[0] && (s_js.phase == JS_ERROR || s_js.phase == JS_RELOAD_REQUIRED))
                snprintf(st, sizeof(st), "%s", s_js.error);
            else
                snprintf(st, sizeof(st), "%u APs, %u clients", aps, clients);
            obs_js_unlock();
        } else {
            snprintf(st, sizeof(st), "Observer");
        }
        lv_label_set_text(status_label, st);
    }

    int aps = obs_model_ap_count();
    if (aps == 0) {
        lv_obj_t *nl = lv_label_create(obs_list);
        lv_label_set_text(nl, "No APs with clients.\nRefresh after sniffing.");
        lv_obj_set_style_text_color(nl, ui_muted_color(), 0);
        lv_obj_set_style_text_font(nl, &lv_font_montserrat_12, 0);
    } else {
        for (int i = 0; i < aps; i++)
            obs_main_append_network(i);
    }

    lv_obj_scroll_to_y(obs_list, scroll_y, LV_ANIM_OFF);
}

/* button handlers */
static void on_refresh_btn(lv_event_t *e)
{
    (void)e;
    JsPhase phase = JS_EMPTY;
    if (obs_js_lock()) {
        phase = s_js.phase;
        obs_js_unlock();
    }
    if (phase == JS_LOADING || phase == JS_STARTING || phase == JS_ACTIVE) return;
    obs_load_clients();
}

static void obs_apply_phase_ui(void)
{
    JsPhase phase = JS_EMPTY;
    char err[48] = {0};
    if (obs_js_lock()) {
        phase = s_js.phase;
        snprintf(err, sizeof(err), "%s", s_js.error);
        obs_js_unlock();
    }
    poll_collecting = (phase == JS_LOADING);
    if (phase == JS_ACTIVE) {
        deauth_active = true;
        if (deauth_btn_lbl)
            lv_label_set_text(deauth_btn_lbl, LV_SYMBOL_CLOSE " STOP");
        return;
    }
    if (phase == JS_READY && deauth_btn_lbl && !deauth_active) {
        if (err[0])
            lv_label_set_text(deauth_btn_lbl, err);
        else
            lv_label_set_text(deauth_btn_lbl, "Deauth Station");
    }
    if (obs_list)
        rebuild_list_content();
    else if (phase == JS_READY || phase == JS_ERROR || phase == JS_RELOAD_REQUIRED)
        show_main_view();
}

static void on_stop_btn(lv_event_t *e)
{
    (void)e;
    stop_all();
    if (!ui_display_lock_wait()) return;
    show_home_screen();
    ui_display_unlock_safe();
}

/* ================================================================== */
/*  Karma flow - Phase 1: list_probes                                  */
/* ================================================================== */

static void probe_timeout_cb(void *arg)
{
    (void)arg;
    if (!probe_collecting) return;
    probe_collecting = false;
    uart_set_line_callback(NULL);
    ESP_LOGI(TAG, "Probes collected: %d", probe_count);
    if (!ui_display_lock_wait()) {
        ESP_LOGW(TAG, "probe_timeout_cb: display lock failed");
        return;
    }
    show_probe_picker();
    ui_display_unlock_safe();
}

static void probe_line_cb(const char *line)
{
    if (!probe_collecting) return;

    const char *p = line;
    while (*p == ' ') p++;
    if (!isdigit((unsigned char)*p)) return;

    int idx = 0;
    while (isdigit((unsigned char)*p)) { idx = idx * 10 + (*p - '0'); p++; }
    while (*p == ' ') p++;
    if (*p == '\0') return;

    if (psram_dynarr_ensure((void **)&probes, &probes_cap,
                            probe_count + 1, sizeof(*probes),
                            OBS_PROBE_HARD_CAP)) {
        probes[probe_count].index = idx;
        strncpy(probes[probe_count].ssid, p, 32);
        probes[probe_count].ssid[32] = '\0';
        int l = strlen(probes[probe_count].ssid);
        while (l > 0 && (probes[probe_count].ssid[l-1] == ' ' ||
                         probes[probe_count].ssid[l-1] == '\r'))
            probes[probe_count].ssid[--l] = '\0';
        probe_count++;
        esp_timer_stop(probe_timer);
        esp_timer_start_once(probe_timer, 500000);
    }
}

static void on_karma_btn(lv_event_t *e)
{
    (void)e;
    if (poll_timer) esp_timer_stop(poll_timer);
    uart_set_line_callback(NULL);
    uart_send_command("stop");
    obs_list = NULL;
    status_label = NULL;

    probe_count = 0;
    probe_collecting = true;

    if (!ui_display_lock_wait()) return;
    {
        lv_obj_t *scr = ui_screen_clear();
        ui_create_top_bar(scr, "Karma", on_back_home, NULL);
        lv_obj_t *sp = lv_spinner_create(scr);
        lv_obj_set_size(sp, 40, 40);
        lv_obj_center(sp);
        lv_obj_set_y(sp, 90);
        lv_obj_t *lb = lv_label_create(scr);
        lv_label_set_text(lb, "Loading probes...");
        lv_obj_set_style_text_color(lb, ui_text_color(), 0);
        lv_obj_set_style_text_font(lb, &lv_font_montserrat_14, 0);
        lv_obj_align(lb, LV_ALIGN_CENTER, 0, 45);
    }
    ui_display_unlock_safe();

    ensure_timer(&probe_timer, "obs_probe", probe_timeout_cb);
    uart_set_line_callback(probe_line_cb);
    esp_timer_start_once(probe_timer, 2000000);
    uart_send_command("list_probes");
}

/* ================================================================== */
/*  Karma flow - Probe picker UI                                       */
/* ================================================================== */

static void on_probe_back(lv_event_t *e)
{
    (void)e;
    probe_collecting = false;
    uart_set_line_callback(NULL);
    if (probe_timer) esp_timer_stop(probe_timer);

    obs_running = true;
    resume_sniffer();
    if (!ui_display_lock_wait()) return;
    show_main_view();
    ui_display_unlock_safe();
}

static void on_probe_selected(lv_event_t *e);

static void show_probe_picker(void)
{
    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Karma - Probes", on_probe_back, NULL);

    if (probe_count == 0) {
        lv_obj_t *lb = lv_label_create(scr);
        lv_label_set_text(lb, "No probes captured.\nLet sniffer run longer.");
        lv_obj_set_style_text_color(lb, ui_muted_color(), 0);
        lv_obj_set_style_text_font(lb, &lv_font_montserrat_14, 0);
        lv_obj_set_width(lb, LV_PCT(90));
        lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(lb);
        return;
    }

    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_set_size(list, LV_PCT(100), 240 - 36);
    lv_obj_set_pos(list, 0, 36);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 6, 0);
    lv_obj_set_style_pad_row(list, 4, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    for (int i = 0; i < probe_count; i++) {
        lv_obj_t *btn = lv_btn_create(list);
        lv_obj_set_size(btn, LV_PCT(100), 30);
        lv_obj_set_style_bg_color(btn, ui_card_color(), 0);
        lv_obj_set_style_bg_color(btn, UI_ACCENT_ORANGE, LV_STATE_PRESSED);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_pad_hor(btn, 8, 0);
        lv_obj_add_event_cb(btn, on_probe_selected, LV_EVENT_CLICKED,
                            (void*)(intptr_t)i);

        lv_obj_t *lb = lv_label_create(btn);
        char d[48];
        snprintf(d, sizeof(d), "%d. %s", probes[i].index, probes[i].ssid);
        lv_label_set_text(lb, d);
        lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lb, 270);
        lv_obj_set_style_text_color(lb, ui_text_color(), 0);
        lv_obj_set_style_text_font(lb, &lv_font_montserrat_12, 0);
        lv_obj_align(lb, LV_ALIGN_LEFT_MID, 0, 0);
    }
}

/* ================================================================== */
/*  Karma flow - Phase 2: list_sd                                      */
/* ================================================================== */

static void sd_timeout_cb(void *arg)
{
    (void)arg;
    if (!sd_collecting) return;
    sd_collecting = false;
    uart_set_line_callback(NULL);
    ESP_LOGI(TAG, "SD files: %d", sd_file_count);
    if (!ui_display_lock_wait()) return;
    show_html_picker();
    ui_display_unlock_safe();
}

static void sd_line_cb(const char *line)
{
    if (!sd_collecting) return;
    if (!isdigit((unsigned char)line[0])) return;

    int num = 0;
    const char *p = line;
    while (isdigit((unsigned char)*p)) { num = num * 10 + (*p - '0'); p++; }
    while (*p == ' ') p++;
    if (*p == '\0') return;

    if (sd_file_count < MAX_SD_FILES) {
        sd_files[sd_file_count].number = num;
        strncpy(sd_files[sd_file_count].filename, p, 63);
        sd_files[sd_file_count].filename[63] = '\0';
        int l = strlen(sd_files[sd_file_count].filename);
        while (l > 0 && (sd_files[sd_file_count].filename[l-1] == ' ' ||
                         sd_files[sd_file_count].filename[l-1] == '\r'))
            sd_files[sd_file_count].filename[--l] = '\0';
        sd_file_count++;
        esp_timer_stop(sd_timer);
        esp_timer_start_once(sd_timer, 500000);
    }
}

static void on_probe_selected(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    selected_probe_idx = idx;
    ESP_LOGI(TAG, "Probe: %d -> %s", probes[idx].index, probes[idx].ssid);

    sd_file_count = 0;
    sd_collecting = true;

    if (!ui_display_lock_wait()) return;
    {
        lv_obj_t *scr = ui_screen_clear();
        ui_create_top_bar(scr, "Karma", NULL, NULL);
        lv_obj_t *sp = lv_spinner_create(scr);
        lv_obj_set_size(sp, 40, 40);
        lv_obj_center(sp);
        lv_obj_set_y(sp, 90);
        lv_obj_t *lb = lv_label_create(scr);
        lv_label_set_text(lb, "Loading HTML files...");
        lv_obj_set_style_text_color(lb, ui_text_color(), 0);
        lv_obj_set_style_text_font(lb, &lv_font_montserrat_14, 0);
        lv_obj_align(lb, LV_ALIGN_CENTER, 0, 45);
    }
    ui_display_unlock_safe();

    ensure_timer(&sd_timer, "obs_sd", sd_timeout_cb);
    uart_set_line_callback(sd_line_cb);
    esp_timer_start_once(sd_timer, 2000000);
    uart_send_command("list_sd");
}

/* ================================================================== */
/*  Karma flow - HTML picker UI                                        */
/* ================================================================== */

static void on_html_back(lv_event_t *e)
{
    (void)e;
    sd_collecting = false;
    uart_set_line_callback(NULL);
    if (sd_timer) esp_timer_stop(sd_timer);
    if (!ui_display_lock_wait()) return;
    show_probe_picker();
    ui_display_unlock_safe();
}

static void on_html_selected(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    ESP_LOGI(TAG, "HTML: %d -> %s", sd_files[idx].number, sd_files[idx].filename);

    char cmd[80];
    snprintf(cmd, sizeof(cmd), "select_html %d", sd_files[idx].number);
    uart_send_command(cmd);

    snprintf(cmd, sizeof(cmd), "start_karma %d", probes[selected_probe_idx].index);
    uart_send_command(cmd);

    karma_running = true;
    karma_status_lbl = NULL;
    karma_capture_lbl = NULL;

    if (!ui_display_lock_wait()) return;
    show_karma_running();
    ui_display_unlock_safe();
}

static void show_html_picker(void)
{
    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Karma - HTML", on_html_back, NULL);

    if (sd_file_count == 0) {
        lv_obj_t *lb = lv_label_create(scr);
        lv_label_set_text(lb, "No HTML files on SD.");
        lv_obj_set_style_text_color(lb, ui_muted_color(), 0);
        lv_obj_set_style_text_font(lb, &lv_font_montserrat_14, 0);
        lv_obj_center(lb);
        return;
    }

    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_set_size(list, LV_PCT(100), 240 - 36);
    lv_obj_set_pos(list, 0, 36);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 6, 0);
    lv_obj_set_style_pad_row(list, 4, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    for (int i = 0; i < sd_file_count; i++) {
        lv_obj_t *btn = lv_btn_create(list);
        lv_obj_set_size(btn, LV_PCT(100), 30);
        lv_obj_set_style_bg_color(btn, ui_card_color(), 0);
        lv_obj_set_style_bg_color(btn, UI_ACCENT_ORANGE, LV_STATE_PRESSED);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_pad_hor(btn, 8, 0);
        lv_obj_add_event_cb(btn, on_html_selected, LV_EVENT_CLICKED,
                            (void*)(intptr_t)i);

        lv_obj_t *lb = lv_label_create(btn);
        char d[72];
        snprintf(d, sizeof(d), "%d. %s", sd_files[i].number, sd_files[i].filename);
        lv_label_set_text(lb, d);
        lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lb, 270);
        lv_obj_set_style_text_color(lb, ui_text_color(), 0);
        lv_obj_set_style_text_font(lb, &lv_font_montserrat_12, 0);
        lv_obj_align(lb, LV_ALIGN_LEFT_MID, 0, 0);
    }
}

/* ================================================================== */
/*  Karma flow - Running screen                                        */
/* ================================================================== */

static void karma_line_cb(const char *line)
{
    if (!karma_running) return;

    if (strstr(line, "Captive portal started")) {
        if (!ui_display_lock_wait()) return;
        if (karma_status_lbl)
            lv_label_set_text(karma_status_lbl, "Portal active!");
        ui_display_unlock_safe();
    }

    if (strstr(line, "Client connected")) {
        if (!ui_display_lock_wait()) return;
        if (karma_status_lbl)
            lv_label_set_text(karma_status_lbl, "Client connected!");
        ui_display_unlock_safe();
    }

    const char *pw;
    if ((pw = strstr(line, "Password:")) != NULL) {
        pw += 9;
        while (*pw == ' ') pw++;
        if (!ui_display_lock_wait()) return;
        if (karma_capture_lbl) {
            char t[128];
            snprintf(t, sizeof(t), "Captured: %s", pw);
            lv_label_set_text(karma_capture_lbl, t);
        }
        ui_display_unlock_safe();
    }

    if (strstr(line, "Portal data saved")) {
        if (!ui_display_lock_wait()) return;
        if (karma_status_lbl)
            lv_label_set_text(karma_status_lbl, "Data saved to SD!");
        ui_display_unlock_safe();
    }
}

static void on_karma_stop(lv_event_t *e)
{
    (void)e;
    karma_running = false;
    uart_set_line_callback(NULL);
    uart_send_command("stop");

    obs_running = true;
    resume_sniffer();
    if (!ui_display_lock_wait()) return;
    show_main_view();
    ui_display_unlock_safe();
}

static void show_karma_running(void)
{
    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Karma Running", NULL, NULL);

    lv_obj_t *info = lv_obj_create(scr);
    lv_obj_set_size(info, LV_PCT(100), 140);
    lv_obj_set_pos(info, 0, 36);
    lv_obj_set_style_bg_opa(info, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(info, 0, 0);
    lv_obj_set_style_pad_all(info, 10, 0);
    lv_obj_set_style_pad_row(info, 6, 0);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *sl = lv_label_create(info);
    char stxt[48];
    snprintf(stxt, sizeof(stxt), "SSID: %s", probes[selected_probe_idx].ssid);
    lv_label_set_text(sl, stxt);
    lv_obj_set_style_text_color(sl, UI_ACCENT_ORANGE, 0);
    lv_obj_set_style_text_font(sl, &lv_font_montserrat_14, 0);

    karma_status_lbl = lv_label_create(info);
    lv_label_set_text(karma_status_lbl, "Starting portal...");
    lv_obj_set_style_text_color(karma_status_lbl, UI_ACCENT_CYAN, 0);
    lv_obj_set_style_text_font(karma_status_lbl, &lv_font_montserrat_12, 0);

    karma_capture_lbl = lv_label_create(info);
    lv_label_set_text(karma_capture_lbl, "");
    lv_obj_set_width(karma_capture_lbl, 300);
    lv_label_set_long_mode(karma_capture_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(karma_capture_lbl, UI_ACCENT_GREEN, 0);
    lv_obj_set_style_text_font(karma_capture_lbl, &lv_font_montserrat_12, 0);

    lv_obj_t *btn = lv_btn_create(scr);
    lv_obj_set_size(btn, 140, 36);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(btn, UI_ACCENT_RED, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_add_event_cb(btn, on_karma_stop, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, LV_SYMBOL_CLOSE " Stop");
    lv_obj_set_style_text_color(bl, lv_color_white(), 0);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_14, 0);
    lv_obj_center(bl);

    uart_set_line_callback(karma_line_cb);
}

/* ================================================================== */
/*  Entry point: scan -> sniffer -> main view                          */
/* ================================================================== */

static void observer_apply_main_view(void *unused)
{
    (void)unused;
    show_main_view();
}

static void observer_lvgl_dispatch_job(void *unused)
{
    (void)unused;
    if (!ui_lvgl_async_call(observer_apply_main_view, NULL))
        ESP_LOGW(TAG, "observer: failed to schedule main view");
}

static void on_scan_complete(const char **lines, int count)
{
    ESP_LOGI(TAG, "Scan callback: %d lines", count);
    obs_net_count = 0;
    if (obs_nets && obs_nets_cap > 0) {
        memset(obs_nets, 0, (size_t)obs_nets_cap * sizeof(*obs_nets));
    }

    for (int i = 0; i < count; i++) {
        obs_network_t net;
        memset(&net, 0, sizeof(net));
        if (!parse_scan_line(lines[i], &net)) continue;
        if (!psram_dynarr_ensure((void **)&obs_nets, &obs_nets_cap,
                                 obs_net_count + 1, sizeof(*obs_nets),
                                 OBS_NET_HARD_CAP)) {
            ESP_LOGW(TAG, "obs_nets cap reached at %d", obs_net_count);
            break;
        }
        obs_nets[obs_net_count++] = net;
        ESP_LOGD(TAG, " #%d %s ch%d %ddBm", net.scan_index,
                 net.ssid[0] ? net.ssid : "(hidden)",
                 net.channel, net.rssi);
    }

    sort_networks();
    ESP_LOGI(TAG, "Parsed %d networks", obs_net_count);

    obs_running = true;
    uart_send_command("unselect_networks");
    uart_send_command("start_sniffer");
    obs_load_clients();

    if (!parse_worker_post(observer_lvgl_dispatch_job, NULL)) {
        if (!ui_lvgl_async_call(observer_apply_main_view, NULL))
            ESP_LOGE(TAG, "observer: main view schedule failed");
    }
}

void show_network_observer_screen(void)
{
    stop_all();

    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Observer", on_back_home, NULL);

    lv_obj_t *sp = lv_spinner_create(scr);
    lv_obj_set_size(sp, 50, 50);
    lv_obj_center(sp);
    lv_obj_set_y(sp, 90);

    lv_obj_t *lb = lv_label_create(scr);
    lv_label_set_text(lb, "Scanning networks...");
    lv_obj_set_style_text_color(lb, ui_text_color(), 0);
    lv_obj_set_style_text_font(lb, &lv_font_montserrat_14, 0);
    lv_obj_align(lb, LV_ALIGN_CENTER, 0, 50);

    obs_net_count = 0;
    if (obs_nets && obs_nets_cap > 0) {
        memset(obs_nets, 0, (size_t)obs_nets_cap * sizeof(*obs_nets));
    }

    uart_start_collect("Scan results printed", on_scan_complete);
    uart_send_command("scan_networks");
}
