#include "subghz_listen_screen.h"
#include "subghz_screen.h"
#include "subghz_parser.h"
#include "ui_helpers.h"
#include "uart_handler.h"
#include "cardkb.h"
#include "led_indicator.h"
#include "esp_log.h"
#include "bsp/m5stack_core_s3.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static const char *TAG = "subghz_listen";

#define WATERFALL_W       320
#define WATERFALL_H       40
#define WATERFALL_TICK_MS 120
#define SIGNAL_CHUNK_CAPACITY 64
#define SIGNAL_ROW_HEIGHT 18
#define SIGNAL_ROW_POOL_SIZE 10
/* Cap virtual scroll height so lv_coord / layout stay stable with huge captures. */
#define SIGNAL_VSCROLL_MAX_PX 28000
#define COL_IDX_W         22
#define COL_TYPE_W        50
#define COL_FREQ_W        50
#define COL_MF_W          90
#define COL_SER_W         55

static int64_t listen_real_content_h(size_t total)
{
    return (int64_t)total * (int64_t)SIGNAL_ROW_HEIGHT;
}

static lv_coord_t listen_virt_content_h(size_t total)
{
    int64_t r = listen_real_content_h(total);
    if (r > SIGNAL_VSCROLL_MAX_PX)
        return (lv_coord_t)SIGNAL_VSCROLL_MAX_PX;
    return (lv_coord_t)r;
}

static size_t listen_scroll_to_first_index(lv_coord_t scroll_y, size_t total)
{
    if (total == 0)
        return 0;
    int64_t real = listen_real_content_h(total);
    lv_coord_t virt = listen_virt_content_h(total);
    if (real <= (int64_t)virt)
        return (size_t)scroll_y / SIGNAL_ROW_HEIGHT;
    int64_t eff = (int64_t)scroll_y * real / (int64_t)virt;
    size_t idx = (size_t)(eff / SIGNAL_ROW_HEIGHT);
    if (idx >= total)
        idx = total - 1;
    return idx;
}

static lv_coord_t listen_row_y(size_t signal_index, size_t total)
{
    if (total == 0)
        return 0;
    lv_coord_t virt = listen_virt_content_h(total);
    int64_t real = listen_real_content_h(total);
    if (real <= (int64_t)virt)
        return (lv_coord_t)((int64_t)signal_index * SIGNAL_ROW_HEIGHT);
    if (total == 1)
        return 0;
    return (lv_coord_t)((int64_t)signal_index * ((int64_t)virt - SIGNAL_ROW_HEIGHT) / (int64_t)(total - 1));
}

typedef struct {
    int   idx;
    char  type[32];
    float freq;
    char  serial[32];
    int   btn;
    int   cnt;
    char  mf[32];
    bool  is_raw;
} subghz_signal_t;

typedef struct subghz_signal_chunk {
    struct subghz_signal_chunk *next;
    size_t used;
    subghz_signal_t items[SIGNAL_CHUNK_CAPACITY];
} subghz_signal_chunk_t;

typedef struct {
    lv_obj_t *row;
    lv_obj_t *idx;
    lv_obj_t *type;
    lv_obj_t *freq;
    lv_obj_t *mf;
    lv_obj_t *serial;
} signal_row_view_t;

static subghz_signal_chunk_t *s_signal_head;
static subghz_signal_chunk_t *s_signal_tail;
static subghz_signal_t       *s_last_signal;
static size_t                 s_signal_count;
static volatile bool          s_running;
static bool            s_raw_mode;
static bool            s_follow_latest;
static volatile bool   s_history_dirty;
static volatile bool   s_activity_pending;
static volatile bool   s_psram_exhausted;
static float           s_freq_mhz = 433.92f;
static portMUX_TYPE    s_signal_lock = portMUX_INITIALIZER_UNLOCKED;

static bool            s_pending_autostart;

static lv_obj_t   *s_canvas;
static lv_color_t *s_canvas_buf;
static lv_obj_t   *s_sig_list;
static lv_obj_t   *s_sig_spacer;
static lv_obj_t   *s_empty_lbl;
static lv_obj_t   *s_sig_count_lbl;
static lv_obj_t   *s_freq_lbl;
static lv_obj_t   *s_btn_start_stop;
static lv_obj_t   *s_btn_raw;
static lv_obj_t   *s_freq_popup;
static lv_obj_t   *s_rollers[5];
static signal_row_view_t s_row_pool[SIGNAL_ROW_POOL_SIZE];
static lv_timer_t *s_kb_timer;
static lv_timer_t *s_ui_timer;

static void on_back(lv_event_t *e);
static void on_start_stop(lv_event_t *e);
static void on_raw_toggle(lv_event_t *e);
static void on_freq_tap(lv_event_t *e);
static void subghz_line_cb(const char *line);
static void kb_poll_cb(lv_timer_t *t);
static void ui_tick_cb(lv_timer_t *t);
static void on_signal_list_scroll(lv_event_t *e);
static void refresh_signal_list_view(void);
static void reset_capture_session(void);
static void clear_signal_history(void);
static void fill_signal(subghz_signal_t *dst, const subghz_signal_info_t *src);
static bool merge_duplicate_signal(const subghz_signal_info_t *src);

static size_t signal_count_snapshot(void)
{
    size_t count;

    portENTER_CRITICAL(&s_signal_lock);
    count = s_signal_count;
    portEXIT_CRITICAL(&s_signal_lock);

    return count;
}

#define WF_BG_COLOR    0x0A1628
#define WF_GRID_COLOR  0x152540

static void waterfall_fill_bg(void)
{
    lv_color_t bg   = lv_color_hex(WF_BG_COLOR);
    lv_color_t grid = lv_color_hex(WF_GRID_COLOR);
    for (int y = 0; y < WATERFALL_H; y++) {
        lv_color_t c = (y % 10 == 0) ? grid : bg;
        for (int x = 0; x < WATERFALL_W; x++)
            s_canvas_buf[y * WATERFALL_W + x] = c;
    }
}

static void waterfall_push_activity(bool active)
{
    if (!s_canvas || !s_canvas_buf) return;

    for (int y = 0; y < WATERFALL_H; y++)
        for (int x = 0; x < WATERFALL_W - 1; x++)
            s_canvas_buf[y * WATERFALL_W + x] = s_canvas_buf[y * WATERFALL_W + x + 1];

    lv_color_t bg   = lv_color_hex(WF_BG_COLOR);
    lv_color_t grid = lv_color_hex(WF_GRID_COLOR);
    lv_color_t bar  = lv_color_hex(0xFF0000);

    for (int y = 0; y < WATERFALL_H; y++) {
        if (active)
            s_canvas_buf[y * WATERFALL_W + WATERFALL_W - 1] = bar;
        else
            s_canvas_buf[y * WATERFALL_W + WATERFALL_W - 1] = (y % 10 == 0) ? grid : bg;
    }

    lv_obj_invalidate(s_canvas);
}

static void fill_signal(subghz_signal_t *dst, const subghz_signal_info_t *src)
{
    memset(dst, 0, sizeof(*dst));
    dst->idx = src->idx;
    dst->freq = src->freq;
    dst->btn = src->btn;
    dst->cnt = src->cnt;
    dst->is_raw = src->is_raw;
    snprintf(dst->type, sizeof(dst->type), "%s", src->type[0] ? src->type : "--");
    snprintf(dst->serial, sizeof(dst->serial), "%s", src->serial[0] ? src->serial : "--");
    snprintf(dst->mf, sizeof(dst->mf), "%s", src->mf[0] ? src->mf : "--");
}

static subghz_signal_chunk_t *alloc_signal_chunk(void)
{
    subghz_signal_chunk_t *chunk = heap_caps_malloc(sizeof(*chunk),
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!chunk)
        return NULL;

    memset(chunk, 0, sizeof(*chunk));
    return chunk;
}

static bool append_signal_history(const subghz_signal_t *sig)
{
    subghz_signal_chunk_t *new_chunk = NULL;
    subghz_signal_t *inserted;

    if (!sig)
        return false;

    if (!s_signal_tail || s_signal_tail->used >= SIGNAL_CHUNK_CAPACITY) {
        new_chunk = alloc_signal_chunk();
        if (!new_chunk) {
            if (!s_psram_exhausted)
                ESP_LOGE(TAG, "PSRAM exhausted while storing captured signals");
            s_psram_exhausted = true;
            s_history_dirty = true;
            return false;
        }
    }

    portENTER_CRITICAL(&s_signal_lock);
    if (!s_signal_head) {
        s_signal_head = new_chunk;
        s_signal_tail = new_chunk;
        new_chunk = NULL;
    } else if (new_chunk) {
        s_signal_tail->next = new_chunk;
        s_signal_tail = new_chunk;
        new_chunk = NULL;
    }

    inserted = &s_signal_tail->items[s_signal_tail->used++];
    *inserted = *sig;
    s_last_signal = inserted;
    s_signal_count++;
    portEXIT_CRITICAL(&s_signal_lock);

    if (new_chunk)
        free(new_chunk);

    s_history_dirty = true;
    return true;
}

static bool merge_duplicate_signal(const subghz_signal_info_t *src)
{
    bool merged = false;

    if (!src || !src->is_duplicate)
        return false;

    portENTER_CRITICAL(&s_signal_lock);
    if (s_last_signal && !s_last_signal->is_raw) {
        if ((src->idx > 0 && s_last_signal->idx == src->idx) ||
            (strcmp(s_last_signal->type, src->type) == 0 &&
             strcmp(s_last_signal->serial, src->serial) == 0 &&
             s_last_signal->btn == src->btn)) {
            if (src->cnt > 0)
                s_last_signal->cnt = src->cnt;
            merged = true;
        }
    }
    portEXIT_CRITICAL(&s_signal_lock);

    if (merged)
        s_history_dirty = true;

    return merged;
}

static void clear_signal_history(void)
{
    subghz_signal_chunk_t *head;

    portENTER_CRITICAL(&s_signal_lock);
    head = s_signal_head;
    s_signal_head = NULL;
    s_signal_tail = NULL;
    s_last_signal = NULL;
    s_signal_count = 0;
    portEXIT_CRITICAL(&s_signal_lock);

    while (head) {
        subghz_signal_chunk_t *next = head->next;
        free(head);
        head = next;
    }
}

static void copy_signal_window(size_t first_index, subghz_signal_t *out,
                               size_t max_items, size_t *out_count,
                               size_t *out_total)
{
    size_t copied = 0;
    size_t base = 0;
    subghz_signal_chunk_t *chunk;
    size_t offset;

    portENTER_CRITICAL(&s_signal_lock);
    if (out_total)
        *out_total = s_signal_count;

    chunk = s_signal_head;
    while (chunk && first_index >= base + chunk->used) {
        base += chunk->used;
        chunk = chunk->next;
    }

    offset = first_index - base;
    while (chunk && copied < max_items) {
        while (offset < chunk->used && copied < max_items) {
            out[copied++] = chunk->items[offset++];
        }
        chunk = chunk->next;
        offset = 0;
    }
    portEXIT_CRITICAL(&s_signal_lock);

    if (out_count)
        *out_count = copied;
}

static void update_signal_count_label(size_t count)
{
    if (!s_sig_count_lbl)
        return;

    if (s_psram_exhausted) {
        lv_label_set_text_fmt(s_sig_count_lbl, "Sig: %lu MEM", (unsigned long)count);
        lv_obj_set_style_text_color(s_sig_count_lbl, UI_ACCENT_RED, 0);
    } else {
        lv_label_set_text_fmt(s_sig_count_lbl, "Sig: %lu", (unsigned long)count);
        lv_obj_set_style_text_color(s_sig_count_lbl, UI_ACCENT_CYAN, 0);
    }
}

static void configure_signal_row(signal_row_view_t *view)
{
    if (!view || !s_sig_list)
        return;

    view->row = lv_obj_create(s_sig_list);
    lv_obj_set_size(view->row, LV_PCT(100), SIGNAL_ROW_HEIGHT - 1);
    lv_obj_set_style_pad_all(view->row, 1, 0);
    lv_obj_set_style_pad_gap(view->row, 2, 0);
    lv_obj_set_style_bg_color(view->row, ui_card_color(), 0);
    lv_obj_set_style_bg_opa(view->row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(view->row, 0, 0);
    lv_obj_set_style_radius(view->row, 3, 0);
    lv_obj_set_style_min_height(view->row, SIGNAL_ROW_HEIGHT - 1, 0);
    lv_obj_set_flex_flow(view->row, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(view->row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(view->row, LV_OBJ_FLAG_HIDDEN);

    view->idx = lv_label_create(view->row);
    lv_obj_set_width(view->idx, COL_IDX_W);
    lv_obj_set_style_text_font(view->idx, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(view->idx, UI_ACCENT_CYAN, 0);

    view->type = lv_label_create(view->row);
    lv_obj_set_width(view->type, COL_TYPE_W);
    lv_obj_set_style_text_font(view->type, &lv_font_montserrat_10, 0);
    lv_label_set_long_mode(view->type, LV_LABEL_LONG_CLIP);

    view->freq = lv_label_create(view->row);
    lv_obj_set_width(view->freq, COL_FREQ_W);
    lv_obj_set_style_text_font(view->freq, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(view->freq, ui_muted_color(), 0);

    view->mf = lv_label_create(view->row);
    lv_obj_set_width(view->mf, COL_MF_W);
    lv_obj_set_style_text_font(view->mf, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(view->mf, ui_text_color(), 0);
    lv_label_set_long_mode(view->mf, LV_LABEL_LONG_CLIP);

    view->serial = lv_label_create(view->row);
    lv_obj_set_width(view->serial, COL_SER_W);
    lv_obj_set_style_text_font(view->serial, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(view->serial, ui_muted_color(), 0);
    lv_label_set_long_mode(view->serial, LV_LABEL_LONG_CLIP);
}

static void refresh_signal_list_view(void)
{
    subghz_signal_t window[SIGNAL_ROW_POOL_SIZE];
    size_t copied = 0;
    size_t total = 0;
    lv_coord_t scroll_y;
    size_t first_index;

    if (!s_sig_list || !s_sig_spacer || !s_empty_lbl)
        return;

    scroll_y = lv_obj_get_scroll_y(s_sig_list);
    if (scroll_y < 0)
        scroll_y = 0;

    {
        size_t total_for_idx = signal_count_snapshot();
        first_index = listen_scroll_to_first_index(scroll_y, total_for_idx);
    }
    copy_signal_window(first_index, window, SIGNAL_ROW_POOL_SIZE, &copied, &total);

    update_signal_count_label(total);

    if (total == 0) {
        lv_obj_clear_flag(s_empty_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(s_sig_spacer, 1);
        for (int i = 0; i < SIGNAL_ROW_POOL_SIZE; i++) {
            if (s_row_pool[i].row)
                lv_obj_add_flag(s_row_pool[i].row, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    lv_obj_add_flag(s_empty_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(s_sig_spacer, listen_virt_content_h(total));

    for (int i = 0; i < SIGNAL_ROW_POOL_SIZE; i++) {
        signal_row_view_t *view = &s_row_pool[i];

        if (!view->row)
            continue;

        if ((size_t)i >= copied) {
            lv_obj_add_flag(view->row, LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        size_t signal_index = first_index + (size_t)i;
        const subghz_signal_t *sig = &window[i];

        lv_obj_set_pos(view->row, 0, listen_row_y(signal_index, total));
        lv_label_set_text_fmt(view->idx, "%d", sig->idx);
        lv_label_set_text(view->type, sig->type);
        lv_obj_set_style_text_color(view->type,
                                    sig->is_raw ? UI_ACCENT_ORANGE : UI_ACCENT_GREEN, 0);
        lv_label_set_text_fmt(view->freq, "%d.%02d",
                              (int)sig->freq,
                              ((int)(sig->freq * 100.0f + 0.5f)) % 100);
        lv_label_set_text(view->mf, sig->mf[0] ? sig->mf : "--");
        lv_label_set_text(view->serial, sig->serial[0] ? sig->serial : "--");
        lv_obj_clear_flag(view->row, LV_OBJ_FLAG_HIDDEN);
    }
}

static void reset_capture_session(void)
{
    s_follow_latest = true;
    s_activity_pending = false;
    s_history_dirty = true;
    s_psram_exhausted = false;
    clear_signal_history();

    if (s_sig_list)
        lv_obj_scroll_to_y(s_sig_list, 0, LV_ANIM_OFF);

    if (s_canvas_buf) {
        waterfall_fill_bg();
        if (s_canvas)
            lv_obj_invalidate(s_canvas);
    }

    refresh_signal_list_view();
}

static void subghz_line_cb(const char *line)
{
    subghz_signal_info_t parsed;

    if (!s_running)
        return;

    if (subghz_parse_rssi_line(line, &(int){0}))
        return;

    if (!subghz_parse_signal_line(line, &parsed))
        return;

    if (parsed.kind == SUBGHZ_SIGNAL_KIND_LIST)
        return;

    if (parsed.kind == SUBGHZ_SIGNAL_KIND_RX ||
        parsed.kind == SUBGHZ_SIGNAL_KIND_RX_DUP ||
        parsed.kind == SUBGHZ_SIGNAL_KIND_RAW) {
        s_activity_pending = true;
        led_indicator_signal_received();
    }

    if (!s_raw_mode && parsed.kind == SUBGHZ_SIGNAL_KIND_RAW)
        return;

    if (s_raw_mode && parsed.kind == SUBGHZ_SIGNAL_KIND_RX_DUP)
        return;

    if (merge_duplicate_signal(&parsed))
        return;

    {
        subghz_signal_t sig;

        fill_signal(&sig, &parsed);
        append_signal_history(&sig);
    }
}

static void close_freq_popup(void)
{
    if (s_freq_popup) {
        lv_obj_delete(s_freq_popup);
        s_freq_popup = NULL;
    }
}

static void update_freq_label(void)
{
    if (s_freq_lbl) {
        int whole = (int)s_freq_mhz;
        int frac  = ((int)(s_freq_mhz * 100.0f + 0.5f)) % 100;
        lv_label_set_text_fmt(s_freq_lbl, "%d.%02d MHz", whole, frac);
    }
}

static void on_freq_set(lv_event_t *e)
{
    (void)e;
    int d[5];
    for (int i = 0; i < 5; i++)
        d[i] = (int)lv_roller_get_selected(s_rollers[i]);

    s_freq_mhz = d[0] * 100.0f + d[1] * 10.0f + d[2] * 1.0f
               + d[3] * 0.1f + d[4] * 0.01f;
    update_freq_label();
    close_freq_popup();
}

static void on_freq_cancel(lv_event_t *e)
{
    (void)e;
    close_freq_popup();
}

static void freq_decompose(float freq, int digits[5])
{
    int val = (int)(freq * 100.0f + 0.5f);
    digits[0] = (val / 10000) % 10;
    digits[1] = (val / 1000) % 10;
    digits[2] = (val / 100) % 10;
    digits[3] = (val / 10) % 10;
    digits[4] = val % 10;
}

static const char *s_digit_opts = "0\n1\n2\n3\n4\n5\n6\n7\n8\n9";

static void on_freq_tap(lv_event_t *e)
{
    (void)e;
    if (s_running) return;
    if (s_freq_popup) { close_freq_popup(); return; }

    int digits[5];
    freq_decompose(s_freq_mhz, digits);

    s_freq_popup = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_freq_popup, 260, 160);
    lv_obj_center(s_freq_popup);
    style_popup_card(s_freq_popup, 10, UI_ACCENT_PINK);
    lv_obj_set_flex_flow(s_freq_popup, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_freq_popup, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(s_freq_popup, 8, 0);
    lv_obj_set_style_pad_gap(s_freq_popup, 6, 0);
    lv_obj_clear_flag(s_freq_popup, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_freq_popup);
    lv_label_set_text(title, "Frequency (MHz)");
    lv_obj_set_style_text_color(title, ui_text_color(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);

    lv_obj_t *roller_row = lv_obj_create(s_freq_popup);
    lv_obj_set_size(roller_row, 240, 70);
    lv_obj_set_flex_flow(roller_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(roller_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(roller_row, 0, 0);
    lv_obj_set_style_pad_gap(roller_row, 2, 0);
    lv_obj_set_style_bg_opa(roller_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(roller_row, 0, 0);
    lv_obj_clear_flag(roller_row, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 5; i++) {
        if (i == 3) {
            lv_obj_t *dot = lv_label_create(roller_row);
            lv_label_set_text(dot, ".");
            lv_obj_set_style_text_font(dot, &lv_font_montserrat_20, 0);
            lv_obj_set_style_text_color(dot, ui_text_color(), 0);
        }
        s_rollers[i] = lv_roller_create(roller_row);
        lv_roller_set_options(s_rollers[i], s_digit_opts, LV_ROLLER_MODE_INFINITE);
        lv_roller_set_visible_row_count(s_rollers[i], 3);
        lv_obj_set_width(s_rollers[i], 36);
        lv_obj_set_style_bg_color(s_rollers[i], ui_card_color(), 0);
        lv_obj_set_style_bg_opa(s_rollers[i], LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(s_rollers[i], ui_text_color(), 0);
        lv_obj_set_style_text_font(s_rollers[i], &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(s_rollers[i], UI_ACCENT_CYAN, LV_PART_SELECTED);
        lv_obj_set_style_bg_color(s_rollers[i], ui_panel_color(), LV_PART_SELECTED);
        lv_obj_set_style_border_width(s_rollers[i], 0, 0);
        lv_obj_set_style_radius(s_rollers[i], 6, 0);
        lv_roller_set_selected(s_rollers[i], digits[i], LV_ANIM_OFF);
    }

    lv_obj_t *btn_row = lv_obj_create(s_freq_popup);
    lv_obj_set_size(btn_row, LV_PCT(100), 30);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *set_btn = lv_btn_create(btn_row);
    lv_obj_set_size(set_btn, 90, 26);
    lv_obj_set_style_bg_color(set_btn, UI_ACCENT_GREEN, 0);
    lv_obj_set_style_radius(set_btn, 6, 0);
    lv_obj_add_event_cb(set_btn, on_freq_set, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl2 = lv_label_create(set_btn);
    lv_label_set_text(sl2, "Set");
    lv_obj_set_style_text_color(sl2, lv_color_white(), 0);
    lv_obj_set_style_text_font(sl2, &lv_font_montserrat_12, 0);
    lv_obj_center(sl2);

    lv_obj_t *cancel_btn = lv_btn_create(btn_row);
    lv_obj_set_size(cancel_btn, 90, 26);
    lv_obj_set_style_bg_color(cancel_btn, ui_muted_color(), 0);
    lv_obj_set_style_radius(cancel_btn, 6, 0);
    lv_obj_add_event_cb(cancel_btn, on_freq_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(cancel_btn);
    lv_label_set_text(cl, "Cancel");
    lv_obj_set_style_text_color(cl, lv_color_white(), 0);
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_12, 0);
    lv_obj_center(cl);
}

static void update_start_stop_btn(void)
{
    if (!s_btn_start_stop) return;
    lv_obj_t *lbl = lv_obj_get_child(s_btn_start_stop, 0);
    if (s_running) {
        lv_obj_set_style_bg_color(s_btn_start_stop, UI_ACCENT_RED, 0);
        if (lbl) lv_label_set_text(lbl, LV_SYMBOL_STOP " Stop");
    } else {
        lv_obj_set_style_bg_color(s_btn_start_stop, UI_ACCENT_GREEN, 0);
        if (lbl) lv_label_set_text(lbl, LV_SYMBOL_PLAY " Start");
    }
}

static void stop_listening(void)
{
    if (!s_running) return;
    s_running = false;
    uart_send_command("subghz_stop");
    uart_set_line_callback(NULL);
    s_activity_pending = false;

    update_start_stop_btn();
    if (s_btn_raw) lv_obj_clear_state(s_btn_raw, LV_STATE_DISABLED);
    ESP_LOGI(TAG, "SubGHz listen stopped");
}

static void on_start_stop(lv_event_t *e)
{
    (void)e;
    if (s_running) {
        stop_listening();
        return;
    }

    uart_stop_collect();
    reset_capture_session();
    s_running = true;

    update_start_stop_btn();
    if (s_btn_raw) lv_obj_add_state(s_btn_raw, LV_STATE_DISABLED);

    char cmd[32];
    snprintf(cmd, sizeof(cmd), "subghz_freq %.2f", s_freq_mhz);
    uart_send_command(cmd);

    uart_set_line_callback(subghz_line_cb);

    if (s_raw_mode)
        uart_send_command("subghz_rx raw");
    else
        uart_send_command("subghz_rx");

    ESP_LOGI(TAG, "SubGHz listen started (%.2f MHz, raw=%d)", s_freq_mhz, s_raw_mode);
}

static void on_raw_toggle(lv_event_t *e)
{
    (void)e;
    s_raw_mode = !s_raw_mode;
    if (s_btn_raw) {
        lv_obj_t *lbl = lv_obj_get_child(s_btn_raw, 0);
        if (lbl) lv_label_set_text(lbl, s_raw_mode ? "RAW" : "Dec");
    }
}

static void on_back(lv_event_t *e)
{
    (void)e;
    stop_listening();
    uart_stop_collect();
    close_freq_popup();
    if (s_kb_timer) { lv_timer_delete(s_kb_timer); s_kb_timer = NULL; }
    if (s_ui_timer) { lv_timer_delete(s_ui_timer); s_ui_timer = NULL; }

    clear_signal_history();

    if (s_canvas_buf) {
        heap_caps_free(s_canvas_buf);
        s_canvas_buf = NULL;
    }
    s_canvas = NULL;

    show_subghz_screen();
}

static void kb_poll_cb(lv_timer_t *t)
{
    (void)t;
    uint8_t key = cardkb_read_key();
    if (key == 0) return;
    if (key == 0x1B || key == 0x08 || key == 0x7F) {
        on_back(NULL);
    }
}

static void ui_tick_cb(lv_timer_t *t)
{
    bool activity;
    bool history_dirty;
    size_t total;
    lv_coord_t target_y;

    (void)t;

    activity = s_activity_pending;
    s_activity_pending = false;
    history_dirty = s_history_dirty;
    s_history_dirty = false;

    bsp_display_lock(0);
    if (s_running)
        waterfall_push_activity(activity);
    if (history_dirty && s_follow_latest && s_sig_list) {
        total = signal_count_snapshot();
        target_y = listen_virt_content_h(total) - lv_obj_get_height(s_sig_list);
        if (target_y < 0)
            target_y = 0;
        lv_obj_scroll_to_y(s_sig_list, target_y, LV_ANIM_OFF);
    }
    if (history_dirty || s_psram_exhausted)
        refresh_signal_list_view();
    bsp_display_unlock();
}

static void on_signal_list_scroll(lv_event_t *e)
{
    size_t total;
    lv_coord_t max_scroll;
    lv_coord_t scroll_y;

    (void)e;

    total = signal_count_snapshot();
    scroll_y = lv_obj_get_scroll_y(s_sig_list);
    if (scroll_y < 0)
        scroll_y = 0;

    max_scroll = listen_virt_content_h(total) - lv_obj_get_height(s_sig_list);
    if (max_scroll < 0)
        max_scroll = 0;

    s_follow_latest = (max_scroll - scroll_y) <= SIGNAL_ROW_HEIGHT;
    refresh_signal_list_view();
}

void show_subghz_listen_screen(void)
{
    memset(s_row_pool, 0, sizeof(s_row_pool));
    s_signal_head  = NULL;
    s_signal_tail  = NULL;
    s_last_signal = NULL;
    s_signal_count = 0;
    s_running      = false;
    s_follow_latest = true;
    s_history_dirty = true;
    s_activity_pending = false;
    s_psram_exhausted = false;
    s_sig_list     = NULL;
    s_sig_spacer   = NULL;
    s_empty_lbl    = NULL;
    s_sig_count_lbl = NULL;
    s_freq_lbl     = NULL;
    s_btn_start_stop = NULL;
    s_btn_raw      = NULL;
    s_freq_popup   = NULL;
    s_kb_timer     = NULL;
    s_ui_timer     = NULL;
    s_canvas       = NULL;
    s_canvas_buf   = NULL;

    lv_obj_t *scr = ui_screen_clear();

    /* Top bar with freq label */
    lv_obj_t *bar = ui_create_top_bar(scr, "Listen", on_back, NULL);

    lv_obj_t *title_lbl = lv_obj_get_child(bar, 1);
    lv_obj_set_flex_grow(title_lbl, 0);

    lv_obj_t *spacer = lv_obj_create(bar);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_set_height(spacer, 1);

    lv_obj_t *freq_btn = lv_btn_create(bar);
    lv_obj_set_size(freq_btn, LV_SIZE_CONTENT, 28);
    lv_obj_set_style_bg_color(freq_btn, ui_card_color(), 0);
    lv_obj_set_style_bg_color(freq_btn, ui_card_pressed_color(), LV_STATE_PRESSED);
    lv_obj_set_style_radius(freq_btn, 6, 0);
    lv_obj_set_style_pad_hor(freq_btn, 8, 0);
    lv_obj_add_event_cb(freq_btn, on_freq_tap, LV_EVENT_CLICKED, NULL);

    s_freq_lbl = lv_label_create(freq_btn);
    {
        int whole = (int)s_freq_mhz;
        int frac  = ((int)(s_freq_mhz * 100.0f + 0.5f)) % 100;
        lv_label_set_text_fmt(s_freq_lbl, "%d.%02d MHz", whole, frac);
    }
    lv_obj_set_style_text_color(s_freq_lbl, UI_ACCENT_PINK, 0);
    lv_obj_set_style_text_font(s_freq_lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(s_freq_lbl);

    /* Control bar */
    lv_obj_t *ctrl = lv_obj_create(scr);
    lv_obj_set_size(ctrl, LV_PCT(100), 30);
    lv_obj_set_y(ctrl, 36);
    lv_obj_set_flex_flow(ctrl, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctrl, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(ctrl, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ctrl, 0, 0);
    lv_obj_set_style_pad_all(ctrl, 2, 0);

    s_btn_start_stop = lv_btn_create(ctrl);
    lv_obj_set_size(s_btn_start_stop, 60, 24);
    lv_obj_set_style_bg_color(s_btn_start_stop, UI_ACCENT_GREEN, 0);
    lv_obj_set_style_radius(s_btn_start_stop, 5, 0);
    lv_obj_add_event_cb(s_btn_start_stop, on_start_stop, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(s_btn_start_stop);
    lv_label_set_text(sl, LV_SYMBOL_PLAY " Start");
    lv_obj_set_style_text_font(sl, &lv_font_montserrat_10, 0);
    lv_obj_center(sl);

    s_btn_raw = lv_btn_create(ctrl);
    lv_obj_set_size(s_btn_raw, 40, 24);
    lv_obj_set_style_bg_color(s_btn_raw, ui_card_color(), 0);
    lv_obj_set_style_bg_color(s_btn_raw, ui_card_pressed_color(), LV_STATE_PRESSED);
    lv_obj_set_style_radius(s_btn_raw, 5, 0);
    lv_obj_add_event_cb(s_btn_raw, on_raw_toggle, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rl = lv_label_create(s_btn_raw);
    lv_label_set_text(rl, s_raw_mode ? "RAW" : "Dec");
    lv_obj_set_style_text_color(rl, ui_text_color(), 0);
    lv_obj_set_style_text_font(rl, &lv_font_montserrat_10, 0);
    lv_obj_center(rl);

    s_sig_count_lbl = lv_label_create(ctrl);
    lv_obj_set_style_text_font(s_sig_count_lbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_sig_count_lbl, UI_ACCENT_CYAN, 0);
    lv_label_set_text(s_sig_count_lbl, "Sig: 0");

    /* Waterfall canvas */
    size_t buf_sz = WATERFALL_W * WATERFALL_H * sizeof(lv_color_t);
    s_canvas_buf = heap_caps_malloc(buf_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_canvas_buf)
        s_canvas_buf = malloc(buf_sz);

    if (s_canvas_buf) {
        waterfall_fill_bg();

        s_canvas = lv_canvas_create(scr);
        lv_canvas_set_buffer(s_canvas, s_canvas_buf, WATERFALL_W, WATERFALL_H, LV_COLOR_FORMAT_RGB888);
        lv_obj_set_pos(s_canvas, 0, 66);
    }

    /* Signal table header */
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_set_size(hdr, LV_PCT(100), 14);
    lv_obj_set_y(hdr, 108);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(hdr, 1, 0);
    lv_obj_set_style_pad_gap(hdr, 2, 0);
    lv_obj_set_style_bg_color(hdr, ui_panel_color(), 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);

    static const struct { const char *t; int w; } cols[] = {
        {"#", COL_IDX_W}, {"Type", COL_TYPE_W}, {"Freq", COL_FREQ_W},
        {"Signal", COL_MF_W}, {"Serial", COL_SER_W},
    };
    for (int i = 0; i < 5; i++) {
        lv_obj_t *l = lv_label_create(hdr);
        lv_obj_set_width(l, cols[i].w);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(l, ui_muted_color(), 0);
        lv_label_set_text(l, cols[i].t);
    }

    /* Signal list */
    s_sig_list = lv_obj_create(scr);
    lv_obj_set_size(s_sig_list, LV_PCT(100), 240 - 122);
    lv_obj_set_y(s_sig_list, 122);
    lv_obj_set_style_pad_all(s_sig_list, 0, 0);
    lv_obj_set_style_bg_color(s_sig_list, ui_bg_color(), 0);
    lv_obj_set_style_bg_opa(s_sig_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_sig_list, 0, 0);
    lv_obj_set_scrollbar_mode(s_sig_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_event_cb(s_sig_list, on_signal_list_scroll, LV_EVENT_SCROLL, NULL);

    s_sig_spacer = lv_obj_create(s_sig_list);
    lv_obj_set_pos(s_sig_spacer, 0, 0);
    lv_obj_set_size(s_sig_spacer, 1, 1);
    lv_obj_set_style_bg_opa(s_sig_spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_sig_spacer, 0, 0);
    lv_obj_clear_flag(s_sig_spacer, LV_OBJ_FLAG_SCROLLABLE);

    s_empty_lbl = lv_label_create(s_sig_list);
    lv_obj_set_pos(s_empty_lbl, 8, 6);
    lv_obj_set_style_text_color(s_empty_lbl, ui_muted_color(), 0);
    lv_obj_set_style_text_font(s_empty_lbl, &lv_font_montserrat_12, 0);
    lv_label_set_text(s_empty_lbl, "No signals captured");

    for (int i = 0; i < SIGNAL_ROW_POOL_SIZE; i++)
        configure_signal_row(&s_row_pool[i]);

    refresh_signal_list_view();

    s_kb_timer = lv_timer_create(kb_poll_cb, 50, NULL);
    s_ui_timer = lv_timer_create(ui_tick_cb, WATERFALL_TICK_MS, NULL);

    ESP_LOGI(TAG, "SubGHz Listen screen ready");

    if (s_pending_autostart) {
        s_pending_autostart = false;
        on_start_stop(NULL);
    }
}

void show_subghz_listen_screen_at(float mhz, bool autostart)
{
    s_freq_mhz = mhz;
    s_pending_autostart = autostart;
    show_subghz_listen_screen();
}
