#include "subghz_manage_screen.h"
#include "subghz_screen.h"
#include "subghz_parser.h"
#include "ui_helpers.h"
#include "uart_handler.h"
#include "cardkb.h"
#include "led_indicator.h"
#include "psram_dynarr.h"
#include "esp_log.h"
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "subghz_manage";

#define SUBGHZ_SIG_HARD_CAP 2048

typedef struct {
    int   idx;
    char  type[32];
    float freq;
    char  serial[32];
    int   btn;
    int   cnt;
    char  mf[32];
    char  name[64];
} mgmt_signal_t;

static mgmt_signal_t *s_sigs;
static int            s_sig_cap;
static int            s_sig_count;
static lv_obj_t     *s_list;
static lv_obj_t     *s_status_lbl;
static lv_obj_t     *s_confirm_popup;
static lv_obj_t     *s_action_popup;
static lv_timer_t   *s_kb_timer;
static lv_timer_t   *s_build_timer;
static int           s_build_idx;
static int           s_action_target_idx;
static int           s_pending_delete_idx;

#define BUILD_ROWS_PER_TICK 6

static void on_back(lv_event_t *e);
static void build_list(void);
static void build_list_step(lv_timer_t *t);
static void build_one_row(int i);
static void kb_poll_cb(lv_timer_t *t);
static void fill_signal(mgmt_signal_t *dst, const subghz_signal_info_t *src);
static void rebuild_list_async(void *unused);
static void import_done_async(void *user_data);
static void stop_build_timer(void);
static void open_rename_popup(int idx);
static void on_rename_confirm(const char *text, void *user_data);
static void on_rename_cancel(void *user_data);
static const mgmt_signal_t *find_signal_by_idx(int idx);
static void on_list_received(const char **lines, int count);
static void on_row_tap(lv_event_t *e);
static void show_action_popup(int idx);
static void close_action_popup(void);
static void on_action_rename(lv_event_t *e);
static void on_action_save(lv_event_t *e);
static void on_action_delete(lv_event_t *e);
static void on_action_transmit(lv_event_t *e);
static void on_action_cancel(lv_event_t *e);
static void show_delete_confirm_popup(int idx);
static void on_delete_confirmed(lv_event_t *e);
static void on_delete_cancel(lv_event_t *e);
static void do_delete(int idx);
static void on_export_done(const char **lines, int count);
static void close_confirm_popup(void);
static bool s_text_input_open;
static int  s_pending_rename_idx;

static void fill_signal(mgmt_signal_t *dst, const subghz_signal_info_t *src)
{
    memset(dst, 0, sizeof(*dst));
    dst->idx = src->idx;
    dst->freq = src->freq;
    dst->btn = src->btn;
    dst->cnt = src->cnt;
    snprintf(dst->type, sizeof(dst->type), "%s", src->type[0] ? src->type : "--");
    snprintf(dst->serial, sizeof(dst->serial), "%s", src->serial[0] ? src->serial : "--");
    snprintf(dst->mf, sizeof(dst->mf), "%s", src->mf[0] ? src->mf : "--");
    snprintf(dst->name, sizeof(dst->name), "%s", src->name);
}

static const mgmt_signal_t *find_signal_by_idx(int idx)
{
    if (idx <= 0) return NULL;
    for (int j = 0; j < s_sig_count; j++) {
        if (s_sigs[j].idx == idx) return &s_sigs[j];
    }
    return NULL;
}

static bool is_valid_rename_char(char c)
{
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= 'a' && c <= 'z') return true;
    if (c >= '0' && c <= '9') return true;
    return c == '_' || c == '-' || c == '.';
}

static void on_rename_confirm(const char *text, void *user_data)
{
    s_text_input_open = false;
    int idx = (int)(intptr_t)user_data;
    if (idx <= 0 || !text) return;

    size_t len = strlen(text);
    if (len == 0) {
        if (s_status_lbl) {
            lv_label_set_text(s_status_lbl, "Rename: empty name");
            lv_obj_set_style_text_color(s_status_lbl, UI_ACCENT_RED, 0);
        }
        return;
    }
    if (len > 63) {
        if (s_status_lbl) {
            lv_label_set_text(s_status_lbl, "Rename: name too long");
            lv_obj_set_style_text_color(s_status_lbl, UI_ACCENT_RED, 0);
        }
        return;
    }
    for (size_t i = 0; i < len; i++) {
        if (!is_valid_rename_char(text[i])) {
            if (s_status_lbl) {
                lv_label_set_text(s_status_lbl, "Rename: invalid chars");
                lv_obj_set_style_text_color(s_status_lbl, UI_ACCENT_RED, 0);
            }
            return;
        }
    }

    s_pending_rename_idx = idx;

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "subghz_rename %d %s", idx, text);
    uart_send_command(cmd);

    if (s_status_lbl) {
        lv_label_set_text_fmt(s_status_lbl, "Renaming #%d...", idx);
        lv_obj_set_style_text_color(s_status_lbl, UI_ACCENT_BLUE, 0);
    }

    /* Re-issue subghz_list so the rebuilt list reflects the new name.
     * The collect buffer will also contain the [SUBGHZ_RENAME] /
     * [SUBGHZ_RENAME_ERR] response line, which on_list_received picks
     * apart to update s_status_lbl after the list rebuild. */
    uart_send_command("subghz_list");
    uart_start_collect("[SUBGHZ_LIST_END]", on_list_received);
}

static void on_rename_cancel(void *user_data)
{
    (void)user_data;
    s_text_input_open = false;
}

static void open_rename_popup(int idx)
{
    const mgmt_signal_t *sig = find_signal_by_idx(idx);
    if (!sig) return;

    s_text_input_open = true;
    ui_show_text_input_popup("Rename signal",
                             sig->name[0] ? sig->name : "",
                             63,
                             UI_ACCENT_BLUE,
                             on_rename_confirm,
                             on_rename_cancel,
                             (void *)(intptr_t)idx);
}

/* Re-running subghz_import re-adds every .sub file on the SD card, so the
 * firmware-side list grows with each import. Hide byte-for-byte duplicates
 * (same type/freq/serial/btn/mf/name) from the UI; storage on JanOS is left
 * intact until the user taps Clear All. The `name` is included so users
 * who renamed a copy still see both rows (the rename is the entire point
 * of having two .sub files with identical payloads). */
static bool is_duplicate_of_existing(const subghz_signal_info_t *p)
{
    int freq_x100 = (int)(p->freq * 100.0f + 0.5f);
    const char *p_type   = p->type[0]   ? p->type   : "--";
    const char *p_serial = p->serial[0] ? p->serial : "--";
    const char *p_mf     = p->mf[0]     ? p->mf     : "--";
    const char *p_name   = p->name;

    for (int j = 0; j < s_sig_count; j++) {
        const mgmt_signal_t *e = &s_sigs[j];
        int e_freq_x100 = (int)(e->freq * 100.0f + 0.5f);
        if (e_freq_x100 != freq_x100) continue;
        if (e->btn != p->btn) continue;
        if (strcmp(e->type, p_type) != 0) continue;
        if (strcmp(e->serial, p_serial) != 0) continue;
        if (strcmp(e->mf, p_mf) != 0) continue;
        if (strcmp(e->name, p_name) != 0) continue;
        return true;
    }
    return false;
}

static void rebuild_list_async(void *unused)
{
    (void)unused;
    if (s_status_lbl) {
        lv_label_set_text_fmt(s_status_lbl, "%d signals (unique)", s_sig_count);
        lv_obj_set_style_text_color(s_status_lbl, ui_muted_color(), 0);
    }
    build_list();
}

/* When a rename has been issued just before subghz_list, the collect buffer
 * holds the [SUBGHZ_RENAME] or [SUBGHZ_RENAME_ERR] response line too.
 * Pluck it out so we can give the user proper feedback after the list rebuild. */
static char s_rename_feedback[96];
static lv_color_t s_rename_feedback_color;
static bool s_rename_feedback_pending;

static void rename_feedback_async(void *user_data)
{
    (void)user_data;
    if (!s_rename_feedback_pending || !s_status_lbl) return;
    lv_label_set_text(s_status_lbl, s_rename_feedback);
    lv_obj_set_style_text_color(s_status_lbl, s_rename_feedback_color, 0);
    s_rename_feedback_pending = false;
}

static void extract_rename_feedback(const char **lines, int count)
{
    if (s_pending_rename_idx <= 0) return;

    for (int i = 0; i < count; i++) {
        const char *line = lines[i];
        if (!line) continue;

        if (strstr(line, "[SUBGHZ_RENAME] ")) {
            int idx = 0;
            char new_name[64] = {0};
            const char *p = strstr(line, "idx=");
            if (p) idx = atoi(p + 4);
            p = strstr(line, "new=");
            if (p) {
                p += 4;
                const char *end = strchr(p, ' ');
                size_t len = end ? (size_t)(end - p) : strlen(p);
                if (len >= sizeof(new_name)) len = sizeof(new_name) - 1;
                memcpy(new_name, p, len);
                new_name[len] = '\0';
            }
            if (idx > 0 && new_name[0]) {
                snprintf(s_rename_feedback, sizeof(s_rename_feedback),
                         "Renamed #%d -> %s", idx, new_name);
                s_rename_feedback_color = UI_ACCENT_GREEN;
                s_rename_feedback_pending = true;
            }
            s_pending_rename_idx = 0;
            return;
        }

        if (strstr(line, "[SUBGHZ_RENAME_ERR] ")) {
            int idx = 0;
            char reason[32] = {0};
            const char *p = strstr(line, "idx=");
            if (p) idx = atoi(p + 4);
            p = strstr(line, "reason=");
            if (p) {
                p += 7;
                const char *end = strchr(p, ' ');
                size_t len = end ? (size_t)(end - p) : strlen(p);
                if (len >= sizeof(reason)) len = sizeof(reason) - 1;
                memcpy(reason, p, len);
                reason[len] = '\0';
            }
            snprintf(s_rename_feedback, sizeof(s_rename_feedback),
                     "Rename #%d failed: %s", idx,
                     reason[0] ? reason : "error");
            s_rename_feedback_color = UI_ACCENT_RED;
            s_rename_feedback_pending = true;
            s_pending_rename_idx = 0;
            return;
        }
    }
}

static void on_list_received(const char **lines, int count)
{
    extract_rename_feedback(lines, count);

    s_sig_count = 0;
    for (int i = 0; i < count; i++) {
        subghz_signal_info_t parsed;

        if (!subghz_parse_signal_line(lines[i], &parsed) || parsed.kind != SUBGHZ_SIGNAL_KIND_LIST)
            continue;

        if (parsed.idx > 0 && is_duplicate_of_existing(&parsed))
            continue;

        if (!psram_dynarr_ensure((void **)&s_sigs, &s_sig_cap,
                                 s_sig_count + 1, sizeof(*s_sigs),
                                 SUBGHZ_SIG_HARD_CAP)) {
            ESP_LOGW(TAG, "signal cap reached at %d, dropping rest", s_sig_count);
            break;
        }

        mgmt_signal_t *s = &s_sigs[s_sig_count];
        fill_signal(s, &parsed);
        if (s->idx > 0) {
            s_sig_count++;
        }
    }

    /* Defer all LVGL work to the LVGL task to avoid hogging core 0 in uart_rx
     * and starving IDLE0 (would trip task_wdt for ~50 signals). */
    if (!ui_lvgl_async_call(rebuild_list_async, NULL))
        ESP_LOGW(TAG, "rebuild_list_async schedule failed");

    /* After the list rebuild runs (also async on LVGL task), push the
     * rename feedback. Schedule separately so the order is: rebuild_list
     * first, then feedback overwrites s_status_lbl. */
    if (s_rename_feedback_pending) {
        if (!ui_lvgl_async_call(rename_feedback_async, NULL))
            ESP_LOGW(TAG, "rename_feedback_async schedule failed");
    }
}

static void do_delete(int idx)
{
    if (idx <= 0) return;

    char cmd[32];
    snprintf(cmd, sizeof(cmd), "subghz_delete %d", idx);
    uart_send_command(cmd);

    if (s_status_lbl) {
        lv_label_set_text_fmt(s_status_lbl, "Deleted signal #%d", idx);
        lv_obj_set_style_text_color(s_status_lbl, UI_ACCENT_RED, 0);
    }

    ESP_LOGI(TAG, "Delete signal idx=%d", idx);

    /* Refresh list after short delay */
    uart_send_command("subghz_list");
    uart_start_collect("[SUBGHZ_LIST_END]", on_list_received);
}

/* ------------------------------------------------------------------ */
/*  Delete confirmation popup                                          */
/* ------------------------------------------------------------------ */

static void on_delete_confirmed(lv_event_t *e)
{
    (void)e;
    int idx = s_pending_delete_idx;
    s_pending_delete_idx = 0;
    close_confirm_popup();
    do_delete(idx);
}

static void on_delete_cancel(lv_event_t *e)
{
    (void)e;
    s_pending_delete_idx = 0;
    close_confirm_popup();
}

static void show_delete_confirm_popup(int idx)
{
    if (idx <= 0) return;
    if (s_confirm_popup) return;

    s_pending_delete_idx = idx;
    const mgmt_signal_t *sig = find_signal_by_idx(idx);

    s_confirm_popup = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_confirm_popup, 240, 130);
    lv_obj_center(s_confirm_popup);
    style_popup_card(s_confirm_popup, 10, UI_ACCENT_RED);
    lv_obj_set_flex_flow(s_confirm_popup, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_confirm_popup, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(s_confirm_popup, 10, 0);
    lv_obj_set_style_pad_gap(s_confirm_popup, 6, 0);
    lv_obj_clear_flag(s_confirm_popup, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_confirm_popup);
    lv_label_set_text_fmt(title, "Delete signal #%d?", idx);
    lv_obj_set_style_text_color(title, ui_text_color(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);

    if (sig && sig->name[0]) {
        lv_obj_t *sub = lv_label_create(s_confirm_popup);
        lv_obj_set_width(sub, 220);
        lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
        lv_label_set_text(sub, sig->name);
        lv_obj_set_style_text_color(sub, ui_muted_color(), 0);
        lv_obj_set_style_text_font(sub, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    }

    lv_obj_t *brow = lv_obj_create(s_confirm_popup);
    lv_obj_set_size(brow, LV_PCT(100), 32);
    lv_obj_set_flex_flow(brow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(brow, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(brow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(brow, 0, 0);
    lv_obj_set_style_pad_all(brow, 0, 0);
    lv_obj_clear_flag(brow, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *yes = lv_btn_create(brow);
    lv_obj_set_size(yes, 90, 28);
    lv_obj_set_style_bg_color(yes, UI_ACCENT_RED, 0);
    lv_obj_set_style_radius(yes, 6, 0);
    lv_obj_add_event_cb(yes, on_delete_confirmed, LV_EVENT_CLICKED, NULL);
    lv_obj_t *yl = lv_label_create(yes);
    lv_label_set_text(yl, "Delete");
    lv_obj_set_style_text_color(yl, lv_color_white(), 0);
    lv_obj_set_style_text_font(yl, &lv_font_montserrat_12, 0);
    lv_obj_center(yl);

    lv_obj_t *no = lv_btn_create(brow);
    lv_obj_set_size(no, 90, 28);
    lv_obj_set_style_bg_color(no, ui_card_color(), 0);
    lv_obj_set_style_radius(no, 6, 0);
    lv_obj_add_event_cb(no, on_delete_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t *nl = lv_label_create(no);
    lv_label_set_text(nl, "Cancel");
    lv_obj_set_style_text_color(nl, ui_text_color(), 0);
    lv_obj_set_style_text_font(nl, &lv_font_montserrat_12, 0);
    lv_obj_center(nl);
}

/* ------------------------------------------------------------------ */
/*  Save (single-signal export) feedback                               */
/* ------------------------------------------------------------------ */

static char       s_export_feedback[96];
static lv_color_t s_export_feedback_color;
static bool       s_export_feedback_pending;

static void export_feedback_async(void *user_data)
{
    (void)user_data;
    if (!s_export_feedback_pending || !s_status_lbl) return;
    lv_label_set_text(s_status_lbl, s_export_feedback);
    lv_obj_set_style_text_color(s_status_lbl, s_export_feedback_color, 0);
    s_export_feedback_pending = false;
}

static void on_export_done(const char **lines, int count)
{
    int idx = 0;
    char name[64] = {0};

    for (int i = 0; i < count; i++) {
        const char *line = lines[i];
        if (!line) continue;
        if (!strstr(line, "[SUBGHZ_EXPORT] ")) continue;

        const char *p = strstr(line, "idx=");
        if (p) idx = atoi(p + 4);
        p = strstr(line, "name=");
        if (p) {
            p += 5;
            const char *end = strchr(p, ' ');
            size_t len = end ? (size_t)(end - p) : strlen(p);
            if (len >= sizeof(name)) len = sizeof(name) - 1;
            memcpy(name, p, len);
            name[len] = '\0';
        }
        break;
    }

    if (idx > 0 && name[0])
        snprintf(s_export_feedback, sizeof(s_export_feedback),
                 "Saved #%d as %s", idx, name);
    else if (idx > 0)
        snprintf(s_export_feedback, sizeof(s_export_feedback),
                 "Saved #%d to favorites", idx);
    else
        snprintf(s_export_feedback, sizeof(s_export_feedback),
                 "Export complete");
    s_export_feedback_color = UI_ACCENT_GREEN;
    s_export_feedback_pending = true;

    if (!ui_lvgl_async_call(export_feedback_async, NULL))
        ESP_LOGW(TAG, "export_feedback_async schedule failed");
}

/* ------------------------------------------------------------------ */
/*  Per-row action popup                                               */
/* ------------------------------------------------------------------ */

static void close_action_popup(void)
{
    if (s_action_popup) {
        lv_obj_delete(s_action_popup);
        s_action_popup = NULL;
    }
}

static void on_action_rename(lv_event_t *e)
{
    (void)e;
    int idx = s_action_target_idx;
    close_action_popup();
    if (idx > 0) open_rename_popup(idx);
}

static void on_action_save(lv_event_t *e)
{
    (void)e;
    int idx = s_action_target_idx;
    close_action_popup();
    if (idx <= 0) return;

    char cmd[32];
    snprintf(cmd, sizeof(cmd), "subghz_export %d", idx);
    uart_send_command(cmd);
    /* Collect until SUBGHZ_EXPORT_END so we can show the saved file name. */
    uart_start_collect("[SUBGHZ_EXPORT_END]", on_export_done);

    if (s_status_lbl) {
        lv_label_set_text_fmt(s_status_lbl, "Saving #%d to favorites...", idx);
        lv_obj_set_style_text_color(s_status_lbl, UI_ACCENT_BLUE, 0);
    }
}

static void on_action_delete(lv_event_t *e)
{
    (void)e;
    int idx = s_action_target_idx;
    close_action_popup();
    if (idx > 0) show_delete_confirm_popup(idx);
}

static void on_action_transmit(lv_event_t *e)
{
    (void)e;
    int idx = s_action_target_idx;
    close_action_popup();
    if (idx <= 0) return;

    char cmd[32];
    snprintf(cmd, sizeof(cmd), "subghz_tx %d", idx);
    uart_send_command(cmd);
    led_indicator_tx_pulse(1500);

    if (s_status_lbl) {
        lv_label_set_text_fmt(s_status_lbl, "Transmitted #%d", idx);
        lv_obj_set_style_text_color(s_status_lbl, UI_ACCENT_GREEN, 0);
    }
    ESP_LOGI(TAG, "Transmit idx=%d", idx);
}

static void on_action_cancel(lv_event_t *e)
{
    (void)e;
    close_action_popup();
}

static void show_action_popup(int idx)
{
    if (idx <= 0) return;
    close_action_popup();

    const mgmt_signal_t *sig = find_signal_by_idx(idx);
    s_action_target_idx = idx;

    s_action_popup = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_action_popup, 310, 190);
    lv_obj_center(s_action_popup);
    style_popup_card(s_action_popup, 10, UI_ACCENT_BLUE);
    lv_obj_set_flex_flow(s_action_popup, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_action_popup, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(s_action_popup, 10, 0);
    lv_obj_set_style_pad_gap(s_action_popup, 8, 0);
    lv_obj_clear_flag(s_action_popup, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_action_popup);
    if (sig)
        lv_label_set_text_fmt(title, "Signal #%d (%s)", sig->idx,
                              sig->type[0] ? sig->type : "--");
    else
        lv_label_set_text_fmt(title, "Signal #%d", idx);
    lv_obj_set_style_text_color(title, ui_text_color(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);

    lv_obj_t *name_lbl = lv_label_create(s_action_popup);
    lv_obj_set_width(name_lbl, 290);
    lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
    lv_label_set_text_fmt(name_lbl, "name: %s",
                          (sig && sig->name[0]) ? sig->name : "(unset)");
    lv_obj_set_style_text_color(name_lbl, ui_muted_color(), 0);
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(name_lbl, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *brow = lv_obj_create(s_action_popup);
    lv_obj_set_size(brow, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(brow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(brow, 0, 0);
    lv_obj_set_style_pad_all(brow, 0, 0);
    lv_obj_set_style_pad_gap(brow, 4, 0);
    lv_obj_set_flex_flow(brow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(brow, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(brow, LV_OBJ_FLAG_SCROLLABLE);

    struct {
        const char    *label;
        lv_color_t     bg;
        lv_event_cb_t  cb;
    } btns[] = {
        { "Rename",   UI_ACCENT_BLUE,   on_action_rename   },
        { "Save",     UI_ACCENT_GREEN,  on_action_save     },
        { "Delete",   UI_ACCENT_RED,    on_action_delete   },
        { "Transmit", UI_ACCENT_ORANGE, on_action_transmit },
        { "Cancel",   ui_muted_color(), on_action_cancel   },
    };
    for (int i = 0; i < (int)(sizeof(btns) / sizeof(btns[0])); i++) {
        lv_obj_t *b = lv_btn_create(brow);
        lv_obj_set_size(b, 54, 30);
        lv_obj_set_style_bg_color(b, btns[i].bg, 0);
        lv_obj_set_style_radius(b, 6, 0);
        lv_obj_add_event_cb(b, btns[i].cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, btns[i].label);
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
        lv_obj_center(l);
    }
}

static void on_row_tap(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx <= 0) return;
    show_action_popup(idx);
}

static void stop_build_timer(void)
{
    if (s_build_timer) {
        lv_timer_delete(s_build_timer);
        s_build_timer = NULL;
    }
}

/* Build a single clickable row showing the signal as a single label.
 * Tapping the row opens the action popup (Rename / Save / Delete / Cancel).
 * No per-row icons — keeps rows narrow and the action affordance unified
 * with the Listen screen UX. */
static void build_one_row(int i)
{
    mgmt_signal_t *sig = &s_sigs[i];

    lv_obj_t *row = lv_obj_create(s_list);
    lv_obj_set_size(row, LV_PCT(100), 28);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(row, 3, 0);
    lv_obj_set_style_pad_gap(row, 6, 0);
    lv_obj_set_style_bg_color(row, ui_card_color(), 0);
    lv_obj_set_style_bg_color(row, ui_card_pressed_color(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 5, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, on_row_tap, LV_EVENT_CLICKED,
                        (void *)(intptr_t)sig->idx);

    lv_obj_t *info = lv_label_create(row);
    lv_obj_set_flex_grow(info, 1);
    lv_obj_set_style_text_font(info, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(info, ui_text_color(), 0);
    lv_label_set_long_mode(info, LV_LABEL_LONG_CLIP);

    /* Prefer the editable `name` (from `[SUBGHZ_LIST] ... name=`); fall
     * back to mf/serial for older firmware that doesn't emit name. */
    const char *tail = sig->name[0] ? sig->name
                                    : (sig->mf[0] ? sig->mf : sig->serial);
    lv_label_set_text_fmt(info, "%d  %s  %d.%02d  %s",
                          sig->idx, sig->type,
                          (int)sig->freq,
                          ((int)(sig->freq * 100.0f + 0.5f)) % 100,
                          tail);
}

static void build_list_step(lv_timer_t *t)
{
    (void)t;
    if (!s_list) {
        stop_build_timer();
        return;
    }
    int target = s_build_idx + BUILD_ROWS_PER_TICK;
    if (target > s_sig_count) target = s_sig_count;
    for (; s_build_idx < target; s_build_idx++) {
        build_one_row(s_build_idx);
    }
    if (s_build_idx >= s_sig_count) {
        stop_build_timer();
    }
}

static void build_list(void)
{
    stop_build_timer();
    if (!s_list) return;
    lv_obj_clean(s_list);
    s_build_idx = 0;

    if (s_sig_count == 0) {
        lv_obj_t *l = lv_label_create(s_list);
        lv_label_set_text(l, "No stored signals");
        lv_obj_set_style_text_color(l, ui_muted_color(), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
        return;
    }

    /* Schedule chunked builds. Period 30 ms gives LVGL room to render
     * and feed IDLE between batches; 6 rows per batch keeps each tick short. */
    s_build_timer = lv_timer_create(build_list_step, 30, NULL);
}

static void close_confirm_popup(void)
{
    if (s_confirm_popup) {
        lv_obj_delete(s_confirm_popup);
        s_confirm_popup = NULL;
    }
}

static void on_clear_confirmed(lv_event_t *e)
{
    (void)e;
    close_confirm_popup();
    uart_send_command("subghz_clear");

    if (s_status_lbl) {
        lv_label_set_text(s_status_lbl, "All signals cleared");
        lv_obj_set_style_text_color(s_status_lbl, UI_ACCENT_RED, 0);
    }

    s_sig_count = 0;
    build_list();
    ESP_LOGI(TAG, "Clear all signals");
}

static void on_clear_cancel(lv_event_t *e)
{
    (void)e;
    close_confirm_popup();
}

static void on_clear_all(lv_event_t *e)
{
    (void)e;
    if (s_confirm_popup) return;

    s_confirm_popup = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_confirm_popup, 220, 100);
    lv_obj_center(s_confirm_popup);
    style_popup_card(s_confirm_popup, 10, UI_ACCENT_RED);
    lv_obj_set_flex_flow(s_confirm_popup, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_confirm_popup, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(s_confirm_popup, 10, 0);
    lv_obj_set_style_pad_gap(s_confirm_popup, 8, 0);
    lv_obj_clear_flag(s_confirm_popup, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *l = lv_label_create(s_confirm_popup);
    lv_label_set_text(l, "Delete ALL signals?");
    lv_obj_set_style_text_color(l, ui_text_color(), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);

    lv_obj_t *brow = lv_obj_create(s_confirm_popup);
    lv_obj_set_size(brow, LV_PCT(100), 32);
    lv_obj_set_flex_flow(brow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(brow, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(brow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(brow, 0, 0);

    lv_obj_t *yes = lv_btn_create(brow);
    lv_obj_set_size(yes, 80, 28);
    lv_obj_set_style_bg_color(yes, UI_ACCENT_RED, 0);
    lv_obj_set_style_radius(yes, 6, 0);
    lv_obj_add_event_cb(yes, on_clear_confirmed, LV_EVENT_CLICKED, NULL);
    l = lv_label_create(yes);
    lv_label_set_text(l, "Yes");
    lv_obj_center(l);

    lv_obj_t *no = lv_btn_create(brow);
    lv_obj_set_size(no, 80, 28);
    lv_obj_set_style_bg_color(no, ui_card_color(), 0);
    lv_obj_set_style_radius(no, 6, 0);
    lv_obj_add_event_cb(no, on_clear_cancel, LV_EVENT_CLICKED, NULL);
    l = lv_label_create(no);
    lv_label_set_text(l, "Cancel");
    lv_obj_set_style_text_color(l, ui_text_color(), 0);
    lv_obj_center(l);
}

static void on_export_all(lv_event_t *e)
{
    (void)e;
    uart_send_command("subghz_export all");
    if (s_status_lbl) {
        lv_label_set_text(s_status_lbl, "Export sent");
        lv_obj_set_style_text_color(s_status_lbl, UI_ACCENT_GREEN, 0);
    }
    ESP_LOGI(TAG, "Export all");
}

static int s_imported_count;

static void import_done_async(void *user_data)
{
    (void)user_data;
    if (s_status_lbl) {
        lv_label_set_text_fmt(s_status_lbl, "Imported %d signal%s",
                              s_imported_count, s_imported_count == 1 ? "" : "s");
        lv_obj_set_style_text_color(s_status_lbl, UI_ACCENT_GREEN, 0);
    }
}

static void on_import_done(const char **lines, int count)
{
    int imported = 0;
    for (int i = 0; i < count; i++) {
        if (strstr(lines[i], "[SUBGHZ_IMPORT] "))
            imported++;
    }
    s_imported_count = imported;

    /* Update label + chain follow-up subghz_list on the LVGL task. */
    if (!ui_lvgl_async_call(import_done_async, NULL))
        ESP_LOGW(TAG, "import_done_async schedule failed");

    uart_send_command("subghz_list");
    uart_start_collect("[SUBGHZ_LIST_END]", on_list_received);
}

static void on_import(lv_event_t *e)
{
    (void)e;
    uart_send_command("subghz_import all");
    uart_start_collect("[SUBGHZ_IMPORT_END]", on_import_done);
    if (s_status_lbl) {
        lv_label_set_text(s_status_lbl, "Importing from SD...");
        lv_obj_set_style_text_color(s_status_lbl, UI_ACCENT_BLUE, 0);
    }
    ESP_LOGI(TAG, "Import all from SD");
}

static void on_back(lv_event_t *e)
{
    (void)e;
    uart_stop_collect();
    close_confirm_popup();
    close_action_popup();
    stop_build_timer();
    if (s_kb_timer) { lv_timer_delete(s_kb_timer); s_kb_timer = NULL; }
    s_pending_delete_idx = 0;
    s_action_target_idx = 0;
    s_list = NULL;
    s_status_lbl = NULL;
    show_subghz_screen();
}

static void kb_poll_cb(lv_timer_t *t)
{
    (void)t;
    /* While a popup owns the keyboard let it consume ESC etc. */
    if (s_text_input_open || s_confirm_popup || s_action_popup) return;
    uint8_t key = cardkb_read_key();
    if (key == 0) return;
    if (key == 0x1B || key == 0x08 || key == 0x7F)
        on_back(NULL);
}

void show_subghz_manage_screen(void)
{
    s_sig_count     = 0;
    s_list          = NULL;
    s_status_lbl    = NULL;
    s_confirm_popup = NULL;
    s_action_popup  = NULL;
    s_kb_timer      = NULL;
    s_build_timer   = NULL;
    s_build_idx     = 0;
    s_text_input_open = false;
    s_pending_rename_idx = 0;
    s_pending_delete_idx = 0;
    s_action_target_idx = 0;
    s_rename_feedback_pending = false;
    s_export_feedback_pending = false;

    lv_obj_t *scr = ui_screen_clear();
    ui_create_top_bar(scr, "Manage Signals", on_back, NULL);

    /* Action bar */
    lv_obj_t *abar = lv_obj_create(scr);
    lv_obj_set_size(abar, LV_PCT(100), 28);
    lv_obj_set_y(abar, 36);
    lv_obj_set_flex_flow(abar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(abar, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(abar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(abar, 0, 0);
    lv_obj_set_style_pad_all(abar, 2, 0);

    lv_obj_t *btn, *lbl;

    btn = lv_btn_create(abar);
    lv_obj_set_size(btn, 70, 22);
    lv_obj_set_style_bg_color(btn, UI_ACCENT_GREEN, 0);
    lv_obj_set_style_radius(btn, 5, 0);
    lv_obj_add_event_cb(btn, on_export_all, LV_EVENT_CLICKED, NULL);
    lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "Export");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
    lv_obj_center(lbl);

    btn = lv_btn_create(abar);
    lv_obj_set_size(btn, 70, 22);
    lv_obj_set_style_bg_color(btn, UI_ACCENT_BLUE, 0);
    lv_obj_set_style_radius(btn, 5, 0);
    lv_obj_add_event_cb(btn, on_import, LV_EVENT_CLICKED, NULL);
    lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "Import");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
    lv_obj_center(lbl);

    btn = lv_btn_create(abar);
    lv_obj_set_size(btn, 70, 22);
    lv_obj_set_style_bg_color(btn, UI_ACCENT_RED, 0);
    lv_obj_set_style_radius(btn, 5, 0);
    lv_obj_add_event_cb(btn, on_clear_all, LV_EVENT_CLICKED, NULL);
    lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "Clear All");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
    lv_obj_center(lbl);

    s_status_lbl = lv_label_create(scr);
    lv_obj_set_y(s_status_lbl, 66);
    lv_obj_set_x(s_status_lbl, 8);
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_status_lbl, ui_muted_color(), 0);
    lv_label_set_text(s_status_lbl, "Loading...");

    s_list = lv_obj_create(scr);
    lv_obj_set_size(s_list, LV_PCT(100), 240 - 82);
    lv_obj_set_y(s_list, 82);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_list, 4, 0);
    lv_obj_set_style_pad_gap(s_list, 3, 0);
    lv_obj_set_style_bg_color(s_list, ui_bg_color(), 0);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_AUTO);

    uart_send_command("subghz_list");
    uart_start_collect("[SUBGHZ_LIST_END]", on_list_received);

    s_kb_timer = lv_timer_create(kb_poll_cb, 50, NULL);

    ESP_LOGI(TAG, "SubGHz Manage screen ready");
}
