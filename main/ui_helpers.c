#include "ui_helpers.h"
#include "home_screen.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_system.h"
#include "bsp/m5stack_core_s3.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>

#define NVS_NAMESPACE       "settings"
#define NVS_KEY_DARK_MODE   "dark_mode"
#define NVS_KEY_BOOT_SOUND  "boot_sound"
#define NVS_KEY_UART_PORT   "uart_port"
#define NVS_KEY_SCREEN_OFF  "screen_off_s"

#define SCREEN_IDLE_POLL_MS           500

/** Must match `ui_create_top_bar` height in this file. */
#define UI_TOP_BAR_H                  36

static const char *TAG = "ui_helpers";

bool dark_mode_enabled = true;
boot_sound_mode_t boot_sound_mode = BOOT_SOUND_NOKIA;
uint16_t screen_off_timeout_s = 0;

static const uint16_t k_screen_timeout_sec[] = { 0, 30, 60, 120, 300, 600 };
#define K_SCREEN_TIMEOUT_OPTS  (sizeof(k_screen_timeout_sec) / sizeof(k_screen_timeout_sec[0]))

static lv_timer_t *s_screen_idle_timer;
static lv_obj_t *s_wake_blocker;
static bool s_bl_asleep;
static int s_saved_brightness = UI_DEFAULT_BRIGHTNESS;

/** Settings top bar; used to keep Back above dropdown lists / popups. Cleared on LV_EVENT_DELETE. */
static lv_obj_t *s_settings_top_bar;

static void on_settings_bar_deleted(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_DELETE) {
        s_settings_top_bar = NULL;
    }
}

static void on_settings_dropdown_raise_bar(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_READY) {
        return;
    }
    lv_obj_t *bar = lv_event_get_user_data(e);
    if (bar) {
        lv_obj_move_to_index(bar, -1);
    }
}

static void screen_wake_blocker_remove(void)
{
    if (s_wake_blocker) {
        lv_obj_del(s_wake_blocker);
        s_wake_blocker = NULL;
    }
}

static void on_wake_blocker_pressed(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_PRESSED) {
        return;
    }

    lv_event_stop_processing(e);

    lv_obj_t *blocker = lv_event_get_target(e);
    s_wake_blocker = NULL;

    bsp_display_brightness_set(s_saved_brightness);
    s_bl_asleep = false;
    lv_obj_del(blocker);
}

static void screen_wake_blocker_install(void)
{
    screen_wake_blocker_remove();

    lv_display_t *disp = lv_display_get_default();
    if (!disp) {
        return;
    }

    lv_obj_t *layer = lv_layer_top();
    if (!layer) {
        return;
    }

    s_wake_blocker = lv_obj_create(layer);
    lv_obj_set_size(s_wake_blocker,
                    lv_display_get_horizontal_resolution(disp),
                    lv_display_get_vertical_resolution(disp));
    lv_obj_align(s_wake_blocker, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(s_wake_blocker, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_wake_blocker, 0, 0);
    lv_obj_clear_flag(s_wake_blocker, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_wake_blocker, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_wake_blocker, on_wake_blocker_pressed, LV_EVENT_PRESSED, NULL);
}

static uint32_t screen_timeout_sec_to_dd_index(uint16_t sec)
{
    for (uint32_t i = 0; i < K_SCREEN_TIMEOUT_OPTS; i++) {
        if (k_screen_timeout_sec[i] == sec) {
            return i;
        }
    }
    return 0;
}

static uint16_t screen_timeout_dd_index_to_sec(uint32_t idx)
{
    if (idx >= K_SCREEN_TIMEOUT_OPTS) {
        return 0;
    }
    return k_screen_timeout_sec[idx];
}

static void screen_idle_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (screen_off_timeout_s == 0) {
        if (s_bl_asleep) {
            screen_wake_blocker_remove();
            bsp_display_brightness_set(s_saved_brightness);
            s_bl_asleep = false;
        }
        return;
    }

    uint32_t inactive_ms = lv_display_get_inactive_time(NULL);
    uint32_t timeout_ms = (uint32_t)screen_off_timeout_s * 1000u;

    if (s_bl_asleep) {
        return;
    }

    if (inactive_ms >= timeout_ms) {
        s_saved_brightness = UI_DEFAULT_BRIGHTNESS;
        bsp_display_brightness_set(0);
        s_bl_asleep = true;
        screen_wake_blocker_install();
    }
}

/* ------------------------------------------------------------------ */
/*  Style helpers                                                      */
/* ------------------------------------------------------------------ */

void style_surface_panel(lv_obj_t *obj, lv_coord_t radius)
{
    if (!obj) return;
    lv_obj_set_style_bg_color(obj, ui_panel_color(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, ui_border_color(), 0);
    lv_obj_set_style_border_opa(obj, dark_mode_enabled ? LV_OPA_40 : LV_OPA_80, 0);
    lv_obj_set_style_radius(obj, radius, 0);
}

void style_popup_card(lv_obj_t *popup, lv_coord_t radius, lv_color_t accent)
{
    if (!popup) return;
    style_surface_panel(popup, radius);
    lv_obj_set_style_bg_color(popup, ui_card_color(), 0);
    lv_obj_set_style_border_color(popup, accent, 0);
    lv_obj_set_style_border_opa(popup, dark_mode_enabled ? LV_OPA_70 : LV_OPA_90, 0);
    lv_obj_set_style_shadow_width(popup, dark_mode_enabled ? 20 : 10, 0);
    lv_obj_set_style_shadow_color(popup, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(popup, dark_mode_enabled ? LV_OPA_30 : LV_OPA_20, 0);
}

void style_neutral_button(lv_obj_t *btn)
{
    if (!btn) return;
    lv_obj_set_style_bg_color(btn,
        dark_mode_enabled ? lv_color_hex(0x13263C) : lv_color_hex(0xD4DFEA), 0);
    lv_obj_set_style_bg_color(btn,
        dark_mode_enabled ? lv_color_hex(0x1E3550) : lv_color_hex(0xC3D1E1),
        LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, ui_border_color(), 0);
    lv_obj_set_style_border_opa(btn, dark_mode_enabled ? LV_OPA_70 : LV_OPA_100, 0);
    lv_obj_set_style_radius(btn, 8, 0);
}

/* ------------------------------------------------------------------ */
/*  Screen helpers                                                     */
/* ------------------------------------------------------------------ */

lv_obj_t *ui_screen_clear(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, ui_bg_color(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    return scr;
}

lv_obj_t *ui_create_top_bar(lv_obj_t *parent, const char *title,
                            lv_event_cb_t on_back, void *user_data)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, LV_PCT(100), 36);
    lv_obj_set_style_bg_color(bar, ui_panel_color(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_hor(bar, 6, 0);
    lv_obj_set_style_pad_ver(bar, 0, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    if (on_back) {
        lv_obj_t *btn = lv_btn_create(bar);
        lv_obj_set_size(btn, 60, 28);
        lv_obj_set_style_bg_color(btn, UI_ACCENT_BLUE, 0);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        /* Extend invisible touch area so the button is easy to hit with a finger,
         * including the very top-left corner of the screen. */
        lv_obj_set_ext_click_area(btn, 40);
        lv_obj_set_user_data(btn, user_data);
        lv_obj_add_event_cb(btn, on_back, LV_EVENT_CLICKED, user_data);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, LV_SYMBOL_LEFT " Back");
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_center(lbl);
    }

    lv_obj_t *lbl = lv_label_create(bar);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_color(lbl, ui_text_color(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_flex_grow(lbl, 1);
    if (on_back)
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);

    return bar;
}

lv_obj_t *ui_add_top_bar_action(lv_obj_t *bar, const char *symbol,
                                lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_btn_create(bar);
    lv_obj_set_size(btn, 28, 28);
    lv_obj_set_style_bg_color(btn, ui_card_color(), 0);
    lv_obj_set_style_bg_color(btn, ui_card_pressed_color(), LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_style_margin_left(btn, 4, 0);
    if (cb) {
        lv_obj_set_user_data(btn, user_data);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    }

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, symbol ? symbol : LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(lbl, ui_muted_color(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl);
    return btn;
}

lv_obj_t *ui_create_tile(lv_obj_t *parent, const char *icon,
                          const char *label_text, lv_color_t accent,
                          lv_event_cb_t on_click, void *user_data)
{
    lv_obj_t *tile = lv_btn_create(parent);
    lv_obj_set_size(tile, LV_PCT(46), 48);
    lv_obj_set_style_bg_color(tile, ui_card_color(), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(tile, ui_card_pressed_color(), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(tile, 0, 0);
    lv_obj_set_style_radius(tile, 8, 0);
    lv_obj_set_style_shadow_width(tile, 0, 0);
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(tile, 4, 0);
    lv_obj_set_style_pad_row(tile, 2, 0);

    if (icon) {
        lv_obj_t *icon_label = lv_label_create(tile);
        lv_label_set_text(icon_label, icon);
        lv_obj_set_style_text_font(icon_label, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(icon_label, accent, 0);
    }

    if (label_text) {
        lv_obj_t *text_label = lv_label_create(tile);
        lv_label_set_text(text_label, label_text);
        lv_obj_set_style_text_font(text_label, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(text_label, ui_text_color(), 0);
        lv_obj_set_style_text_align(text_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(text_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(text_label, LV_PCT(90));
    }

    if (on_click) {
        lv_obj_set_user_data(tile, user_data);
        lv_obj_add_event_cb(tile, on_click, LV_EVENT_CLICKED, user_data);
    }

    return tile;
}

/* ------------------------------------------------------------------ */
/*  NVS persistence                                                    */
/* ------------------------------------------------------------------ */

void save_dark_mode_to_nvs(bool enabled)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        nvs_set_u8(nvs, NVS_KEY_DARK_MODE, enabled ? 1 : 0);
        nvs_commit(nvs);
        nvs_close(nvs);
    } else {
        ESP_LOGW(TAG, "NVS open for write failed: %s", esp_err_to_name(err));
    }
}

void save_boot_sound_to_nvs(boot_sound_mode_t mode)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        nvs_set_u8(nvs, NVS_KEY_BOOT_SOUND, (uint8_t)mode);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

void save_uart_port_to_nvs(uart_port_mode_t mode)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        nvs_set_u8(nvs, NVS_KEY_UART_PORT, (uint8_t)mode);
        nvs_commit(nvs);
        nvs_close(nvs);
    } else {
        ESP_LOGW(TAG, "NVS open for write failed: %s", esp_err_to_name(err));
    }
}

void save_screen_timeout_to_nvs(uint16_t seconds)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        nvs_set_u16(nvs, NVS_KEY_SCREEN_OFF, seconds);
        nvs_commit(nvs);
        nvs_close(nvs);
    } else {
        ESP_LOGW(TAG, "NVS open for write failed: %s", esp_err_to_name(err));
    }
}

void load_settings_from_nvs(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) return;

    uint8_t dark_mode = 1;
    err = nvs_get_u8(nvs, NVS_KEY_DARK_MODE, &dark_mode);
    if (err == ESP_OK) {
        dark_mode_enabled = (dark_mode != 0);
    }

    uint8_t bsound = BOOT_SOUND_NOKIA;
    err = nvs_get_u8(nvs, NVS_KEY_BOOT_SOUND, &bsound);
    if (err == ESP_OK && bsound <= BOOT_SOUND_STARWARS) {
        boot_sound_mode = (boot_sound_mode_t)bsound;
    }

    uint8_t uport = UART_PORT_MODE_MBUS;
    err = nvs_get_u8(nvs, NVS_KEY_UART_PORT, &uport);
    if (err == ESP_OK && uport <= UART_PORT_MODE_PORTC) {
        uart_port_mode = (uart_port_mode_t)uport;
    }

    uint16_t off_s = 0;
    err = nvs_get_u16(nvs, NVS_KEY_SCREEN_OFF, &off_s);
    if (err == ESP_OK) {
        bool known = false;
        for (uint32_t i = 0; i < K_SCREEN_TIMEOUT_OPTS; i++) {
            if (k_screen_timeout_sec[i] == off_s) {
                known = true;
                break;
            }
        }
        if (known) {
            screen_off_timeout_s = off_s;
        }
    }

    nvs_close(nvs);
}

/* ------------------------------------------------------------------ */
/*  Settings screen                                                    */
/* ------------------------------------------------------------------ */

static void on_settings_back(lv_event_t *e)
{
    (void)e;
    show_home_screen();
}

static void on_dark_mode_toggle(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (dark_mode_enabled == enabled) return;

    dark_mode_enabled = enabled;
    save_dark_mode_to_nvs(dark_mode_enabled);
    show_settings_screen();
}

static void on_boot_sound_changed(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    uint32_t sel = lv_dropdown_get_selected(dd);
    if (sel <= BOOT_SOUND_STARWARS) {
        boot_sound_mode = (boot_sound_mode_t)sel;
        save_boot_sound_to_nvs(boot_sound_mode);
    }
}

static void on_restart_now(lv_event_t *e)
{
    (void)e;
    esp_restart();
}

static void on_restart_later(lv_event_t *e)
{
    lv_obj_t *popup = lv_event_get_user_data(e);
    if (popup) lv_obj_del(popup);
}

static void show_uart_restart_popup(void)
{
    lv_obj_t *scr = lv_scr_act();

    lv_obj_t *popup = lv_obj_create(scr);
    lv_obj_set_size(popup, LV_PCT(80), LV_SIZE_CONTENT);
    lv_obj_align(popup, LV_ALIGN_BOTTOM_MID, 0, -8);
    style_popup_card(popup, 12, UI_ACCENT_ORANGE);
    lv_obj_set_style_pad_all(popup, 16, 0);
    lv_obj_set_flex_flow(popup, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(popup, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(popup, 12, 0);
    lv_obj_clear_flag(popup, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *msg = lv_label_create(popup);
    lv_label_set_text(msg, "Restart required to apply UART port change.");
    lv_obj_set_style_text_color(msg, ui_text_color(), 0);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg, LV_PCT(100));

    lv_obj_t *btn_row = lv_obj_create(popup);
    lv_obj_set_size(btn_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *later_btn = lv_btn_create(btn_row);
    lv_obj_set_size(later_btn, 100, 36);
    style_neutral_button(later_btn);
    lv_obj_add_event_cb(later_btn, on_restart_later, LV_EVENT_CLICKED, popup);
    lv_obj_t *llbl = lv_label_create(later_btn);
    lv_label_set_text(llbl, "Later");
    lv_obj_set_style_text_color(llbl, ui_text_color(), 0);
    lv_obj_set_style_text_font(llbl, &lv_font_montserrat_14, 0);
    lv_obj_center(llbl);

    lv_obj_t *now_btn = lv_btn_create(btn_row);
    lv_obj_set_size(now_btn, 120, 36);
    lv_obj_set_style_bg_color(now_btn, UI_ACCENT_ORANGE, 0);
    lv_obj_set_style_radius(now_btn, 8, 0);
    lv_obj_add_event_cb(now_btn, on_restart_now, LV_EVENT_CLICKED, NULL);
    lv_obj_t *nlbl = lv_label_create(now_btn);
    lv_label_set_text(nlbl, "Restart now");
    lv_obj_set_style_text_color(nlbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(nlbl, &lv_font_montserrat_14, 0);
    lv_obj_center(nlbl);

    if (s_settings_top_bar) {
        lv_obj_move_to_index(s_settings_top_bar, -1);
    }
}

static void on_uart_port_changed(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    uint32_t sel = lv_dropdown_get_selected(dd);
    if (sel > UART_PORT_MODE_PORTC) return;
    if ((uart_port_mode_t)sel == uart_port_mode) return;

    uart_port_mode = (uart_port_mode_t)sel;
    save_uart_port_to_nvs(uart_port_mode);
    show_uart_restart_popup();
}

static void on_screen_timeout_changed(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    uint32_t sel = lv_dropdown_get_selected(dd);
    uint16_t sec = screen_timeout_dd_index_to_sec(sel);
    if (sec == screen_off_timeout_s) {
        return;
    }

    screen_off_timeout_s = sec;
    save_screen_timeout_to_nvs(sec);

    if (sec == 0 && s_bl_asleep) {
        screen_wake_blocker_remove();
        bsp_display_brightness_set(s_saved_brightness);
        s_bl_asleep = false;
    }
}

static lv_obj_t *create_settings_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 4, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

void show_settings_screen(void)
{
    lv_obj_t *scr = ui_screen_clear();

    lv_obj_t *top_bar = ui_create_top_bar(scr, "Settings", on_settings_back, NULL);
    s_settings_top_bar = top_bar;
    lv_obj_add_event_cb(top_bar, on_settings_bar_deleted, LV_EVENT_DELETE, NULL);

    lv_obj_t *cont = lv_obj_create(scr);
    lv_obj_set_size(cont, LV_PCT(90), LV_SIZE_CONTENT);
    lv_obj_align_to(cont, top_bar, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);
    {
        lv_display_t *disp = lv_display_get_default();
        int32_t vres = disp ? (int32_t)lv_display_get_vertical_resolution(disp) : 240;
        int32_t max_h = vres - UI_TOP_BAR_H - 6 - 8;
        if (max_h < 80) {
            max_h = 80;
        }
        lv_obj_set_style_max_height(cont, max_h, 0);
    }
    lv_obj_add_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(cont, ui_card_color(), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(cont, 12, 0);
    lv_obj_set_style_border_width(cont, 1, 0);
    lv_obj_set_style_border_color(cont, ui_border_color(), 0);
    lv_obj_set_style_border_opa(cont, dark_mode_enabled ? LV_OPA_40 : LV_OPA_80, 0);
    lv_obj_set_style_pad_all(cont, 16, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(cont, 12, 0);

    /* Dark mode row */
    lv_obj_t *dark_row = create_settings_row(cont);

    lv_obj_t *label = lv_label_create(dark_row);
    lv_label_set_text(label, "Dark mode");
    lv_obj_set_style_text_color(label, ui_text_color(), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);

    lv_obj_t *sw = lv_switch_create(dark_row);
    if (dark_mode_enabled) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, ui_muted_color(), 0);
    lv_obj_set_style_bg_color(sw, UI_ACCENT_BLUE, LV_STATE_CHECKED | LV_PART_INDICATOR);
    lv_obj_add_event_cb(sw, on_dark_mode_toggle, LV_EVENT_VALUE_CHANGED, NULL);

    /* Boot sound row */
    lv_obj_t *sound_row = create_settings_row(cont);

    lv_obj_t *slbl = lv_label_create(sound_row);
    lv_label_set_text(slbl, "Boot sound");
    lv_obj_set_style_text_color(slbl, ui_text_color(), 0);
    lv_obj_set_style_text_font(slbl, &lv_font_montserrat_14, 0);

    lv_obj_t *dd = lv_dropdown_create(sound_row);
    lv_dropdown_set_options(dd, "Off\nNokia\nIntel\nStar Wars");
    lv_dropdown_set_selected(dd, (uint32_t)boot_sound_mode);
    lv_obj_set_width(dd, 120);
    lv_obj_set_style_text_font(dd, &lv_font_montserrat_12, 0);
    lv_obj_set_style_bg_color(dd, ui_card_color(), 0);
    lv_obj_set_style_text_color(dd, ui_text_color(), 0);
    lv_obj_set_style_border_color(dd, ui_border_color(), 0);

    lv_obj_t *list = lv_dropdown_get_list(dd);
    if (list) {
        lv_obj_set_style_bg_color(list, ui_card_color(), 0);
        lv_obj_set_style_text_color(list, ui_text_color(), 0);
        lv_obj_set_style_border_color(list, ui_border_color(), 0);
        lv_obj_set_style_text_font(list, &lv_font_montserrat_12, 0);
    }

    lv_obj_add_event_cb(dd, on_boot_sound_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(dd, on_settings_dropdown_raise_bar, LV_EVENT_READY, top_bar);

    /* UART port row */
    lv_obj_t *uart_row = create_settings_row(cont);

    lv_obj_t *ulbl = lv_label_create(uart_row);
    lv_label_set_text(ulbl, "UART port");
    lv_obj_set_style_text_color(ulbl, ui_text_color(), 0);
    lv_obj_set_style_text_font(ulbl, &lv_font_montserrat_14, 0);

    lv_obj_t *udd = lv_dropdown_create(uart_row);
    lv_dropdown_set_options(udd, "MBus\nPort C");
    lv_dropdown_set_selected(udd, (uint32_t)uart_port_mode);
    lv_obj_set_width(udd, 120);
    lv_obj_set_style_text_font(udd, &lv_font_montserrat_12, 0);
    lv_obj_set_style_bg_color(udd, ui_card_color(), 0);
    lv_obj_set_style_text_color(udd, ui_text_color(), 0);
    lv_obj_set_style_border_color(udd, ui_border_color(), 0);

    lv_obj_t *ulist = lv_dropdown_get_list(udd);
    if (ulist) {
        lv_obj_set_style_bg_color(ulist, ui_card_color(), 0);
        lv_obj_set_style_text_color(ulist, ui_text_color(), 0);
        lv_obj_set_style_border_color(ulist, ui_border_color(), 0);
        lv_obj_set_style_text_font(ulist, &lv_font_montserrat_12, 0);
    }

    lv_obj_add_event_cb(udd, on_uart_port_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(udd, on_settings_dropdown_raise_bar, LV_EVENT_READY, top_bar);

    /* Screen timeout row */
    lv_obj_t *to_row = create_settings_row(cont);

    lv_obj_t *tolbl = lv_label_create(to_row);
    lv_label_set_text(tolbl, "Screen timeout");
    lv_obj_set_style_text_color(tolbl, ui_text_color(), 0);
    lv_obj_set_style_text_font(tolbl, &lv_font_montserrat_14, 0);

    lv_obj_t *todd = lv_dropdown_create(to_row);
    lv_dropdown_set_options(todd, "Never\n30 s\n1 min\n2 min\n5 min\n10 min");
    lv_dropdown_set_selected(todd, screen_timeout_sec_to_dd_index(screen_off_timeout_s));
    lv_obj_set_width(todd, 120);
    lv_obj_set_style_text_font(todd, &lv_font_montserrat_12, 0);
    lv_obj_set_style_bg_color(todd, ui_card_color(), 0);
    lv_obj_set_style_text_color(todd, ui_text_color(), 0);
    lv_obj_set_style_border_color(todd, ui_border_color(), 0);

    lv_obj_t *tolist = lv_dropdown_get_list(todd);
    if (tolist) {
        lv_obj_set_style_bg_color(tolist, ui_card_color(), 0);
        lv_obj_set_style_text_color(tolist, ui_text_color(), 0);
        lv_obj_set_style_border_color(tolist, ui_border_color(), 0);
        lv_obj_set_style_text_font(tolist, &lv_font_montserrat_12, 0);
    }

    lv_obj_add_event_cb(todd, on_screen_timeout_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(todd, on_settings_dropdown_raise_bar, LV_EVENT_READY, top_bar);

    lv_obj_move_to_index(top_bar, -1);
}

void ui_screen_timeout_init(void)
{
    if (s_screen_idle_timer) {
        return;
    }
    s_screen_idle_timer = lv_timer_create(screen_idle_timer_cb, SCREEN_IDLE_POLL_MS, NULL);
}

bool ui_display_lock_wait(void)
{
    return bsp_display_lock(portMAX_DELAY);
}

void ui_display_unlock_safe(void)
{
    bsp_display_unlock();
}

bool ui_lvgl_async_call(lv_async_cb_t cb, void *user_data)
{
    if (!cb)
        return false;
    if (!bsp_display_lock(portMAX_DELAY)) {
        ESP_LOGW(TAG, "ui_lvgl_async_call: display lock failed");
        return false;
    }
    lv_result_t r = lv_async_call(cb, user_data);
    bsp_display_unlock();
    if (r != LV_RESULT_OK) {
        ESP_LOGW(TAG, "lv_async_call failed");
        return false;
    }
    return true;
}
