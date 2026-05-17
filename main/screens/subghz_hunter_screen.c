#include "subghz_hunter_screen.h"
#include "subghz_hunter_settings_screen.h"
#include "subghz_screen.h"
#include "subghz_parser.h"
#include "subghz_rf_settings.h"
#include "ui_helpers.h"
#include "uart_handler.h"
#include "cardkb.h"
#include "led_indicator.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

static const char *TAG = "subghz_hunter";

#define SIGNAL_CHUNK_CAPACITY 64
#define SIGNAL_ROW_HEIGHT     18
#define SIGNAL_ROW_POOL_SIZE  10
#define UI_TICK_MS            120
#define STATUS_BUF_LEN        48

/* Cap virtual scroll height so lv_coord / layout stay stable with huge captures. */
#define SIGNAL_VSCROLL_MAX_PX 28000

static int64_t hunter_real_content_h(size_t total)
{
    return (int64_t)total * (int64_t)SIGNAL_ROW_HEIGHT;
}

static lv_coord_t hunter_virt_content_h(size_t total)
{
    int64_t r = hunter_real_content_h(total);
    if (r > SIGNAL_VSCROLL_MAX_PX)
        return (lv_coord_t)SIGNAL_VSCROLL_MAX_PX;
    return (lv_coord_t)r;
}

static size_t hunter_scroll_to_first_index(lv_coord_t scroll_y, size_t total)
{
    if (total == 0)
        return 0;
    int64_t real = hunter_real_content_h(total);
    lv_coord_t virt = hunter_virt_content_h(total);
    if (real <= (int64_t)virt)
        return (size_t)scroll_y / SIGNAL_ROW_HEIGHT;
    int64_t eff = (int64_t)scroll_y * real / (int64_t)virt;
    size_t idx = (size_t)(eff / SIGNAL_ROW_HEIGHT);
    if (idx >= total)
        idx = total - 1;
    return idx;
}

static lv_coord_t hunter_row_y(size_t signal_index, size_t total)
{
    if (total == 0)
        return 0;
    lv_coord_t virt = hunter_virt_content_h(total);
    int64_t real = hunter_real_content_h(total);
    if (real <= (int64_t)virt)
        return (lv_coord_t)((int64_t)signal_index * SIGNAL_ROW_HEIGHT);
    if (total == 1)
        return 0;
    return (lv_coord_t)((int64_t)signal_index * ((int64_t)virt - SIGNAL_ROW_HEIGHT) / (int64_t)(total - 1));
}

#define COL_IDX_W             22
#define COL_TYPE_W            50
#define COL_FREQ_W            50
#define COL_MF_W              90
#define COL_SER_W             55

typedef struct {
    int   idx;
    char  type[32];
    float freq;
    char  serial[32];
    int   btn;
    int   cnt;
    char  mf[32];
    bool  is_raw;
} hunter_signal_t;

typedef struct hunter_signal_chunk {
    struct hunter_signal_chunk *next;
    size_t used;
    hunter_signal_t items[SIGNAL_CHUNK_CAPACITY];
} hunter_signal_chunk_t;

typedef struct {
    lv_obj_t *row;
    lv_obj_t *idx;
    lv_obj_t *type;
    lv_obj_t *freq;
    lv_obj_t *mf;
    lv_obj_t *serial;
} hunter_row_view_t;

typedef enum {
    HUNTER_STATUS_IDLE = 0,
    HUNTER_STATUS_SCAN,
    HUNTER_STATUS_CAPTURING,
    HUNTER_STATUS_TIMEOUT,
    HUNTER_STATUS_DUPLICATE,
    HUNTER_STATUS_ERROR,
    HUNTER_STATUS_STOPPED,
} hunter_status_kind_t;

static hunter_signal_chunk_t *s_signal_head;
static hunter_signal_chunk_t *s_signal_tail;
static hunter_signal_t       *s_last_signal;
static size_t                 s_signal_count;
static volatile bool          s_running;
static bool                   s_follow_latest;
static volatile bool          s_history_dirty;
static volatile bool          s_status_dirty;
static volatile bool          s_psram_exhausted;
static portMUX_TYPE           s_signal_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE           s_status_lock = portMUX_INITIALIZER_UNLOCKED;

static char                   s_status_text[STATUS_BUF_LEN];
static hunter_status_kind_t   s_status_kind;

static lv_obj_t   *s_spinner;
static lv_obj_t   *s_status_lbl;
static lv_obj_t   *s_capt_count_lbl;
static lv_obj_t   *s_btn_stop;
static lv_obj_t   *s_sig_list;
static lv_obj_t   *s_sig_spacer;
static lv_obj_t   *s_empty_lbl;
static hunter_row_view_t s_row_pool[SIGNAL_ROW_POOL_SIZE];
static lv_timer_t *s_kb_timer;
static lv_timer_t *s_ui_timer;

static void on_back(lv_event_t *e);
static void on_settings(lv_event_t *e);
static void on_stop(lv_event_t *e);
static void hunter_delete_timers(void);
static void hunter_reset_lvgl_pointers(void);
static void hunter_build_ui(lv_obj_t *scr);
static void hunter_start_uart(void);
static void hunter_line_cb(const char *line);
static void kb_poll_cb(lv_timer_t *t);
static void ui_tick_cb(lv_timer_t *t);
static void on_signal_list_scroll(lv_event_t *e);
static void refresh_signal_list_view(void);
static void clear_signal_history(void);
static void fill_signal(hunter_signal_t *dst, const subghz_signal_info_t *src);
static bool merge_duplicate_signal(const subghz_signal_info_t *src);
static void set_status(hunter_status_kind_t kind, const char *fmt, ...);
static void apply_status_to_label(void);

static size_t signal_count_snapshot(void)
{
    size_t count;

    portENTER_CRITICAL(&s_signal_lock);
    count = s_signal_count;
    portEXIT_CRITICAL(&s_signal_lock);

    return count;
}

static void set_status(hunter_status_kind_t kind, const char *fmt, ...)
{
    char buf[STATUS_BUF_LEN];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    portENTER_CRITICAL(&s_status_lock);
    s_status_kind = kind;
    strncpy(s_status_text, buf, sizeof(s_status_text) - 1);
    s_status_text[sizeof(s_status_text) - 1] = '\0';
    portEXIT_CRITICAL(&s_status_lock);

    s_status_dirty = true;
}

static lv_color_t status_color_for(hunter_status_kind_t kind)
{
    switch (kind) {
    case HUNTER_STATUS_CAPTURING: return UI_ACCENT_ORANGE;
    case HUNTER_STATUS_SCAN:      return UI_ACCENT_PINK;
    case HUNTER_STATUS_ERROR:     return UI_ACCENT_RED;
    case HUNTER_STATUS_DUPLICATE: return UI_ACCENT_CYAN;
    case HUNTER_STATUS_TIMEOUT:
    case HUNTER_STATUS_IDLE:
    case HUNTER_STATUS_STOPPED:
    default:                      return ui_muted_color();
    }
}

static void apply_status_to_label(void)
{
    hunter_status_kind_t kind;
    char snapshot[STATUS_BUF_LEN];

    portENTER_CRITICAL(&s_status_lock);
    kind = s_status_kind;
    memcpy(snapshot, s_status_text, sizeof(snapshot));
    portEXIT_CRITICAL(&s_status_lock);

    if (!s_status_lbl) return;

    lv_label_set_text(s_status_lbl, snapshot);
    lv_obj_set_style_text_color(s_status_lbl, status_color_for(kind), 0);
}

static void fill_signal(hunter_signal_t *dst, const subghz_signal_info_t *src)
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

static hunter_signal_chunk_t *alloc_signal_chunk(void)
{
    hunter_signal_chunk_t *chunk = heap_caps_malloc(sizeof(*chunk),
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!chunk)
        return NULL;

    memset(chunk, 0, sizeof(*chunk));
    return chunk;
}

static bool append_signal_history(const hunter_signal_t *sig)
{
    hunter_signal_chunk_t *new_chunk = NULL;
    hunter_signal_t *inserted;

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
    hunter_signal_chunk_t *head;

    portENTER_CRITICAL(&s_signal_lock);
    head = s_signal_head;
    s_signal_head = NULL;
    s_signal_tail = NULL;
    s_last_signal = NULL;
    s_signal_count = 0;
    portEXIT_CRITICAL(&s_signal_lock);

    while (head) {
        hunter_signal_chunk_t *next = head->next;
        free(head);
        head = next;
    }
}

static void copy_signal_window(size_t first_index, hunter_signal_t *out,
                               size_t max_items, size_t *out_count,
                               size_t *out_total)
{
    size_t copied = 0;
    size_t base = 0;
    hunter_signal_chunk_t *chunk;
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
    if (!s_capt_count_lbl)
        return;

    if (s_psram_exhausted) {
        lv_label_set_text_fmt(s_capt_count_lbl, "Captured: %lu MEM", (unsigned long)count);
        lv_obj_set_style_text_color(s_capt_count_lbl, UI_ACCENT_RED, 0);
    } else {
        lv_label_set_text_fmt(s_capt_count_lbl, "Captured: %lu", (unsigned long)count);
        lv_obj_set_style_text_color(s_capt_count_lbl, UI_ACCENT_CYAN, 0);
    }
}

static void configure_signal_row(hunter_row_view_t *view)
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
    hunter_signal_t window[SIGNAL_ROW_POOL_SIZE];
    size_t copied = 0;
    size_t total = 0;
    lv_coord_t scroll_y;
    size_t first_index;

    if (!s_sig_list || !s_sig_spacer || !s_empty_lbl)
        return;

    scroll_y = lv_obj_get_scroll_y(s_sig_list);
    if (scroll_y < 0)
        scroll_y = 0;

    first_index = hunter_scroll_to_first_index(scroll_y, total);
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
    lv_obj_set_height(s_sig_spacer, hunter_virt_content_h(total));

    for (int i = 0; i < SIGNAL_ROW_POOL_SIZE; i++) {
        hunter_row_view_t *view = &s_row_pool[i];

        if (!view->row)
            continue;

        if ((size_t)i >= copied) {
            lv_obj_add_flag(view->row, LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        size_t signal_index = first_index + (size_t)i;
        const hunter_signal_t *sig = &window[i];

        lv_obj_set_pos(view->row, 0, hunter_row_y(signal_index, total));
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

static void parse_fa_status_line(const char *line)
{
    if (strstr(line, "[SUBGHZ_FA] hunt capture")) {
        float f = 0.0f;
        const char *p = strstr(line, "freq=");
        set_status(HUNTER_STATUS_CAPTURING, "Capturing signals..");
        return;
    }
    if (strstr(line, "[SUBGHZ_FA] hunt timeout")) {
        set_status(HUNTER_STATUS_TIMEOUT, "Timeout (no burst)");
        return;
    }
    if (strstr(line, "[SUBGHZ_FA] hunt duplicate")) {
        set_status(HUNTER_STATUS_DUPLICATE, "Duplicate");
        return;
    }
    if (strstr(line, "[SUBGHZ_FA] hunt error")) {
        set_status(HUNTER_STATUS_ERROR, "Capture error");
        return;
    }
    if (strstr(line, "[SUBGHZ_FA] silent")) {
        set_status(HUNTER_STATUS_IDLE, "Idle (no signal)");
        return;
    }
    if (strstr(line, "[SUBGHZ_FA] freq=")) {
        float freq = 0.0f;
        int   rssi = 0;
        char  stage[8] = {0};
        const char *fields = strstr(line, "[SUBGHZ_FA]");
        if (fields &&
            sscanf(fields, "[SUBGHZ_FA] freq=%f rssi=%d stage=%7s",
                   &freq, &rssi, stage) >= 2) {
            set_status(HUNTER_STATUS_SCAN, "Scan %.2f @ %d dBm", freq, rssi);
        }
        return;
    }
    if (strstr(line, "[SUBGHZ_FA_START]")) {
        set_status(HUNTER_STATUS_SCAN, "Hunting...");
        return;
    }
}

static void hunter_line_cb(const char *line)
{
    subghz_signal_info_t parsed;

    if (!s_running)
        return;

    /* FA status / hunt events update the spinner status text */
    if (strstr(line, "[SUBGHZ_FA")) {
        parse_fa_status_line(line);
        /* fallthrough: not an RX/RAW line, no signal to store */
        return;
    }

    if (subghz_parse_rssi_line(line, &(int){0}))
        return;

    if (!subghz_parse_signal_line(line, &parsed))
        return;

    if (parsed.kind == SUBGHZ_SIGNAL_KIND_LIST)
        return;

    if (parsed.kind == SUBGHZ_SIGNAL_KIND_RX ||
        parsed.kind == SUBGHZ_SIGNAL_KIND_RX_DUP ||
        parsed.kind == SUBGHZ_SIGNAL_KIND_RAW) {
        led_indicator_signal_received();
    }

    if (merge_duplicate_signal(&parsed))
        return;

    {
        hunter_signal_t sig;

        fill_signal(&sig, &parsed);
        append_signal_history(&sig);
    }
}

static void stop_hunting(void)
{
    if (!s_running) return;
    s_running = false;
    uart_send_command("subghz_stop");
    uart_set_line_callback(NULL);

    if (s_spinner)
        lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);

    set_status(HUNTER_STATUS_STOPPED, "Stopped");

    if (s_btn_stop) {
        lv_obj_add_state(s_btn_stop, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(s_btn_stop, ui_muted_color(), 0);
    }

    ESP_LOGI(TAG, "Hunter stopped");
}

static void on_stop(lv_event_t *e)
{
    (void)e;
    stop_hunting();
}

static void hunter_delete_timers(void)
{
    if (s_kb_timer) { lv_timer_delete(s_kb_timer); s_kb_timer = NULL; }
    if (s_ui_timer) { lv_timer_delete(s_ui_timer); s_ui_timer = NULL; }
}

static void hunter_reset_lvgl_pointers(void)
{
    memset(s_row_pool, 0, sizeof(s_row_pool));
    s_spinner        = NULL;
    s_status_lbl     = NULL;
    s_capt_count_lbl = NULL;
    s_btn_stop       = NULL;
    s_sig_list       = NULL;
    s_sig_spacer     = NULL;
    s_empty_lbl      = NULL;
}

static void hunter_start_uart(void)
{
    subghz_rf_settings_t cfg;
    char cmd[96];

    subghz_rf_settings_load(&cfg);
    subghz_rf_build_hunter_cmd(&cfg, cmd, sizeof(cmd));
    if (cmd[0] == '\0') {
        strncpy(cmd, "subghz_freq_analyzer -70 hunt timeout=2000", sizeof(cmd) - 1);
        cmd[sizeof(cmd) - 1] = '\0';
    }

    uart_stop_collect();
    s_running = true;
    uart_set_line_callback(hunter_line_cb);
    uart_send_command(cmd);
    set_status(HUNTER_STATUS_SCAN, "Hunting...");
    ESP_LOGI(TAG, "Hunter UART: %s", cmd);
}

static void on_settings(lv_event_t *e)
{
    (void)e;
    stop_hunting();
    uart_stop_collect();
    hunter_delete_timers();
    show_subghz_hunter_settings_screen();
}

static void on_back(lv_event_t *e)
{
    (void)e;
    stop_hunting();
    uart_stop_collect();
    hunter_delete_timers();

    clear_signal_history();
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
    bool history_dirty;
    bool status_dirty;
    size_t total;
    lv_coord_t target_y;

    (void)t;

    history_dirty = s_history_dirty;
    s_history_dirty = false;
    status_dirty = s_status_dirty;
    s_status_dirty = false;

    if (!ui_display_lock_try())
        return;
    if (status_dirty)
        apply_status_to_label();
    if (history_dirty && s_follow_latest && s_sig_list) {
        total = signal_count_snapshot();
        lv_coord_t virt = hunter_virt_content_h(total);
        lv_coord_t list_h = lv_obj_get_height(s_sig_list);
        target_y = virt - list_h;
        if (target_y < 0)
            target_y = 0;
        lv_obj_scroll_to_y(s_sig_list, target_y, LV_ANIM_OFF);
    }
    if (history_dirty || s_psram_exhausted)
        refresh_signal_list_view();
    ui_display_unlock_safe();
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

    lv_coord_t virt = hunter_virt_content_h(total);
    max_scroll = virt - lv_obj_get_height(s_sig_list);
    if (max_scroll < 0)
        max_scroll = 0;

    s_follow_latest = (max_scroll - scroll_y) <= SIGNAL_ROW_HEIGHT;
    refresh_signal_list_view();
}

static void hunter_build_ui(lv_obj_t *scr)
{
    lv_obj_t *bar = ui_create_top_bar(scr, "Hunter", on_back, NULL);
    {
        lv_obj_t *title = lv_obj_get_child(bar, 1);
        if (title) {
            lv_obj_set_flex_grow(title, 0);
            lv_label_set_long_mode(title, LV_LABEL_LONG_CLIP);
        }
    }
    lv_obj_t *bar_spacer = lv_obj_create(bar);
    lv_obj_remove_style_all(bar_spacer);
    lv_obj_set_flex_grow(bar_spacer, 1);
    lv_obj_set_height(bar_spacer, 1);
    ui_top_bar_pass_through(bar_spacer);

    ui_add_top_bar_action(bar, LV_SYMBOL_SETTINGS, on_settings, NULL);

    /* Animation strip: spinner + status text */
    lv_obj_t *anim_row = lv_obj_create(scr);
    lv_obj_set_size(anim_row, LV_PCT(100), 40);
    lv_obj_set_y(anim_row, 36);
    lv_obj_set_flex_flow(anim_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(anim_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(anim_row, 8, 0);
    lv_obj_set_style_pad_ver(anim_row, 2, 0);
    lv_obj_set_style_pad_gap(anim_row, 8, 0);
    lv_obj_set_style_bg_color(anim_row, ui_panel_color(), 0);
    lv_obj_set_style_bg_opa(anim_row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(anim_row, 0, 0);
    lv_obj_clear_flag(anim_row, LV_OBJ_FLAG_SCROLLABLE);

    s_spinner = lv_spinner_create(anim_row);
    lv_obj_set_size(s_spinner, 28, 28);
    lv_spinner_set_anim_params(s_spinner, 900, 200);
    lv_obj_set_style_arc_color(s_spinner, ui_card_color(), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_spinner, UI_ACCENT_PINK, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_spinner, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_spinner, 4, LV_PART_INDICATOR);

    s_status_lbl = lv_label_create(anim_row);
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_status_lbl, UI_ACCENT_PINK, 0);
    lv_label_set_text(s_status_lbl, s_status_text);
    lv_obj_set_flex_grow(s_status_lbl, 1);

    /* Control row: Stop button + capture counter */
    lv_obj_t *ctrl = lv_obj_create(scr);
    lv_obj_set_size(ctrl, LV_PCT(100), 30);
    lv_obj_set_y(ctrl, 78);
    lv_obj_set_flex_flow(ctrl, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctrl, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(ctrl, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ctrl, 0, 0);
    lv_obj_set_style_pad_hor(ctrl, 8, 0);
    lv_obj_set_style_pad_ver(ctrl, 2, 0);
    lv_obj_clear_flag(ctrl, LV_OBJ_FLAG_SCROLLABLE);

    s_btn_stop = lv_btn_create(ctrl);
    lv_obj_set_size(s_btn_stop, 80, 24);
    lv_obj_set_style_bg_color(s_btn_stop, UI_ACCENT_RED, 0);
    lv_obj_set_style_radius(s_btn_stop, 5, 0);
    lv_obj_add_event_cb(s_btn_stop, on_stop, LV_EVENT_CLICKED, NULL);
    lv_obj_t *stop_lbl = lv_label_create(s_btn_stop);
    lv_label_set_text(stop_lbl, LV_SYMBOL_STOP " Stop");
    lv_obj_set_style_text_color(stop_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(stop_lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(stop_lbl);

    s_capt_count_lbl = lv_label_create(ctrl);
    lv_obj_set_style_text_font(s_capt_count_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_capt_count_lbl, UI_ACCENT_CYAN, 0);
    lv_label_set_text(s_capt_count_lbl, "Captured: 0");

    /* Signal table header */
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_set_size(hdr, LV_PCT(100), 14);
    lv_obj_set_y(hdr, 110);
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
    lv_obj_set_size(s_sig_list, LV_PCT(100), 240 - 124);
    lv_obj_set_y(s_sig_list, 124);
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
    lv_label_set_text(s_empty_lbl, "Waiting for captures...");

    for (int i = 0; i < SIGNAL_ROW_POOL_SIZE; i++)
        configure_signal_row(&s_row_pool[i]);

    refresh_signal_list_view();
    apply_status_to_label();

    s_kb_timer = lv_timer_create(kb_poll_cb, 50, NULL);
    s_ui_timer = lv_timer_create(ui_tick_cb, UI_TICK_MS, NULL);
}

void show_subghz_hunter_screen(void)
{
    clear_signal_history();
    s_running         = false;
    s_follow_latest   = true;
    s_history_dirty   = true;
    s_status_dirty    = true;
    s_psram_exhausted = false;
    hunter_delete_timers();
    hunter_reset_lvgl_pointers();

    s_status_kind = HUNTER_STATUS_SCAN;
    snprintf(s_status_text, sizeof(s_status_text), "Starting hunter...");

    lv_obj_t *scr = ui_screen_clear();
    hunter_build_ui(scr);
    hunter_start_uart();

    ESP_LOGI(TAG, "SubGHz Hunter screen ready");
}

void show_subghz_hunter_screen_resume(void)
{
    s_running         = false;
    s_history_dirty   = true;
    s_status_dirty    = true;
    hunter_delete_timers();
    hunter_reset_lvgl_pointers();

    s_status_kind = HUNTER_STATUS_SCAN;
    snprintf(s_status_text, sizeof(s_status_text), "Restarting hunter...");

    lv_obj_t *scr = ui_screen_clear();
    hunter_build_ui(scr);
    hunter_start_uart();

    ESP_LOGI(TAG, "SubGHz Hunter resumed (history preserved)");
}
