#ifndef UI_HELPERS_H
#define UI_HELPERS_H

#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>
#include "uart_handler.h"

/** Default LCD brightness; keep in sync with first `bsp_display_brightness_set` in app_main. */
#define UI_DEFAULT_BRIGHTNESS  80

/** Reduced LCD brightness used during high-current RF ops (e.g. nRF24 jammer)
 *  to free up power-rail headroom on shared-supply setups. Kept non-zero so
 *  the on-screen Stop control stays visible. */
#define UI_LOW_POWER_BRIGHTNESS  10

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

typedef enum {
    NFC_BUS_MODE_SPI = 0,
    NFC_BUS_MODE_I2C = 1,
    NFC_BUS_MODE_PN532 = 2,
} nfc_bus_mode_t;

extern nfc_bus_mode_t nfc_bus_mode;

/** Seconds of LVGL inactivity before backlight off; 0 = never. */
extern uint16_t screen_off_timeout_s;

/** When true, CoreS3 reads the stacked M5 GPS v2.1 and pushes fixes to firmware. */
extern bool external_gps_enabled;

/** Screen-lock feature master switch (NVS `scr_lock`, default on). When off the
 *  status-bar lock button is hidden and auto-lock never triggers. */
extern bool screen_lock_enabled;

/** When true, the screen locks automatically as it dims/blanks on idle
 *  (NVS `scr_lock_dim`, default off). Only effective while `screen_lock_enabled`. */
extern bool auto_lock_on_dim;

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
lv_obj_t *ui_add_top_bar_action(lv_obj_t *bar, const char *symbol,
                                lv_event_cb_t cb, void *user_data);
lv_obj_t *ui_create_tile(lv_obj_t *parent, const char *icon,
                          const char *label_text, lv_color_t accent,
                          lv_event_cb_t on_click, void *user_data);

/* ========== NVS persistence ========== */
void save_dark_mode_to_nvs(bool enabled);
void save_boot_sound_to_nvs(boot_sound_mode_t mode);
void save_nfc_bus_to_nvs(nfc_bus_mode_t mode);
void save_uart_port_to_nvs(uart_port_mode_t mode);
void save_external_gps_to_nvs(bool enabled);
void save_screen_timeout_to_nvs(uint16_t seconds);
void save_red_team_to_nvs(bool enabled);
void save_screen_lock_to_nvs(bool enabled);
void save_auto_lock_dim_to_nvs(bool enabled);
void load_settings_from_nvs(void);

/** Returns true when Red Team attacks are enabled (NVS, default off). */
bool ui_red_team_enabled(void);

/** Start periodic check for screen idle (call after display + brightness init). */
void ui_screen_timeout_init(void);

/** While inhibited, the screen-off idle timeout is suspended (screen stays
 *  awake). Use during long-running RF ops so the backlight never powers
 *  down/up under load (avoids a brownout-inducing current spike). */
void ui_screen_idle_inhibit(bool inhibit);

/**
 * Enter/leave display low-power mode for high-current RF operations.
 * On enter: suspends the idle timeout and drops the LCD backlight to
 * UI_LOW_POWER_BRIGHTNESS so the CoreS3 draws less from a shared power rail
 * (mitigates nRF24-jammer brownouts). On leave: restores UI_DEFAULT_BRIGHTNESS
 * and re-enables the idle timeout.
 */
void ui_screen_low_power(bool enable);

/* ========== Screen lock ========== */

/**
 * Lock the screen: install a full-screen "slide to unlock" overlay on the
 * LVGL top layer that captures every touch so nothing underneath can be
 * activated. No-op if a lock overlay is already active (never stacks two).
 */
void ui_screen_lock_now(void);

/** True while the slide-to-unlock overlay is present. */
bool ui_screen_lock_active(void);

/* ========== Settings screen ========== */
void show_settings_screen(void);

/**
 * Take the LVGL display mutex, waiting indefinitely. Use from non-LVGL
 * threads (e.g. uart_rx) before any lv_* API.
 */
bool ui_display_lock_wait(void);

/** Non-blocking; use from LVGL timers so touch input is not delayed. */
bool ui_display_lock_try(void);

void ui_display_unlock_safe(void);

/** Decorative top-bar child: visible but does not steal touches. */
void ui_top_bar_pass_through(lv_obj_t *obj);

/**
 * Queue work on the LVGL task from another thread: takes the display mutex,
 * calls lv_async_call, then releases. Safe from uart_rx / parse_worker.
 */
bool ui_lvgl_async_call(lv_async_cb_t cb, void *user_data);

/* ========== CardKB text input popup ========== */

typedef void (*ui_text_input_confirm_cb_t)(const char *text, void *user_data);
typedef void (*ui_text_input_cancel_cb_t)(void *user_data);

/**
 * Modal one-line text input popup driven by the CardKB.
 *
 * Layout: full-screen overlay + centered card with a title label, a
 * one-line lv_textarea (prefilled with `initial`), and OK / Cancel
 * buttons. The popup polls cardkb_read_key() internally:
 *   - printable ASCII -> lv_textarea_add_char
 *   - 0x08 / 0x7F     -> lv_textarea_delete_char (backspace)
 *   - 0x0D / 0x0A     -> confirm (same as OK)
 *   - 0x1B            -> cancel (same as Cancel)
 *
 * Either callback may be NULL. The popup destroys itself in both
 * outcomes before invoking the callback, so callers can open another
 * popup from within `on_confirm`.
 */
void ui_show_text_input_popup(const char *title,
                              const char *initial,
                              uint32_t max_len,
                              lv_color_t accent,
                              ui_text_input_confirm_cb_t on_confirm,
                              ui_text_input_cancel_cb_t on_cancel,
                              void *user_data);

#endif
