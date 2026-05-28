#include "ap_radar_screen.h"
#include "attack_select_screen.h"
#include "wifi_scan_screen.h"
#include "ui_helpers.h"
#include "uart_handler.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *TAG = "ap_radar";

#define RADAR_RSSI_CENTER  -30
#define RADAR_RSSI_EDGE    -90

static lv_obj_t *s_panel = NULL;
static lv_obj_t *s_rssi_lbl = NULL;
static int s_rssi = -100;
static int s_sweep = 0;
static lv_timer_t *s_sweep_timer = NULL;
static bool s_running = false;

static float rssi_to_dist(int rssi)
{
    if (rssi >= RADAR_RSSI_CENTER) return 0.0f;
    if (rssi <= RADAR_RSSI_EDGE) return 1.0f;
    return (float)(RADAR_RSSI_CENTER - rssi) /
           (float)(RADAR_RSSI_CENTER - RADAR_RSSI_EDGE);
}

static void on_back(lv_event_t *e)
{
    (void)e;
    s_running = false;
    uart_send_command("stop");
    uart_set_line_callback(NULL);
    if (s_sweep_timer) {
        lv_timer_delete(s_sweep_timer);
        s_sweep_timer = NULL;
    }
    show_attack_select_screen();
}

static void radar_line_cb(const char *line)
{
    if (!s_running || !line) return;
    const char *p = strstr(line, "[AP Locator]");
    if (!p) return;
    const char *r = strstr(line, "RSSI:");
    if (!r) return;
    s_rssi = atoi(r + 5);
    if (s_rssi_lbl && ui_display_lock_wait()) {
        char buf[32];
        snprintf(buf, sizeof(buf), "RSSI: %d dBm", s_rssi);
        lv_label_set_text(s_rssi_lbl, buf);
        if (s_panel) lv_obj_invalidate(s_panel);
        ui_display_unlock_safe();
    }
}

static void sweep_cb(lv_timer_t *t)
{
    (void)t;
    s_sweep = (s_sweep + 15) % 360;
    if (s_panel) lv_obj_invalidate(s_panel);
}

static void panel_draw_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN) return;

    lv_obj_t *obj = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);

    int32_t w = lv_area_get_width(&coords);
    int32_t h = lv_area_get_height(&coords);
    int32_t cx = coords.x1 + w / 2;
    int32_t cy = coords.y1 + h / 2;
    int32_t max_r = (w < h ? w : h) / 2 - 8;
    if (max_r < 16) max_r = 16;

    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.base.layer = layer;
    line_dsc.color = lv_color_hex(0x1E4D3A);
    line_dsc.width = 1;

    for (int ring = 1; ring <= 3; ring++) {
        int32_t r = max_r * ring / 3;
        line_dsc.p1.x = cx - r;
        line_dsc.p1.y = cy;
        line_dsc.p2.x = cx + r;
        line_dsc.p2.y = cy;
        lv_draw_line(layer, &line_dsc);
        line_dsc.p1.x = cx;
        line_dsc.p1.y = cy - r;
        line_dsc.p2.x = cx;
        line_dsc.p2.y = cy + r;
        lv_draw_line(layer, &line_dsc);
    }

    float dist = rssi_to_dist(s_rssi);
    int32_t blip_r = (int32_t)(max_r * dist);
    float rad = (float)s_sweep * (float)M_PI / 180.0f;
    int32_t bx = cx + (int32_t)(blip_r * cosf(rad));
    int32_t by = cy + (int32_t)(blip_r * sinf(rad));

    lv_draw_rect_dsc_t rect_dsc;
    lv_draw_rect_dsc_init(&rect_dsc);
    rect_dsc.base.layer = layer;
    rect_dsc.bg_color = s_rssi > -50 ? UI_ACCENT_GREEN :
                        (s_rssi > -70 ? UI_ACCENT_ORANGE : UI_ACCENT_RED);
    rect_dsc.bg_opa = LV_OPA_COVER;
    rect_dsc.radius = LV_RADIUS_CIRCLE;

    lv_area_t blip = { bx - 4, by - 4, bx + 4, by + 4 };
    lv_draw_rect(layer, &rect_dsc, &blip);
}

void show_ap_radar_screen(void)
{
    s_running = true;
    s_rssi = -100;

    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "AP Radar", on_back, NULL);

    s_panel = lv_obj_create(scr);
    lv_obj_set_size(s_panel, 180, 150);
    lv_obj_align(s_panel, LV_ALIGN_CENTER, 0, -10);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(0x051A10), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_panel, UI_ACCENT_GREEN, 0);
    lv_obj_set_style_border_width(s_panel, 1, 0);
    lv_obj_add_event_cb(s_panel, panel_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    s_rssi_lbl = lv_label_create(scr);
    lv_label_set_text(s_rssi_lbl, "RSSI: -- dBm");
    lv_obj_set_style_text_color(s_rssi_lbl, ui_text_color(), 0);
    lv_obj_align(s_rssi_lbl, LV_ALIGN_BOTTOM_MID, 0, -8);

    if (!s_sweep_timer)
        s_sweep_timer = lv_timer_create(sweep_cb, 80, NULL);

    uart_set_line_callback(radar_line_cb);
}
