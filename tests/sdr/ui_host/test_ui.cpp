// SPDX-License-Identifier: GPL-3.0-or-later
// Render and exercise the production screen with real LVGL and a fake worker.
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include "screens/sdr_screen.cpp"
#include "nvs.h"

static bool red_team = false;
static unsigned closes = 0;
static unsigned busy_opens = 0;
static std::map<std::string, int32_t> flash;
static uint16_t display_pixels[320 * 240];
static lv_point_t touch_point;
static lv_indev_state_t touch_state = LV_INDEV_STATE_RELEASED;
bool dark_mode_enabled = true;

void *heap_caps_malloc(std::size_t bytes, unsigned) { return std::malloc(bytes); }
void *heap_caps_calloc(std::size_t count, std::size_t bytes, unsigned) { return std::calloc(count, bytes); }
void heap_caps_free(void *pointer) { std::free(pointer); }
int64_t esp_timer_get_time() { return int64_t(lv_tick_get()) * 1000; }
extern "C" bool ui_red_team_enabled() { return red_team; }
extern "C" void show_home_screen() { ui_screen_clear(); }
extern "C" lv_obj_t *ui_screen_clear() {
    auto *screen = lv_screen_active();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, ui_bg_color(), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    return screen;
}
extern "C" void style_neutral_button(lv_obj_t *object) {
    lv_obj_set_style_bg_color(object, ui_card_color(), 0);
    lv_obj_set_style_border_color(object, ui_border_color(), 0);
}
extern "C" lv_obj_t *ui_create_top_bar(lv_obj_t *parent, const char *title, lv_event_cb_t back, void *data) {
    auto *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, 320, 36);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, ui_panel_color(), 0);
    auto *button = lv_button_create(bar);
    lv_obj_set_size(button, 60, 28);
    lv_obj_add_event_cb(button, back, LV_EVENT_CLICKED, data);
    auto *label = lv_label_create(button);
    lv_label_set_text(label, "Back"); lv_obj_center(label);
    label = lv_label_create(bar);
    lv_label_set_text(label, title);
    lv_obj_set_pos(label, 145, 8);
    lv_obj_set_style_text_color(label, ui_text_color(), 0);
    return bar;
}
esp_err_t nvs_open(const char *, int, nvs_handle_t *handle) { *handle = 1; return ESP_OK; }
void nvs_close(nvs_handle_t) {}
esp_err_t nvs_get_i32(nvs_handle_t, const char *key, int32_t *value) {
    if (!flash.count(key)) return ESP_ERR_NOT_FOUND;
    *value = flash[key]; return ESP_OK;
}
esp_err_t nvs_set_i32(nvs_handle_t, const char *key, int32_t value) { flash[key] = value; return ESP_OK; }
esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *value) {
    int32_t number = 0; auto error = nvs_get_i32(handle, key, &number); *value = number; return error;
}
esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value) { return nvs_set_i32(handle, key, value); }
esp_err_t nvs_commit(nvs_handle_t) { return ESP_OK; }

namespace sdr {
struct Session { bool allowed; Result result; std::vector<Command> commands; };
Session *open(const Settings &, bool allowed, esp_err_t &error) {
    if (busy_opens) { --busy_opens; error = ESP_ERR_INVALID_STATE; return nullptr; }
    error = ESP_OK;
    auto *session = new Session;
    session->allowed = allowed;
    session->result.initialized = true;
    session->result.gain_max = 82;
    return session;
}
bool send(Session *session, const Command &command) {
    session->commands.push_back(command);
    if (command.operation != Operation::Pause && command.operation != Operation::StopTx) session->result.generation = command.generation;
    if (command.operation == Operation::StartTx) { assert(session->allowed); session->result.tx.active = true; }
    if (command.operation == Operation::StopTx) session->result.tx.active = false;
    return true;
}
bool receive(Session *session, Result &result) {
    ++session->result.frame_sequence;
    for (int i = 0; i < PLOT_WIDTH; ++i) {
        session->result.frame.db[i] = -85 + 55 * std::exp(-std::pow((i - 120) / 10.0f, 2));
        session->result.frame.peak[i] = session->result.frame.db[i] + 4;
    }
    session->result.frame.strongest_db = -30;
    session->result.frame.gain = 40;
    if (session->result.tx.active) { session->result.tx.sent = 48; session->result.tx.remaining_ms = 4200; }
    result = session->result;
    return true;
}
void allow_tx(Session *session, bool allowed) { session->allowed = allowed; }
void close(Session *session) { if (session) { delete session; ++closes; } }
}
static void capture(const std::string &name) {
    lv_obj_update_layout(lv_screen_active());
    lv_refr_now(nullptr);
    std::ofstream file(name, std::ios::binary);
    file << "P6\n320 240\n255\n";
    for (auto pixel : display_pixels) {
        char rgb[3] = {char(((pixel >> 11) & 31) * 255 / 31), char(((pixel >> 5) & 63) * 255 / 63), char((pixel & 31) * 255 / 31)};
        file.write(rgb, 3);
    }
}
static void touch(lv_indev_t *input, int x, int y, lv_indev_state_t state) {
    touch_point = {x, y}; touch_state = state;
    lv_indev_read(input); lv_tick_inc(20); lv_timer_handler();
}
int main(int argc, char **argv) {
    assert(argc == 2);
    lv_init();
    auto *display = lv_display_create(320, 240);
    static uint16_t draw_buffer[320 * 240];
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, draw_buffer, nullptr, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, [](lv_display_t *display, const lv_area_t *area, uint8_t *data) {
        auto *source = reinterpret_cast<uint16_t *>(data);
        for (int y = area->y1; y <= area->y2; ++y)
            for (int x = area->x1; x <= area->x2; ++x) display_pixels[y * 320 + x] = *source++;
        lv_display_flush_ready(display);
    });
    auto *input = lv_indev_create();
    lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(input, [](lv_indev_t *, lv_indev_data_t *data) { data->point = touch_point; data->state = touch_state; });
    show_sdr_screen();
    auto *root = lv_obj_get_child(lv_screen_active(), 0);
    auto *screen = static_cast<Screen *>(lv_event_dsc_get_user_data(lv_obj_get_event_dsc(root, 0)));
    assert(screen->page == Page::Main && screen->session);
    screen->poll();
    assert(lv_obj_has_state(screen->tx_button, LV_STATE_DISABLED));
    for (int i = 0; i < 44; ++i) screen->poll();
    lv_tick_inc(1000);
    const auto inactive = lv_display_get_inactive_time(display);
    screen->poll();
    assert(lv_display_get_inactive_time(display) == inactive);
    capture(std::string(argv[1]) + "/main-dark.ppm");
    const auto initial = screen->settings.frequency_mhz;
    touch(input, 285, 105, LV_INDEV_STATE_PRESSED);
    touch(input, 285, 105, LV_INDEV_STATE_RELEASED);
    assert(screen->settings.frequency_mhz > initial);
    lv_obj_update_layout(root);
    const auto before_drag = screen->settings.frequency_mhz;
    touch(input, 170, 105, LV_INDEV_STATE_PRESSED);
    touch(input, 210, 105, LV_INDEV_STATE_PRESSED);
    touch(input, 210, 105, LV_INDEV_STATE_RELEASED);
    assert(screen->settings.frequency_mhz < before_drag);
    const auto locked_frequency = screen->settings.frequency_mhz;
    auto *overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, 320, 240);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_update_layout(lv_layer_top());
    touch(input, 285, 105, LV_INDEV_STATE_PRESSED);
    touch(input, 285, 105, LV_INDEV_STATE_RELEASED);
    assert(screen->settings.frequency_mhz == locked_frequency);
    lv_obj_delete(overlay);
    screen->action(SettingsPage);
    screen->action(101);
    assert(screen->draft.rate_mhz != screen->settings.rate_mhz);
    screen->action(Cancel);
    assert(screen->settings.rate_mhz == 40);
    screen->action(SettingsPage);
    screen->action(101);
    screen->action(Save);
    assert(screen->settings.rate_mhz == 80);
    screen->action(SettingsPage);
    screen->action(Defaults);
    assert(screen->draft.rate_mhz == 40 && screen->settings.rate_mhz == 80);
    screen->action(Cancel);
    assert(screen->settings.rate_mhz == 80);
    for (unsigned tab = 0; tab < 4; ++tab) {
        screen->action(SettingsPage); screen->action(200 + tab);
        capture(std::string(argv[1]) + "/settings-" + std::to_string(tab) + ".ppm");
        screen->action(Cancel);
    }
    screen->action(KeypadPage);
    screen->action(KeyOk);
    assert(screen->keypad_error && screen->page == Page::Keypad);
    for (int digit : {2, 4, 3, 7}) screen->action(300 + digit);
    capture(std::string(argv[1]) + "/keypad.ppm");
    screen->action(KeyOk);
    assert(screen->page == Page::Main && screen->settings.frequency_mhz == 2437);
    red_team = true; screen->poll(); screen->action(TxPage);
    assert(screen->page == Page::TxSetup);
    capture(std::string(argv[1]) + "/tx-setup.ppm");
    screen->action(Continue);
    capture(std::string(argv[1]) + "/tx-confirm.ppm");
    screen->action(Start); screen->poll();
    assert(screen->page == Page::TxRunning);
    capture(std::string(argv[1]) + "/tx-running.ppm");
    screen->action(Stop); assert(screen->page == Page::Main);
    const auto previous_closes = closes;
    ui_screen_clear();
    assert(closes == previous_closes + 1);
    dark_mode_enabled = false;
    show_sdr_screen();
    root = lv_obj_get_child(lv_screen_active(), 0);
    screen = static_cast<Screen *>(lv_event_dsc_get_user_data(lv_obj_get_event_dsc(root, 0)));
    for (int i = 0; i < 44; ++i) screen->poll();
    capture(std::string(argv[1]) + "/main-light.ppm");
    ui_screen_clear();
    busy_opens = 2;
    show_sdr_screen();
    root = lv_obj_get_child(lv_screen_active(), 0);
    screen = static_cast<Screen *>(lv_event_dsc_get_user_data(lv_obj_get_event_dsc(root, 0)));
    assert(!screen->session);
    screen->action(SettingsPage);
    screen->poll(); screen->poll();
    assert(screen->session && screen->session->commands.front().operation == sdr::Operation::Pause && screen->session->commands.front().paused);
    ui_screen_clear();
    lv_indev_delete(input);
    lv_display_delete(display);
    lv_deinit();
    std::puts("PASS: real LVGL screen, tap/drag tuning, settings Save/Cancel, keypad, Red Team, TX navigation, delete cleanup");
}
