#include "ui_helpers.h"
#include "home_screen.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_system.h"

#define NVS_NAMESPACE       "settings"
#define NVS_KEY_DARK_MODE   "dark_mode"
#define NVS_KEY_BOOT_SOUND  "boot_sound"
#define NVS_KEY_UART_PORT   "uart_port"

static const char *TAG = "ui_helpers";

bool dark_mode_enabled = true;
boot_sound_mode_t boot_sound_mode = BOOT_SOUND_NOKIA;

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
        lv_obj_set_ext_click_area(btn, 18);
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
    lv_obj_center(popup);
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

    ui_create_top_bar(scr, "Settings", on_settings_back, NULL);

    lv_obj_t *cont = lv_obj_create(scr);
    lv_obj_set_size(cont, LV_PCT(90), LV_SIZE_CONTENT);
    lv_obj_align(cont, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_style_bg_color(cont, ui_card_color(), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(cont, 12, 0);
    lv_obj_set_style_border_width(cont, 1, 0);
    lv_obj_set_style_border_color(cont, ui_border_color(), 0);
    lv_obj_set_style_border_opa(cont, dark_mode_enabled ? LV_OPA_40 : LV_OPA_80, 0);
    lv_obj_set_style_pad_all(cont, 16, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(cont, 12, 0);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

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
}
