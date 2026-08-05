#include "file_admin_ap_screen.h"

#include "bsp/m5stack_core_s3.h"
#include "home_screen.h"
#include "nvs.h"
#include "esp_log.h"
#include "uart_handler.h"
#include "ui_helpers.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define FILE_ADMIN_AP_NVS_NAMESPACE  "settings"
#define FILE_ADMIN_AP_NVS_KEY_PASS   "admin_ap_pw"
#define FILE_ADMIN_AP_PASS_MAX_LEN   63

static const char *TAG = "file_admin_ap";
static const char *DEFAULT_PASSWORD = "12345678";

typedef enum {
    FILE_ADMIN_STATUS_INFO = 0,
    FILE_ADMIN_STATUS_OK,
    FILE_ADMIN_STATUS_WARN,
    FILE_ADMIN_STATUS_ERROR,
} file_admin_status_t;

static char s_password[FILE_ADMIN_AP_PASS_MAX_LEN + 1];
static char s_status_text[160];
static lv_obj_t *s_password_lbl;
static lv_obj_t *s_status_lbl;
static bool s_starting;
static bool s_locked;
static file_admin_status_t s_status_kind;

static void show_config_screen(void);
static void show_locked_screen(void);

static void reset_widget_refs(void)
{
    s_password_lbl = NULL;
    s_status_lbl = NULL;
}

static lv_color_t status_color(file_admin_status_t kind)
{
    switch (kind) {
    case FILE_ADMIN_STATUS_OK:
        return UI_ACCENT_GREEN;
    case FILE_ADMIN_STATUS_WARN:
        return UI_ACCENT_ORANGE;
    case FILE_ADMIN_STATUS_ERROR:
        return UI_ACCENT_RED;
    case FILE_ADMIN_STATUS_INFO:
    default:
        return UI_ACCENT_CYAN;
    }
}

static void set_status(file_admin_status_t kind, const char *text)
{
    s_status_kind = kind;
    if (text && text[0]) {
        strncpy(s_status_text, text, sizeof(s_status_text) - 1);
        s_status_text[sizeof(s_status_text) - 1] = '\0';
    } else {
        s_status_text[0] = '\0';
    }

    if (s_status_lbl && lv_obj_is_valid(s_status_lbl)) {
        lv_label_set_text(s_status_lbl, s_status_text);
        lv_obj_set_style_text_color(s_status_lbl, status_color(s_status_kind), 0);
    }
}

static void format_password_line(char *buf, size_t buf_sz)
{
    snprintf(buf, buf_sz, "Password: %s", s_password);
}

static void refresh_password_label(void)
{
    if (!s_password_lbl || !lv_obj_is_valid(s_password_lbl)) {
        return;
    }

    char line[96];
    format_password_line(line, sizeof(line));
    lv_label_set_text(s_password_lbl, line);
}

static void load_password_from_nvs(void)
{
    nvs_handle_t nvs;
    size_t required = sizeof(s_password);

    strncpy(s_password, DEFAULT_PASSWORD, sizeof(s_password) - 1);
    s_password[sizeof(s_password) - 1] = '\0';

    esp_err_t err = nvs_open(FILE_ADMIN_AP_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return;
    }

    err = nvs_get_str(nvs, FILE_ADMIN_AP_NVS_KEY_PASS, s_password, &required);
    if (err != ESP_OK || s_password[0] == '\0') {
        strncpy(s_password, DEFAULT_PASSWORD, sizeof(s_password) - 1);
        s_password[sizeof(s_password) - 1] = '\0';
    }

    nvs_close(nvs);
}

static bool save_password_to_nvs(const char *password)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(FILE_ADMIN_AP_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_set_str(nvs, FILE_ADMIN_AP_NVS_KEY_PASS, password);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS save failed: %s", esp_err_to_name(err));
        return false;
    }

    return true;
}

static bool validate_password(const char *password, char *err_buf, size_t err_buf_sz)
{
    size_t len;

    if (!password) {
        snprintf(err_buf, err_buf_sz, "Password is required.");
        return false;
    }

    len = strlen(password);
    if (len < 8 || len > FILE_ADMIN_AP_PASS_MAX_LEN) {
        snprintf(err_buf, err_buf_sz, "Password must be 8-63 chars.");
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        if (isspace((unsigned char)password[i])) {
            snprintf(err_buf, err_buf_sz, "Password cannot contain spaces.");
            return false;
        }
    }

    return true;
}

static void on_back(lv_event_t *e)
{
    (void)e;

    if (s_locked) {
        return;
    }

    if (s_starting) {
        uart_set_line_callback(NULL);
        uart_send_command("stop");
        s_starting = false;
    }

    reset_widget_refs();
    ui_screen_idle_inhibit(false);
    show_home_screen();
}

static void on_password_saved(const char *text, void *user_data)
{
    char validation_error[80];

    (void)user_data;

    if (!validate_password(text, validation_error, sizeof(validation_error))) {
        set_status(FILE_ADMIN_STATUS_WARN, validation_error);
        show_config_screen();
        return;
    }

    strncpy(s_password, text, sizeof(s_password) - 1);
    s_password[sizeof(s_password) - 1] = '\0';

    if (!save_password_to_nvs(s_password)) {
        set_status(FILE_ADMIN_STATUS_ERROR, "Failed to save password to NVS.");
        show_config_screen();
        return;
    }

    set_status(FILE_ADMIN_STATUS_OK, "Password saved to NVS.");
    show_config_screen();
}

static void on_password_cancel(void *user_data)
{
    (void)user_data;
}

static void on_set_password(lv_event_t *e)
{
    (void)e;

    if (s_starting || s_locked) {
        return;
    }

    ui_show_text_input_popup("Admin AP Password",
                             s_password,
                             FILE_ADMIN_AP_PASS_MAX_LEN,
                             UI_ACCENT_ORANGE,
                             on_password_saved,
                             on_password_cancel,
                             NULL);
}

static void on_start_result_line(const char *line)
{
    if (!s_starting || !line) {
        return;
    }

    if (strstr(line, "Captive portal started successfully") != NULL ||
        strstr(line, "Admin portal started. Connect to 'JanOS-Admin' (WPA2) and open http://172.0.0.1") != NULL ||
        strstr(line, "Admin portal started.") != NULL) {
        uart_set_line_callback(NULL);
        s_starting = false;
        s_locked = true;
        bsp_display_lock(0);
        ui_screen_idle_inhibit(true);
        show_locked_screen();
        bsp_display_unlock();
        return;
    }

    if (strstr(line, "Failed") != NULL ||
        strstr(line, "FAILED") != NULL ||
        strstr(line, "Usage:") != NULL ||
        strstr(line, "Error") != NULL ||
        strstr(line, "error") != NULL) {
        uart_set_line_callback(NULL);
        s_starting = false;
        bsp_display_lock(0);
        set_status(FILE_ADMIN_STATUS_ERROR, line);
        show_config_screen();
        bsp_display_unlock();
    }
}

static void on_start_ap(lv_event_t *e)
{
    char validation_error[80];
    char cmd[128];

    (void)e;

    if (s_starting || s_locked) {
        return;
    }

    if (!validate_password(s_password, validation_error, sizeof(validation_error))) {
        set_status(FILE_ADMIN_STATUS_WARN, validation_error);
        show_config_screen();
        return;
    }

    s_starting = true;
    set_status(FILE_ADMIN_STATUS_INFO, "Starting admin portal...");
    show_config_screen();

    snprintf(cmd, sizeof(cmd), "start_admin_portal %s", s_password);
    uart_set_line_callback(on_start_result_line);
    uart_send_command(cmd);
}

static void build_action_button(lv_obj_t *parent,
                                const char *text,
                                lv_color_t color,
                                lv_event_cb_t cb,
                                bool disabled)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, LV_PCT(100), 38);
    lv_obj_set_style_bg_color(btn, color, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_bg_opa(btn, disabled ? LV_OPA_40 : LV_OPA_COVER, 0);
    if (disabled) {
        lv_obj_add_state(btn, LV_STATE_DISABLED);
    }
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl);
}

static void show_config_screen(void)
{
    reset_widget_refs();

    lv_obj_t *scr = ui_screen_clear();
    lv_obj_t *top_bar = ui_create_top_bar(scr, "File Admin AP", on_back, NULL);

    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_set_size(card, LV_PCT(90), 182);
    lv_obj_align_to(card, top_bar, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
    style_surface_panel(card, 12);
    lv_obj_set_style_bg_color(card, ui_card_color(), 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_style_pad_row(card, 10, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Admin portal over JanOS-Admin AP");
    lv_obj_set_style_text_color(title, ui_text_color(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);

    s_password_lbl = lv_label_create(card);
    refresh_password_label();
    lv_obj_set_style_text_color(s_password_lbl, UI_ACCENT_ORANGE, 0);
    lv_obj_set_style_text_font(s_password_lbl, &lv_font_montserrat_14, 0);

    s_status_lbl = lv_label_create(card);
    lv_obj_set_width(s_status_lbl, LV_PCT(100));
    lv_label_set_long_mode(s_status_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_12, 0);
    set_status(s_status_kind, s_status_text);

    build_action_button(card,
                        "Set Password",
                        UI_ACCENT_ORANGE,
                        on_set_password,
                        s_starting);
    build_action_button(card,
                        s_starting ? "Waiting for firmware..." : "Start AP",
                        UI_ACCENT_GREEN,
                        on_start_ap,
                        s_starting);
}

static void show_locked_screen(void)
{
    char message[256];

    reset_widget_refs();

    lv_obj_t *scr = ui_screen_clear();

    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_set_size(card, LV_PCT(92), LV_PCT(78));
    lv_obj_center(card);
    style_popup_card(card, 14, UI_ACCENT_GREEN);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_style_pad_row(card, 12, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = lv_label_create(card);
    lv_label_set_text(icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(icon, UI_ACCENT_GREEN, 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, 0);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "File Admin AP Active");
    lv_obj_set_style_text_color(title, ui_text_color(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);

    snprintf(message,
             sizeof(message),
             "Join JanOS-Admin with the password %s, then open http://172.0.0.1/, reboot device to quit",
             s_password);

    lv_obj_t *msg = lv_label_create(card);
    lv_label_set_text(msg, message);
    lv_obj_set_width(msg, LV_PCT(100));
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(msg, ui_text_color(), 0);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_14, 0);
}

void show_file_admin_ap_screen(void)
{
    load_password_from_nvs();
    s_starting = false;
    s_locked = false;
    reset_widget_refs();
    ui_screen_idle_inhibit(false);
    set_status(FILE_ADMIN_STATUS_INFO, "Set password and start AP.");
    show_config_screen();
}