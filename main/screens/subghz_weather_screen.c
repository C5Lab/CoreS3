#include "subghz_weather_screen.h"
#include "subghz_screen.h"
#include "ui_helpers.h"
#include "uart_handler.h"
#include "cardkb.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "subghz_weather";

#define MAX_SENSORS   8
#define UI_TICK_MS    250

typedef struct {
    bool          used;
    char          proto[24];
    unsigned long id;
    char          ch[8];
    char          temp[16];
    char          hum[8];
    char          batt[8];
    TickType_t    last_seen;
} weather_sensor_t;

typedef struct {
    lv_obj_t *tile;
    lv_obj_t *proto_lbl;
    lv_obj_t *age_lbl;
    lv_obj_t *temp_lbl;
    lv_obj_t *hum_lbl;
    lv_obj_t *batt_lbl;
    lv_obj_t *empty_lbl;
} weather_tile_view_t;

static weather_sensor_t    s_sensors[MAX_SENSORS];
static weather_tile_view_t s_tile_views[MAX_SENSORS];

static portMUX_TYPE  s_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_dirty;
static volatile bool s_running;
static volatile bool s_rx_pulse;
static float         s_listen_freq;

static lv_obj_t   *s_status_lbl;
static lv_obj_t   *s_dot;
static bool        s_dot_bright;
static lv_timer_t *s_kb_timer;
static lv_timer_t *s_ui_timer;

static void on_back(lv_event_t *e);
static void weather_line_cb(const char *line);
static void kb_poll_cb(lv_timer_t *t);
static void ui_tick_cb(lv_timer_t *t);
static void refresh_tiles(void);
static void update_status_label(int sensor_count);
static void build_tile(lv_obj_t *parent, int idx);

static void stop_weather(void)
{
    if (!s_running) return;
    s_running = false;
    uart_send_command("subghz_stop");
    uart_set_line_callback(NULL);
}

static int find_sensor_slot_unlocked(const char *proto, unsigned long id, const char *ch)
{
    for (int i = 0; i < MAX_SENSORS; i++) {
        if (s_sensors[i].used &&
            s_sensors[i].id == id &&
            strncmp(s_sensors[i].proto, proto, sizeof(s_sensors[i].proto)) == 0 &&
            strncmp(s_sensors[i].ch, ch, sizeof(s_sensors[i].ch)) == 0) {
            return i;
        }
    }
    return -1;
}

static int pick_free_or_lru_slot_unlocked(void)
{
    for (int i = 0; i < MAX_SENSORS; i++) {
        if (!s_sensors[i].used) return i;
    }
    int lru = 0;
    for (int i = 1; i < MAX_SENSORS; i++) {
        if ((int32_t)(s_sensors[i].last_seen - s_sensors[lru].last_seen) < 0)
            lru = i;
    }
    return lru;
}

static void weather_upsert(const char *proto, unsigned long id, const char *ch,
                           const char *temp, const char *hum, const char *batt)
{
    portENTER_CRITICAL(&s_lock);

    int slot = find_sensor_slot_unlocked(proto, id, ch);
    if (slot < 0)
        slot = pick_free_or_lru_slot_unlocked();

    weather_sensor_t *s = &s_sensors[slot];
    s->used = true;
    s->id = id;
    snprintf(s->proto, sizeof(s->proto), "%s", proto);
    snprintf(s->ch,    sizeof(s->ch),    "%s", ch);
    snprintf(s->temp,  sizeof(s->temp),  "%s", temp);
    snprintf(s->hum,   sizeof(s->hum),   "%s", hum);
    snprintf(s->batt,  sizeof(s->batt),  "%s", batt);
    s->last_seen = xTaskGetTickCount();

    portEXIT_CRITICAL(&s_lock);

    s_rx_pulse = true;
    s_dirty = true;
}

static void weather_line_cb(const char *line)
{
    if (!s_running || !line) return;

    if (strstr(line, "[SUBGHZ_WEATHER_START]")) {
        float f = 0.0f;
        if (sscanf(line, "[SUBGHZ_WEATHER_START] freq=%f", &f) == 1)
            s_listen_freq = f;
        s_dirty = true;
        return;
    }

    const char *p = strstr(line, "[SUBGHZ_WEATHER] proto=");
    if (!p) return;

    char proto[24] = {0};
    char ch[8]     = {0};
    char temp[16]  = {0};
    char hum[8]    = {0};
    char batt[8]   = {0};
    unsigned long id = 0;

    if (sscanf(p,
               "[SUBGHZ_WEATHER] proto=%23s id=0x%lX ch=%7s temp=%15s hum=%7s batt=%7s",
               proto, &id, ch, temp, hum, batt) == 6) {
        weather_upsert(proto, id, ch, temp, hum, batt);
    }
}

static lv_color_t batt_color(const char *batt)
{
    if (strcmp(batt, "ok") == 0)  return UI_ACCENT_GREEN;
    if (strcmp(batt, "low") == 0) return UI_ACCENT_ORANGE;
    return ui_muted_color();
}

static void apply_sensor_to_tile(weather_tile_view_t *v, const weather_sensor_t *s)
{
    if (!v->tile) return;

    if (!s->used) {
        lv_obj_set_style_bg_color(v->tile, ui_panel_color(), LV_STATE_DEFAULT);
        if (v->proto_lbl) lv_obj_add_flag(v->proto_lbl, LV_OBJ_FLAG_HIDDEN);
        if (v->age_lbl)   lv_obj_add_flag(v->age_lbl,   LV_OBJ_FLAG_HIDDEN);
        if (v->temp_lbl)  lv_obj_add_flag(v->temp_lbl,  LV_OBJ_FLAG_HIDDEN);
        if (v->hum_lbl)   lv_obj_add_flag(v->hum_lbl,   LV_OBJ_FLAG_HIDDEN);
        if (v->batt_lbl)  lv_obj_add_flag(v->batt_lbl,  LV_OBJ_FLAG_HIDDEN);
        if (v->empty_lbl) lv_obj_clear_flag(v->empty_lbl, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_set_style_bg_color(v->tile, ui_card_color(), LV_STATE_DEFAULT);
    if (v->empty_lbl) lv_obj_add_flag(v->empty_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(v->proto_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(v->age_lbl,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(v->temp_lbl,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(v->hum_lbl,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(v->batt_lbl,  LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text(v->proto_lbl, s->proto);

    if (strcmp(s->temp, "-") == 0)
        lv_label_set_text(v->temp_lbl, "-- C");
    else
        lv_label_set_text_fmt(v->temp_lbl, "%s C", s->temp);

    if (strcmp(s->hum, "-") == 0)
        lv_label_set_text(v->hum_lbl, "--");
    else
        lv_label_set_text_fmt(v->hum_lbl, "%s%%", s->hum);

    lv_label_set_text(v->batt_lbl, s->batt);
    lv_obj_set_style_text_color(v->batt_lbl, batt_color(s->batt), 0);
}

static void refresh_tiles(void)
{
    weather_sensor_t snap[MAX_SENSORS];
    int count = 0;

    portENTER_CRITICAL(&s_lock);
    memcpy(snap, s_sensors, sizeof(snap));
    portEXIT_CRITICAL(&s_lock);

    for (int i = 0; i < MAX_SENSORS; i++) {
        if (snap[i].used) count++;
        apply_sensor_to_tile(&s_tile_views[i], &snap[i]);
    }

    update_status_label(count);
}

static void format_age(uint32_t secs, char *buf, size_t sz)
{
    if (secs < 60)          snprintf(buf, sz, "%us", (unsigned)secs);
    else if (secs < 3600)   snprintf(buf, sz, "%um", (unsigned)(secs / 60));
    else if (secs < 86400)  snprintf(buf, sz, "%uh", (unsigned)(secs / 3600));
    else                    snprintf(buf, sz, "%ud", (unsigned)(secs / 86400));
}

static void refresh_ages(void)
{
    TickType_t now = xTaskGetTickCount();
    struct { bool used; TickType_t last; } snap[MAX_SENSORS];

    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < MAX_SENSORS; i++) {
        snap[i].used = s_sensors[i].used;
        snap[i].last = s_sensors[i].last_seen;
    }
    portEXIT_CRITICAL(&s_lock);

    for (int i = 0; i < MAX_SENSORS; i++) {
        weather_tile_view_t *v = &s_tile_views[i];
        if (!v->age_lbl) continue;
        if (!snap[i].used) continue;

        uint32_t ms = (uint32_t)((now - snap[i].last) * portTICK_PERIOD_MS);
        uint32_t secs = ms / 1000U;
        char buf[12];
        format_age(secs, buf, sizeof(buf));
        lv_label_set_text(v->age_lbl, buf);
    }
}

static void update_status_label(int sensor_count)
{
    if (!s_status_lbl) return;
    if (s_listen_freq > 0.0f) {
        int whole = (int)s_listen_freq;
        int frac  = ((int)(s_listen_freq * 100.0f + 0.5f)) % 100;
        lv_label_set_text_fmt(s_status_lbl, "%d.%02d MHz  -  %d sensor%s",
                              whole, frac, sensor_count,
                              sensor_count == 1 ? "" : "s");
    } else {
        lv_label_set_text_fmt(s_status_lbl, "Listening...  -  %d sensor%s",
                              sensor_count, sensor_count == 1 ? "" : "s");
    }
}

static void ui_tick_cb(lv_timer_t *t)
{
    (void)t;

    bool pulse = s_rx_pulse;
    bool dirty = s_dirty;
    s_rx_pulse = false;
    s_dirty = false;

    if (!ui_display_lock_wait()) return;

    if (pulse && s_dot) {
        s_dot_bright = !s_dot_bright;
        lv_obj_set_style_bg_opa(s_dot, s_dot_bright ? LV_OPA_COVER : LV_OPA_20, 0);
    }

    if (dirty)
        refresh_tiles();

    refresh_ages();

    ui_display_unlock_safe();
}

static void on_back(lv_event_t *e)
{
    (void)e;
    stop_weather();
    uart_stop_collect();

    if (s_kb_timer) { lv_timer_delete(s_kb_timer); s_kb_timer = NULL; }
    if (s_ui_timer) { lv_timer_delete(s_ui_timer); s_ui_timer = NULL; }

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

static void build_tile(lv_obj_t *parent, int idx)
{
    lv_obj_t *tile = lv_obj_create(parent);
    lv_obj_set_size(tile, 150, 56);
    lv_obj_set_style_bg_color(tile, ui_panel_color(), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(tile, 0, 0);
    lv_obj_set_style_radius(tile, 8, 0);
    lv_obj_set_style_shadow_width(tile, 0, 0);
    lv_obj_set_style_pad_all(tile, 4, 0);
    lv_obj_set_style_pad_gap(tile, 1, 0);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *empty = lv_label_create(tile);
    lv_label_set_text(empty, "--");
    lv_obj_set_style_text_font(empty, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(empty, ui_muted_color(), 0);
    lv_obj_center(empty);

    lv_obj_t *proto = lv_label_create(tile);
    lv_label_set_text(proto, "");
    lv_obj_set_style_text_font(proto, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(proto, UI_ACCENT_CYAN, 0);
    lv_obj_set_pos(proto, 2, 2);
    lv_obj_set_width(proto, 90);
    lv_label_set_long_mode(proto, LV_LABEL_LONG_CLIP);
    lv_obj_add_flag(proto, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *age = lv_label_create(tile);
    lv_label_set_text(age, "");
    lv_obj_set_style_text_font(age, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(age, ui_muted_color(), 0);
    lv_obj_align(age, LV_ALIGN_TOP_RIGHT, -2, 4);
    lv_label_set_long_mode(age, LV_LABEL_LONG_CLIP);
    lv_obj_add_flag(age, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *temp = lv_label_create(tile);
    lv_label_set_text(temp, "");
    lv_obj_set_style_text_font(temp, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(temp, ui_text_color(), 0);
    lv_obj_align(temp, LV_ALIGN_BOTTOM_LEFT, 0, -2);
    lv_obj_add_flag(temp, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *hum = lv_label_create(tile);
    lv_label_set_text(hum, "");
    lv_obj_set_style_text_font(hum, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(hum, ui_muted_color(), 0);
    lv_obj_align(hum, LV_ALIGN_BOTTOM_MID, 16, -4);
    lv_obj_add_flag(hum, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *batt = lv_label_create(tile);
    lv_label_set_text(batt, "");
    lv_obj_set_style_text_font(batt, &lv_font_montserrat_10, 0);
    lv_obj_align(batt, LV_ALIGN_BOTTOM_RIGHT, -2, -4);
    lv_obj_add_flag(batt, LV_OBJ_FLAG_HIDDEN);

    weather_tile_view_t *v = &s_tile_views[idx];
    v->tile      = tile;
    v->empty_lbl = empty;
    v->proto_lbl = proto;
    v->age_lbl   = age;
    v->temp_lbl  = temp;
    v->hum_lbl   = hum;
    v->batt_lbl  = batt;
}

void show_subghz_weather_screen(void)
{
    memset(s_sensors,    0, sizeof(s_sensors));
    memset(s_tile_views, 0, sizeof(s_tile_views));
    s_dirty        = true;
    s_running      = false;
    s_rx_pulse     = false;
    s_listen_freq  = 0.0f;
    s_status_lbl   = NULL;
    s_dot          = NULL;
    s_dot_bright   = true;
    s_kb_timer     = NULL;
    s_ui_timer     = NULL;

    lv_obj_t *scr = ui_screen_clear();

    lv_obj_t *bar = ui_create_top_bar(scr, "Weather", on_back, NULL);

    lv_obj_t *bar_spacer = lv_obj_create(bar);
    lv_obj_remove_style_all(bar_spacer);
    lv_obj_set_flex_grow(bar_spacer, 1);
    lv_obj_set_height(bar_spacer, 1);

    s_dot = lv_obj_create(bar);
    lv_obj_remove_style_all(s_dot);
    lv_obj_set_size(s_dot, 12, 12);
    lv_obj_set_style_bg_color(s_dot, UI_ACCENT_GREEN, 0);
    lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_margin_left(s_dot, 6, 0);
    lv_obj_set_style_margin_right(s_dot, 6, 0);

    s_status_lbl = lv_label_create(scr);
    lv_obj_set_pos(s_status_lbl, 8, 40);
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_status_lbl, UI_ACCENT_CYAN, 0);
    lv_label_set_text(s_status_lbl, "Listening...  -  0 sensors");

    lv_obj_t *grid = lv_obj_create(scr);
    lv_obj_set_size(grid, LV_PCT(100), 240 - 60);
    lv_obj_set_y(grid, 60);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 4, 0);
    lv_obj_set_style_pad_row(grid, 4, 0);
    lv_obj_set_style_pad_column(grid, 4, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < MAX_SENSORS; i++)
        build_tile(grid, i);

    refresh_tiles();

    s_kb_timer = lv_timer_create(kb_poll_cb, 50, NULL);
    s_ui_timer = lv_timer_create(ui_tick_cb, UI_TICK_MS, NULL);

    uart_stop_collect();
    uart_set_line_callback(weather_line_cb);
    s_running = true;
    uart_send_command("subghz_weather");

    ESP_LOGI(TAG, "SubGHz Weather screen ready");
}
