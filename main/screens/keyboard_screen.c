#include "keyboard_screen.h"
#include "home_screen.h"
#include "ui_helpers.h"
#include "cardkb.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include <string.h>

static const char *TAG = "kb_scr";

#define INPUT_MAX_LEN  128

/* ---- state ---- */
static char           input_buf[INPUT_MAX_LEN + 1];
static int            input_len = 0;
static lv_obj_t      *lbl_input  = NULL;
static lv_obj_t      *lbl_cursor = NULL;
static lv_timer_t    *poll_timer = NULL;
static keyboard_done_cb_t done_cb = NULL;

/* Forward */
static void poll_cardkb(lv_timer_t *t);
static void refresh_label(void);

/* ---- public ---- */

void show_keyboard_screen(keyboard_done_cb_t on_done)
{
    done_cb   = on_done;
    input_len = 0;
    input_buf[0] = '\0';

    lv_obj_t *scr = ui_screen_clear();

    /* Top bar */
    ui_create_top_bar(scr, "CardKB Input", NULL, NULL);

    /* Prompt label */
    lv_obj_t *prompt = lv_label_create(scr);
    lv_label_set_text(prompt, "Type on CardKB, press Enter to continue:");
    lv_obj_set_style_text_color(prompt, ui_muted_color(), 0);
    lv_obj_set_style_text_font(prompt, &lv_font_montserrat_14, 0);
    lv_obj_set_width(prompt, LV_PCT(90));
    lv_obj_set_style_text_align(prompt, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(prompt, LV_ALIGN_TOP_MID, 0, 50);

    /* Input display area – dark card */
    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_set_size(card, LV_PCT(90), 90);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_style_bg_color(card, ui_card_color(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_border_color(card, UI_ACCENT_CYAN, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* Text label inside card */
    lbl_input = lv_label_create(card);
    lv_label_set_long_mode(lbl_input, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_input, LV_PCT(100));
    lv_obj_set_style_text_color(lbl_input, UI_ACCENT_GREEN, 0);
    lv_obj_set_style_text_font(lbl_input, &lv_font_montserrat_16, 0);
    lv_label_set_text(lbl_input, "_");

    /* Blinking cursor hint at bottom */
    lbl_cursor = lv_label_create(scr);
    lv_label_set_text(lbl_cursor, "Waiting for input...");
    lv_obj_set_style_text_color(lbl_cursor, ui_muted_color(), 0);
    lv_obj_set_style_text_font(lbl_cursor, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl_cursor, LV_ALIGN_BOTTOM_MID, 0, -10);

    /* Start polling the CardKB every 50 ms */
    poll_timer = lv_timer_create(poll_cardkb, 50, NULL);

    ESP_LOGI(TAG, "Keyboard input screen shown");
}

/* ---- internal ---- */

static void refresh_label(void)
{
    /* Show current text + blinking underscore cursor */
    static char display[INPUT_MAX_LEN + 4];
    snprintf(display, sizeof(display), "%s_", input_buf);
    lv_label_set_text(lbl_input, display);
}

static void finish_input(void)
{
    /* Stop polling */
    if (poll_timer) {
        lv_timer_del(poll_timer);
        poll_timer = NULL;
    }

    ESP_LOGI(TAG, "Input confirmed: \"%s\"", input_buf);

    if (done_cb) {
        done_cb(input_buf);
    }

    /* Transition to home screen */
    show_home_screen();
}

static void poll_cardkb(lv_timer_t *t)
{
    (void)t;
    uint8_t key = cardkb_read_key();
    if (key == 0) return;

    ESP_LOGD(TAG, "key=0x%02X ('%c')", key, (key >= 0x20 && key < 0x7F) ? key : '.');

    if (key == 0x0D || key == 0x0A) {
        /* Enter */
        finish_input();
        return;
    }

    if (key == 0x08 || key == 0x7F) {
        /* Backspace / DEL */
        if (input_len > 0) {
            input_buf[--input_len] = '\0';
            refresh_label();
        }
        return;
    }

    /* Printable ASCII */
    if (key >= 0x20 && key < 0x7F && input_len < INPUT_MAX_LEN) {
        input_buf[input_len++] = (char)key;
        input_buf[input_len]   = '\0';
        refresh_label();
        lv_label_set_text(lbl_cursor, "Press Enter to confirm");
    }
}
