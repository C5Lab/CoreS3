#ifndef UI_HELPERS_H
#define UI_HELPERS_H

#include "lvgl.h"
#include <stdbool.h>

/* ========== Dark palette ========== */
#define COLOR_DARK_BG            lv_color_hex(0x050A14)
#define COLOR_DARK_PANEL         lv_color_hex(0x091423)
#define COLOR_DARK_CARD          lv_color_hex(0x0C1A2A)
#define COLOR_DARK_CARD_PRESSED  lv_color_hex(0x132338)
#define COLOR_DARK_BORDER        lv_color_hex(0x273D57)
#define COLOR_DARK_TEXT          lv_color_hex(0xEAF3FF)
#define COLOR_DARK_MUTED         lv_color_hex(0x93A6BC)
#define COLOR_DARK_ACCENT        lv_color_hex(0x52B6FF)

/* ========== Light palette ========== */
#define COLOR_LIGHT_BG            lv_color_hex(0xE8EEF5)
#define COLOR_LIGHT_PANEL         lv_color_hex(0xDCE6F1)
#define COLOR_LIGHT_CARD          lv_color_hex(0xF8FBFF)
#define COLOR_LIGHT_CARD_PRESSED  lv_color_hex(0xEAF1F8)
#define COLOR_LIGHT_BORDER        lv_color_hex(0xA6B6C8)
#define COLOR_LIGHT_TEXT          lv_color_hex(0x1E2A36)
#define COLOR_LIGHT_MUTED         lv_color_hex(0x5C6D80)
#define COLOR_LIGHT_ACCENT        lv_color_hex(0x365E87)

/* ========== Semantic accent colors (theme-independent) ========== */
#define UI_ACCENT_BLUE       lv_color_hex(0x2196F3)
#define UI_ACCENT_RED        lv_color_hex(0xF44336)
#define UI_ACCENT_ORANGE     lv_color_hex(0xFF9800)
#define UI_ACCENT_PURPLE     lv_color_hex(0x9C27B0)
#define UI_ACCENT_CYAN       lv_color_hex(0x00BCD4)
#define UI_ACCENT_GREEN      lv_color_hex(0x4CAF50)
#define UI_ACCENT_PINK       lv_color_hex(0xE91E63)
#define UI_ACCENT_TEAL       lv_color_hex(0x009688)

/* ========== Magenta accent (splash / borders) ========== */
#define COLOR_MAGENTA        lv_color_hex(0xFF2FA3)

/* ========== Theme toggle ========== */
extern bool dark_mode_enabled;

/* ========== Boot sound ========== */
typedef enum {
    BOOT_SOUND_OFF      = 0,
    BOOT_SOUND_NOKIA    = 1,
    BOOT_SOUND_INTEL    = 2,
    BOOT_SOUND_STARWARS = 3,
} boot_sound_mode_t;

extern boot_sound_mode_t boot_sound_mode;

/* ========== Color accessor functions ========== */
static inline lv_color_t ui_bg_color(void) {
    return dark_mode_enabled ? COLOR_DARK_BG : COLOR_LIGHT_BG;
}
static inline lv_color_t ui_panel_color(void) {
    return dark_mode_enabled ? COLOR_DARK_PANEL : COLOR_LIGHT_PANEL;
}
static inline lv_color_t ui_card_color(void) {
    return dark_mode_enabled ? COLOR_DARK_CARD : COLOR_LIGHT_CARD;
}
static inline lv_color_t ui_card_pressed_color(void) {
    return dark_mode_enabled ? COLOR_DARK_CARD_PRESSED : COLOR_LIGHT_CARD_PRESSED;
}
static inline lv_color_t ui_border_color(void) {
    return dark_mode_enabled ? COLOR_DARK_BORDER : COLOR_LIGHT_BORDER;
}
static inline lv_color_t ui_text_color(void) {
    return dark_mode_enabled ? COLOR_DARK_TEXT : COLOR_LIGHT_TEXT;
}
static inline lv_color_t ui_muted_color(void) {
    return dark_mode_enabled ? COLOR_DARK_MUTED : COLOR_LIGHT_MUTED;
}
static inline lv_color_t ui_accent_color(void) {
    return dark_mode_enabled ? COLOR_DARK_ACCENT : COLOR_LIGHT_ACCENT;
}

/* ========== Style helpers ========== */
void style_surface_panel(lv_obj_t *obj, lv_coord_t radius);
void style_popup_card(lv_obj_t *popup, lv_coord_t radius, lv_color_t accent);
void style_neutral_button(lv_obj_t *btn);

/* ========== Screen helpers ========== */
lv_obj_t *ui_screen_clear(void);
lv_obj_t *ui_create_top_bar(lv_obj_t *parent, const char *title,
                            lv_event_cb_t on_back, void *user_data);
lv_obj_t *ui_create_tile(lv_obj_t *parent, const char *icon,
                          const char *label_text, lv_color_t accent,
                          lv_event_cb_t on_click, void *user_data);

/* ========== NVS persistence ========== */
void save_dark_mode_to_nvs(bool enabled);
void save_boot_sound_to_nvs(boot_sound_mode_t mode);
void load_settings_from_nvs(void);

/* ========== Settings screen ========== */
void show_settings_screen(void);

#endif
