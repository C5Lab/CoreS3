#include "subghz_scanner_screen.h"
#include "subghz_scanner_settings_screen.h"
#include "subghz_screen.h"
#include "subghz_listen_screen.h"
#include "subghz_rf_settings.h"
#include "ui_helpers.h"
#include "uart_handler.h"
#include "cardkb.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

static const char *TAG = "subghz_scanner";

#define MAX_FREQ_TILES   6
#define UI_TICK_MS       120

typedef struct {
    bool  used;
    float freq;
} scanner_freq_t;

typedef struct {
    lv_obj_t *tile;
    lv_obj_t *freq_lbl;
    lv_obj_t *hint_lbl;
} scanner_tile_view_t;

static scanner_freq_t      s_freqs[MAX_FREQ_TILES];
static float               s_tile_freq[MAX_FREQ_TILES];
static scanner_tile_view_t s_tile_views[MAX_FREQ_TILES];

static volatile bool s_pass_pulse;
static volatile bool s_tiles_dirty;
static volatile bool s_running;
static portMUX_TYPE  s_lock = portMUX_INITIALIZER_UNLOCKED;

static lv_obj_t   *s_dot;
static bool        s_dot_bright;
static lv_timer_t *s_kb_timer;
static lv_timer_t *s_ui_timer;

static void on_back(lv_event_t *e);
static void on_settings(lv_event_t *e);
static void on_tile_clicked(lv_event_t *e);
static void scanner_delete_timers(void);
static void scanner_start_uart(void);
static void scanner_line_cb(const char *line);
static void kb_poll_cb(lv_timer_t *t);
static void ui_tick_cb(lv_timer_t *t);
static void refresh_tiles(void);
static lv_obj_t *make_freq_tile(lv_obj_t *parent, int idx);

static void stop_scanner(void)
{
    if (!s_running) return;
    s_running = false;
    uart_send_command("subghz_stop");
    uart_set_line_callback(NULL);
}

static void scanner_delete_timers(void)
{
    if (s_kb_timer) { lv_timer_delete(s_kb_timer); s_kb_timer = NULL; }
    if (s_ui_timer) { lv_timer_delete(s_ui_timer); s_ui_timer = NULL; }
}

static void scanner_start_uart(void)
{
    subghz_rf_settings_t cfg;
    char cmd[96];

    subghz_rf_settings_load(&cfg);
    subghz_rf_build_scanner_cmd(&cfg, cmd, sizeof(cmd));
    if (cmd[0] == '\0') {
        strncpy(cmd, "subghz_scanner dwell=120 edges=4 -60", sizeof(cmd) - 1);
        cmd[sizeof(cmd) - 1] = '\0';
    }

    uart_stop_collect();
    uart_set_line_callback(scanner_line_cb);
    s_running = true;
    uart_send_command(cmd);
    ESP_LOGI(TAG, "Scanner UART: %s", cmd);
}

static void mru_insert_freq(float freq)
{
    portENTER_CRITICAL(&s_lock);

    int existing = -1;
    for (int i = 0; i < MAX_FREQ_TILES; i++) {
        if (s_freqs[i].used && fabsf(s_freqs[i].freq - freq) < 0.005f) {
            existing = i;
            break;
        }
    }

    int from = (existing >= 0) ? existing : (MAX_FREQ_TILES - 1);
    for (int i = from; i > 0; i--)
        s_freqs[i] = s_freqs[i - 1];

    s_freqs[0].used = true;
    s_freqs[0].freq = freq;

    portEXIT_CRITICAL(&s_lock);

    s_tiles_dirty = true;
}

static void scanner_line_cb(const char *line)
{
    if (!s_running || !line) return;

    if (strstr(line, "[SUBGHZ_SCAN_PASS]")) {
        s_pass_pulse = true;
        return;
    }

    const char *hit = strstr(line, "[SUBGHZ_SCAN_HIT] freq=");
    if (hit) {
        float freq = 0.0f;
        unsigned edges = 0;
        int rssi = 0;
        if (sscanf(hit, "[SUBGHZ_SCAN_HIT] freq=%f edges=%u rssi=%d",
                   &freq, &edges, &rssi) >= 1 && freq > 0.0f) {
            float rounded = roundf(freq * 100.0f) / 100.0f;
            mru_insert_freq(rounded);
        }
        return;
    }
}

static void refresh_tiles(void)
{
    scanner_freq_t snap[MAX_FREQ_TILES];

    portENTER_CRITICAL(&s_lock);
    memcpy(snap, s_freqs, sizeof(snap));
    portEXIT_CRITICAL(&s_lock);

    for (int i = 0; i < MAX_FREQ_TILES; i++) {
        scanner_tile_view_t *v = &s_tile_views[i];
        if (!v->tile) continue;

        if (snap[i].used) {
            s_tile_freq[i] = snap[i].freq;
            int whole = (int)snap[i].freq;
            int frac  = ((int)(snap[i].freq * 100.0f + 0.5f)) % 100;
            lv_label_set_text_fmt(v->freq_lbl, "%d.%02d MHz", whole, frac);
            lv_obj_set_style_text_color(v->freq_lbl, UI_ACCENT_PINK, 0);
            if (v->hint_lbl) {
                lv_label_set_text(v->hint_lbl, "Tap to Listen");
                lv_obj_set_style_text_color(v->hint_lbl, ui_muted_color(), 0);
            }
            lv_obj_add_flag(v->tile, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_color(v->tile, ui_card_color(), LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(v->tile, ui_card_pressed_color(), LV_STATE_PRESSED);
        } else {
            s_tile_freq[i] = 0.0f;
            lv_label_set_text(v->freq_lbl, "--");
            lv_obj_set_style_text_color(v->freq_lbl, ui_muted_color(), 0);
            if (v->hint_lbl) {
                lv_label_set_text(v->hint_lbl, "(empty)");
                lv_obj_set_style_text_color(v->hint_lbl, ui_muted_color(), 0);
            }
            lv_obj_clear_flag(v->tile, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_color(v->tile, ui_panel_color(), LV_STATE_DEFAULT);
        }
    }
}

static void ui_tick_cb(lv_timer_t *t)
{
    (void)t;

    bool pulse = s_pass_pulse;
    bool dirty = s_tiles_dirty;

    if (!pulse && !dirty) return;

    s_pass_pulse = false;
    s_tiles_dirty = false;

    if (!ui_display_lock_wait()) return;

    if (pulse && s_dot) {
        s_dot_bright = !s_dot_bright;
        lv_obj_set_style_bg_opa(s_dot, s_dot_bright ? LV_OPA_COVER : LV_OPA_20, 0);
    }

    if (dirty)
        refresh_tiles();

    ui_display_unlock_safe();
}

static void on_tile_clicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= MAX_FREQ_TILES) return;

    float freq = s_tile_freq[idx];
    if (freq <= 0.0f) return;

    stop_scanner();

    char cmd[32];
    snprintf(cmd, sizeof(cmd), "subghz_freq %.2f", freq);
    uart_send_command(cmd);

    scanner_delete_timers();

    ESP_LOGI(TAG, "Tile clicked: %.2f MHz -> Listen", freq);
    show_subghz_listen_screen_at(freq, true);
}

static void on_settings(lv_event_t *e)
{
    (void)e;
    stop_scanner();
    uart_stop_collect();
    scanner_delete_timers();
    show_subghz_scanner_settings_screen();
}

static void on_back(lv_event_t *e)
{
    (void)e;
    stop_scanner();
    uart_stop_collect();
    scanner_delete_timers();

    show_subghz_screen();
}

static void kb_poll_cb(lv_timer_t *t)
{
    (void)t;
    uint8_t key = cardkb_read_key();
    if (key == 0) return;
    if (key == 0x1B || key == 0x08 || key == 0x7F)
        on_back(NULL);
}

static lv_obj_t *make_freq_tile(lv_obj_t *parent, int idx)
{
    lv_obj_t *tile = lv_btn_create(parent);
    lv_obj_set_size(tile, 150, 54);
    lv_obj_set_style_bg_color(tile, ui_panel_color(), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(tile, ui_card_pressed_color(), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(tile, 0, 0);
    lv_obj_set_style_radius(tile, 8, 0);
    lv_obj_set_style_shadow_width(tile, 0, 0);
    lv_obj_set_style_pad_all(tile, 4, 0);
    lv_obj_set_style_pad_row(tile, 1, 0);
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *freq_lbl = lv_label_create(tile);
    lv_label_set_text(freq_lbl, "--");
    lv_obj_set_style_text_font(freq_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(freq_lbl, ui_muted_color(), 0);

    lv_obj_t *hint_lbl = lv_label_create(tile);
    lv_label_set_text(hint_lbl, "(empty)");
    lv_obj_set_style_text_font(hint_lbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(hint_lbl, ui_muted_color(), 0);

    lv_obj_add_event_cb(tile, on_tile_clicked, LV_EVENT_CLICKED,
                        (void *)(intptr_t)idx);

    s_tile_views[idx].tile = tile;
    s_tile_views[idx].freq_lbl = freq_lbl;
    s_tile_views[idx].hint_lbl = hint_lbl;

    return tile;
}

void show_subghz_scanner_screen(void)
{
    memset(s_freqs, 0, sizeof(s_freqs));
    memset(s_tile_freq, 0, sizeof(s_tile_freq));
    memset(s_tile_views, 0, sizeof(s_tile_views));
    s_pass_pulse  = false;
    s_tiles_dirty = true;
    s_running     = false;
    s_dot         = NULL;
    s_dot_bright  = true;
    s_kb_timer    = NULL;
    s_ui_timer    = NULL;

    lv_obj_t *scr = ui_screen_clear();

    lv_obj_t *bar = ui_create_top_bar(scr, "Scanner", on_back, NULL);
    {
        lv_obj_t *title = lv_obj_get_child(bar, 1);
        if (title) {
            lv_obj_set_flex_grow(title, 0);
            lv_label_set_long_mode(title, LV_LABEL_LONG_CLIP);
        }
    }

    lv_obj_t *spacer = lv_obj_create(bar);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_height(spacer, 1);

    lv_obj_t *scan_lbl = lv_label_create(bar);
    lv_label_set_text(scan_lbl, "Scanning");
    lv_obj_set_style_text_font(scan_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(scan_lbl, ui_muted_color(), 0);

    s_dot = lv_obj_create(bar);
    lv_obj_remove_style_all(s_dot);
    lv_obj_set_size(s_dot, 14, 14);
    lv_obj_set_style_bg_color(s_dot, UI_ACCENT_RED, 0);
    lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_margin_left(s_dot, 6, 0);
    lv_obj_set_style_margin_right(s_dot, 4, 0);

    ui_add_top_bar_action(bar, LV_SYMBOL_SETTINGS, on_settings, NULL);

    lv_obj_t *grid = lv_obj_create(scr);
    lv_obj_set_size(grid, LV_PCT(100), 240 - 36);
    lv_obj_set_y(grid, 36);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 6, 0);
    lv_obj_set_style_pad_row(grid, 6, 0);
    lv_obj_set_style_pad_column(grid, 6, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < MAX_FREQ_TILES; i++)
        make_freq_tile(grid, i);

    refresh_tiles();

    s_kb_timer = lv_timer_create(kb_poll_cb, 50, NULL);
    s_ui_timer = lv_timer_create(ui_tick_cb, UI_TICK_MS, NULL);

    scanner_start_uart();

    ESP_LOGI(TAG, "SubGHz Scanner screen ready");
}
