#ifndef UI_HELPERS_H
#define UI_HELPERS_H

#include "lvgl.h"

/* ---------- Material-ish dark palette ---------- */
#define UI_BG_COLOR         lv_color_hex(0x1A1A2E)
#define UI_BG_CARD          lv_color_hex(0x16213E)
#define UI_TEXT_COLOR        lv_color_hex(0xE0E0E0)
#define UI_TEXT_DIM          lv_color_hex(0x888888)
#define UI_ACCENT_BLUE       lv_color_hex(0x2196F3)
#define UI_ACCENT_RED        lv_color_hex(0xF44336)
#define UI_ACCENT_ORANGE     lv_color_hex(0xFF9800)
#define UI_ACCENT_PURPLE     lv_color_hex(0x9C27B0)
#define UI_ACCENT_CYAN       lv_color_hex(0x00BCD4)
#define UI_ACCENT_GREEN      lv_color_hex(0x4CAF50)
#define UI_ACCENT_PINK       lv_color_hex(0xE91E63)
#define UI_ACCENT_TEAL       lv_color_hex(0x009688)
#define UI_BAR_COLOR         lv_color_hex(0x0F3460)

/* ---------- Screen helpers ---------- */

/*
 * Clear active screen and set dark background.
 * Returns the active screen object.
 */
lv_obj_t *ui_screen_clear(void);

/*
 * Create a top bar with a title label and an optional back button.
 * If on_back is non-NULL a "< Back" button is added that calls it.
 * Returns the bar container.
 */
lv_obj_t *ui_create_top_bar(lv_obj_t *parent, const char *title,
                            lv_event_cb_t on_back, void *user_data);

/*
 * Create a single rounded tile button (used on home & attack screens).
 * Returns the tile object.
 */
lv_obj_t *ui_create_tile(lv_obj_t *parent, const char *label_text,
                          lv_color_t color, lv_event_cb_t on_click,
                          void *user_data);

#endif
