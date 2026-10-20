// SPDX-License-Identifier: GPL-3.0-or-later
#include "sdr_screen.h"
#include "service.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include "esp_heap_caps.h"
#include "esp_timer.h"
extern "C" {
#include "home_screen.h"
#include "ui_helpers.h"
}

namespace {
constexpr int WIDTH = PLOT_WIDTH, PLOT_HEIGHT = 44, WATER_HEIGHT = 44;
constexpr int WATER_Y = 58, IMAGE_HEIGHT = WATER_Y + WATER_HEIGHT;
enum Action { Minus, Plus, Step, Pause, Peak, SettingsPage, KeypadPage, TxPage,
              Save, Cancel, Defaults, Continue, Start, Stop, KeyOk, KeyErase };
enum class Page { Main, Settings, Keypad, TxSetup, TxConfirm, TxRunning };

template <size_t N> void cycle(unsigned &value, const unsigned (&values)[N], int delta)
{
    unsigned index = 0;
    while (index + 1 < N && values[index] != value) ++index;
    value = values[(index + N + delta) % N];
}
uint16_t rgb(unsigned r, unsigned g, unsigned b)
{
    return ((r & 248) << 8) | ((g & 252) << 3) | (b >> 3);
}
uint16_t color565(lv_color_t color) { return lv_color_to_u16(color); }
uint16_t palette(unsigned value, unsigned mode)
{
    if (mode == 2) return rgb(value, value, value);
    const float t = value / 255.0f;
    if (mode == 1) return rgb(std::min(255, int(t * 510)), std::clamp(int((t - .3f) * 365), 0, 255), std::clamp(int((t - .75f) * 1020), 0, 255));
    if (t < .25f) return rgb(4, 8, 20 + int(t * 480));
    if (t < .55f) return rgb(0, int((t - .25f) * 800), 140 + int((t - .25f) * 380));
    if (t < .8f) return rgb(int((t - .55f) * 1020), 245, 255 - int((t - .55f) * 1020));
    return rgb(255, 245 - int((t - .8f) * 850), 0);
}

class Screen {
public:
    Settings settings, draft;
    sdr::Session *session = nullptr;
    sdr::Result result;
    lv_obj_t *root = nullptr, *body = nullptr, *status = nullptr, *canvas = nullptr;
    lv_obj_t *frequency = nullptr, *tx_button = nullptr, *notice = nullptr;
    lv_timer_t *timer = nullptr;
    uint16_t *pixels = nullptr;
    uint8_t *waterfall = nullptr;
    Page page = Page::Main;
    unsigned tab = 0, rows = 0, head = 0;
    uint32_t generation = 0, seen_frame = 0, seen_save = 0;
    bool paused = false, dragging = false, waiting_tx = false, had_tx = false;
    bool open_failed = false, keypad_error = false;
    lv_point_t gesture_start{};
    char digits[5]{};
    unsigned digit_count = 0;
    int64_t message_until = 0;
    char message[96]{};

    ~Screen()
    {
        if (timer) lv_timer_delete(timer);
        sdr::close(session);
        heap_caps_free(pixels);
        heap_caps_free(waterfall);
    }
    static void action_event(lv_event_t *event)
    {
        auto *self = static_cast<Screen *>(lv_event_get_user_data(event));
        self->action(static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(lv_event_get_target_obj(event)))));
    }
    static void back_event(lv_event_t *event)
    {
        auto *self = static_cast<Screen *>(lv_event_get_user_data(event));
        if (self->page == Page::Main) show_home_screen();
        else {
            if (self->page == Page::TxRunning) self->command(sdr::Operation::StopTx);
            self->change_page(self->page == Page::TxConfirm ? Page::TxSetup : Page::Main);
        }
    }
    static void deleted(lv_event_t *event)
    {
        auto *self = static_cast<Screen *>(lv_event_get_user_data(event));
        if (lv_event_get_target_obj(event) != self->root) return;
        self->~Screen();
        heap_caps_free(self);
    }
    lv_obj_t *label(const char *text, int x, int y, int width, const lv_font_t *font = &lv_font_montserrat_12)
    {
        auto *object = lv_label_create(body);
        lv_label_set_text(object, text);
        lv_obj_set_pos(object, x, y);
        lv_obj_set_width(object, width);
        lv_obj_set_style_text_font(object, font, 0);
        lv_obj_set_style_text_color(object, ui_text_color(), 0);
        return object;
    }
    lv_obj_t *button(const char *text, int x, int y, int width, int height, int action, bool selected = false)
    {
        auto *object = lv_button_create(body);
        lv_obj_set_pos(object, x, y);
        lv_obj_set_size(object, width, height);
        style_neutral_button(object);
        lv_obj_set_style_pad_all(object, 0, 0);
        if (selected) lv_obj_set_style_border_color(object, UI_ACCENT_CYAN, 0);
        lv_obj_set_user_data(object, reinterpret_cast<void *>(static_cast<intptr_t>(action)));
        lv_obj_add_event_cb(object, action_event, LV_EVENT_CLICKED, this);
        auto *text_object = lv_label_create(object);
        lv_label_set_text(text_object, text);
        lv_obj_set_style_text_font(text_object, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(text_object, ui_text_color(), 0);
        lv_obj_center(text_object);
        return object;
    }
    void notify(const char *text)
    {
        std::snprintf(message, sizeof(message), "%s", text);
        message_until = esp_timer_get_time() + 2500000;
        if (page == Page::Main) update_status();
        if (notice) lv_label_set_text(notice, message);
    }
    bool command(sdr::Operation operation, bool save_now = false, bool suspend = false)
    {
        if (!session) { notify("SDR is not ready"); return false; }
        sdr::Command value;
        value.operation = operation;
        value.settings = settings;
        value.generation = generation;
        value.save_now = save_now;
        value.paused = suspend;
        if (sdr::send(session, value)) return true;
        notify("SDR busy - try again");
        return false;
    }
    bool apply(const Settings &value, sdr::Operation operation, bool save_now = false)
    {
        const Settings previous = settings;
        settings = value;
        settings.validate();
        ++generation;
        if (command(operation, save_now)) {
            if (operation != sdr::Operation::StartTx) reset_history();
            return true;
        }
        --generation;
        settings = previous;
        return false;
    }
    void reset_history()
    {
        rows = head = 0;
        std::fill_n(result.frame.db, WIDTH, -120.0f);
        std::fill_n(result.frame.peak, WIDTH, -120.0f);
        if (canvas) draw(false);
    }
    void change_page(Page next)
    {
        // Enqueue the pause before exposing controls that can start a TX test.
        if (session && !command(sdr::Operation::Pause, false, next != Page::Main || paused)) return;
        page = next;
        build();
    }
    void init()
    {
        load_settings(settings);
        pixels = static_cast<uint16_t *>(heap_caps_malloc(WIDTH * IMAGE_HEIGHT * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        waterfall = static_cast<uint8_t *>(heap_caps_calloc(WIDTH, WATER_HEIGHT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        auto *screen = ui_screen_clear();
        root = lv_obj_create(screen);
        lv_obj_remove_style_all(root);
        lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
        lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(root, deleted, LV_EVENT_DELETE, this);
        ui_create_top_bar(root, "SDR", back_event, this);
        body = lv_obj_create(root);
        lv_obj_remove_style_all(body);
        lv_obj_set_pos(body, 0, 36);
        lv_obj_set_size(body, 320, 204);
        lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
        reset_history();
        if (!pixels || !waterfall) {
            open_failed = true;
            label("SDR buffer allocation failed.\nCheck PSRAM; use Back to exit.", 8, 20, 304);
            return;
        }
        build();
        timer = lv_timer_create([](lv_timer_t *timer) {
            static_cast<Screen *>(lv_timer_get_user_data(timer))->poll();
        }, 30, this);
        poll();
    }
    void update_status()
    {
        if (!status) return;
        char text[128];
        if (esp_timer_get_time() < message_until) std::snprintf(text, sizeof(text), "%s", message);
        else if (!session) std::snprintf(text, sizeof(text), "%s", open_failed ? "SDR unavailable - Back to retry" : "Waiting for previous SDR session to stop...");
        else if (result.fatal) std::snprintf(text, sizeof(text), "RF ERROR: %s - Back to reopen", esp_err_to_name(result.error));
        else if (!result.initialized) std::snprintf(text, sizeof(text), "Initializing local SDR receiver...");
        else if (result.error != ESP_OK) std::snprintf(text, sizeof(text), "RF ERROR: %s", esp_err_to_name(result.error));
        else std::snprintf(text, sizeof(text), "%s %uM/%u FFT %s G%u  %.0f dBFS%s%s",
            paused ? "PAUSE" : "RX", settings.rate_mhz, settings.fft_size,
            settings.agc ? "AGC" : "MAN", result.frame.gain, result.frame.strongest_db,
            result.frame.clipped ? " CLIP" : "", settings.frequency_mhz < 2412 || settings.frequency_mhz > 2484 ? " EXP" : "");
        lv_label_set_text(status, text);
    }
    void poll()
    {
        if (!session && !open_failed) {
            esp_err_t error;
            session = sdr::open(settings, ui_red_team_enabled(), error);
            // A subpage may have opened while the previous session was closing.
            if (session) command(sdr::Operation::Pause, false, page != Page::Main || paused);
            if (!session && error != ESP_ERR_INVALID_STATE) {
                open_failed = true;
                notify("Cannot start SDR worker - Back to retry");
            }
        }
        if (session) {
            sdr::allow_tx(session, ui_red_team_enabled());
            if (sdr::receive(session, result)) {
                if (result.save_sequence != seen_save) {
                    seen_save = result.save_sequence;
                    notify(result.save_error == ESP_OK ? "Settings saved" : "Save failed - settings in RAM");
                }
                if (result.tx.active) { had_tx = true; waiting_tx = false; }
                else if (page == Page::TxRunning && (had_tx || result.error != ESP_OK || result.generation == generation)) {
                    waiting_tx = false;
                    had_tx = false;
                    change_page(Page::Main);
                    reset_history();
                    notify(result.error == ESP_OK ? "TX stopped; SDR restored" : "TX failed; check RF status");
                }
                if (page == Page::Main && result.generation == generation && result.frame_sequence != seen_frame) {
                    seen_frame = result.frame_sequence;
                    draw(true);
                }
                if (page == Page::TxRunning && status) {
                    char text[128];
                    std::snprintf(text, sizeof(text), "%s\nSent: %u   Failed: %u\nRemaining: %.1f s",
                        waiting_tx ? "Starting TX..." : "TRANSMITTING", result.tx.sent, result.tx.failed, result.tx.remaining_ms / 1000.0f);
                    lv_label_set_text(status, text);
                }
            }
        }
        if (tx_button) {
            if (!ui_red_team_enabled() || !result.initialized || result.fatal) lv_obj_add_state(tx_button, LV_STATE_DISABLED);
            else lv_obj_remove_state(tx_button, LV_STATE_DISABLED);
        }
        if (page == Page::Main) update_status();
        if (notice && esp_timer_get_time() >= message_until) lv_label_set_text(notice, "");
    }
    void line(int x0, int y0, int x1, int y1, uint16_t color)
    {
        const int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
        const int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
        int error = dx + dy;
        for (;;) {
            if (x0 >= 0 && x0 < WIDTH && y0 >= 0 && y0 < PLOT_HEIGHT) pixels[y0 * WIDTH + x0] = color;
            if (x0 == x1 && y0 == y1) break;
            int twice = 2 * error;
            if (twice >= dy) { error += dy; x0 += sx; }
            if (twice <= dx) { error += dx; y0 += sy; }
        }
    }
    int level(float db, int height) const
    {
        return std::clamp(int((db - settings.floor_db) * (height - 1) / settings.range_db), 0, height - 1);
    }
    void draw(bool add_waterfall)
    {
        if (!canvas || !pixels || !waterfall) return;
        std::fill_n(pixels, WIDTH * IMAGE_HEIGHT, color565(ui_bg_color()));
        if (add_waterfall) {
            head = (head + WATER_HEIGHT - 1) % WATER_HEIGHT;
            for (int x = 0; x < WIDTH; ++x) waterfall[head * WIDTH + x] = level(result.frame.db[x], 256);
            rows = std::min(rows + 1, unsigned(WATER_HEIGHT));
        }
        for (int j = 0; j < 3; ++j) line(0, j * (PLOT_HEIGHT - 1) / 2, WIDTH - 1, j * (PLOT_HEIGHT - 1) / 2, color565(ui_border_color()));
        for (int j = 0; j <= 4; ++j) line(j * (WIDTH - 1) / 4, 0, j * (WIDTH - 1) / 4, PLOT_HEIGHT - 1, color565(ui_border_color()));
        for (int x = 1; x < WIDTH; ++x) {
            line(x - 1, PLOT_HEIGHT - 1 - level(result.frame.db[x - 1], PLOT_HEIGHT), x, PLOT_HEIGHT - 1 - level(result.frame.db[x], PLOT_HEIGHT), color565(UI_ACCENT_CYAN));
            if (settings.peak_hold) line(x - 1, PLOT_HEIGHT - 1 - level(result.frame.peak[x - 1], PLOT_HEIGHT), x, PLOT_HEIGHT - 1 - level(result.frame.peak[x], PLOT_HEIGHT), color565(UI_ACCENT_ORANGE));
        }
        for (unsigned y = 0; y < rows; ++y)
            for (int x = 0; x < WIDTH; ++x)
                pixels[(WATER_Y + y) * WIDTH + x] = palette(waterfall[((head + y) % WATER_HEIGHT) * WIDTH + x], settings.palette);
        lv_obj_invalidate(canvas);
    }
    static void gesture(lv_event_t *event)
    {
        auto *self = static_cast<Screen *>(lv_event_get_user_data(event));
        auto *input = lv_indev_active();
        if (!input) return;
        lv_point_t point;
        lv_indev_get_point(input, &point);
        if (lv_event_get_code(event) == LV_EVENT_PRESSED) {
            self->gesture_start = point;
            self->dragging = true;
        } else if (lv_event_get_code(event) == LV_EVENT_PRESS_LOST) self->dragging = false;
        else if (lv_event_get_code(event) == LV_EVENT_RELEASED && self->dragging) {
            self->dragging = false;
            lv_area_t bounds;
            lv_obj_get_coords(self->canvas, &bounds);
            const int dx = point.x - self->gesture_start.x;
            if (std::abs(dx) > 12) self->tune(int(self->settings.frequency_mhz) - int(std::lround(dx * float(self->settings.rate_mhz) / WIDTH)));
            else self->tune(int(std::lround(self->settings.frequency_mhz + ((point.x - bounds.x1) / float(WIDTH - 1) - .5f) * self->settings.rate_mhz)));
        }
    }
    void tune(int mhz)
    {
        Settings value = settings;
        value.frequency_mhz = std::clamp(mhz, 2200, 2700);
        if (apply(value, sdr::Operation::Configure)) build();
    }
    void build()
    {
        lv_obj_clean(body);
        status = canvas = frequency = tx_button = notice = nullptr;
        if (page == Page::Main) build_main();
        else if (page == Page::Settings) build_settings();
        else if (page == Page::Keypad) build_keypad();
        else build_tx();
        if (page == Page::Settings || page == Page::TxSetup || page == Page::TxConfirm)
            notice = label(esp_timer_get_time() < message_until ? message : "", 4, 158, 312, &lv_font_montserrat_10);
    }
    void build_main()
    {
        char text[48];
        std::snprintf(text, sizeof(text), "%u MHz", settings.frequency_mhz);
        frequency = button(text, 4, 2, 148, 30, KeypadPage);
        button("Set", 210, 2, 50, 30, SettingsPage);
        tx_button = button("TX", 264, 2, 52, 30, TxPage);
        if (!ui_red_team_enabled() || !result.initialized || result.fatal) lv_obj_add_state(tx_button, LV_STATE_DISABLED);
        status = label("", 4, 35, 312, &lv_font_montserrat_10);
        canvas = lv_canvas_create(body);
        lv_obj_set_pos(canvas, 28, 52);
        lv_canvas_set_buffer(canvas, pixels, WIDTH, IMAGE_HEIGHT, LV_COLOR_FORMAT_RGB565);
        lv_obj_add_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);
        for (auto code : {LV_EVENT_PRESSED, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST}) lv_obj_add_event_cb(canvas, gesture, code, this);
        for (int j = 0; j < 3; ++j) {
            std::snprintf(text, sizeof(text), "%d", settings.floor_db + int(settings.range_db) * (2 - j) / 2);
            label(text, 0, 50 + j * (PLOT_HEIGHT - 1) / 2, 28, &lv_font_montserrat_10);
            std::snprintf(text, sizeof(text), "%.0f", settings.frequency_mhz + (j - 1) * float(settings.rate_mhz) * .5f);
            label(text, 28 + j * 122, 96, 42, &lv_font_montserrat_10);
        }
        button("-", 4, 168, 36, 32, Minus);
        std::snprintf(text, sizeof(text), "%u MHz", settings.step_mhz);
        button(text, 44, 168, 62, 32, Step);
        button("+", 110, 168, 36, 32, Plus);
        button(paused ? "Run" : "Pause", 150, 168, 80, 32, Pause, paused);
        button("Peak", 234, 168, 82, 32, Peak, settings.peak_hold);
        draw(false);
        update_status();
    }
    void build_settings()
    {
        const char *tabs[] = {"RADIO", "FFT", "VIEW", "MORE"};
        for (unsigned i = 0; i < 4; ++i) button(tabs[i], 4 + i * 79, 0, 75, 26, 200 + i, tab == i);
        const char *names[] = {"Sample rate", "RF filter", "Hardware AGC", "Gain index", "FFT bins", "Average", "Remove DC", "Peak hold", "Floor dBFS", "Range dB", "Palette", "", "Refresh / sec", "Tune step MHz", "", ""};
        for (unsigned row = 0; row < 4; ++row) {
            const unsigned index = tab * 4 + row;
            const int y = 29 + row * 33;
            if (index == 11 || index >= 14) continue;
            char text[24];
            switch (index) {
            case 0: std::snprintf(text, sizeof(text), "%u MS/s", draft.rate_mhz); break;
            case 1:
                if (draft.bandwidth_mhz) std::snprintf(text, sizeof(text), "%u MHz", draft.bandwidth_mhz);
                else std::snprintf(text, sizeof(text), "Wide");
                break;
            case 2: std::snprintf(text, sizeof(text), "%s", draft.agc ? "On" : "Off"); break;
            case 3: std::snprintf(text, sizeof(text), "%u", draft.gain); break;
            case 4: std::snprintf(text, sizeof(text), "%u", draft.fft_size); break;
            case 5: std::snprintf(text, sizeof(text), "%ux", draft.averaging); break;
            case 6: std::snprintf(text, sizeof(text), "%s", draft.remove_dc ? "On" : "Off"); break;
            case 7: std::snprintf(text, sizeof(text), "%s", draft.peak_hold ? "On" : "Off"); break;
            case 8: std::snprintf(text, sizeof(text), "%d", draft.floor_db); break;
            case 9: std::snprintf(text, sizeof(text), "%u", draft.range_db); break;
            case 10: std::snprintf(text, sizeof(text), "%s", draft.palette == 0 ? "Ocean" : draft.palette == 1 ? "Fire" : "Gray"); break;
            case 12: std::snprintf(text, sizeof(text), "%u", draft.fps); break;
            default: std::snprintf(text, sizeof(text), "%u", draft.step_mhz); break;
            }
            label(names[index], 6, y + 8, 130);
            button("-", 138, y, 32, 30, 100 + row * 2);
            auto *value = label(text, 174, y + 8, 102);
            lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_CENTER, 0);
            button("+", 280, y, 34, 30, 101 + row * 2);
        }
        if (tab == 3) label("2200-2700 MHz (experimental)\nRelative dBFS, not dBm.\nDisplay settings follow CoreS3.", 6, 103, 308, &lv_font_montserrat_10);
        button("Cancel", 4, 170, 98, 30, Cancel);
        button("Defaults", 106, 170, 104, 30, Defaults);
        button("Save", 214, 170, 102, 30, Save);
    }
    void build_keypad()
    {
        char text[64];
        std::snprintf(text, sizeof(text), "Frequency MHz: %s", digits);
        label(text, 6, 0, 308, &lv_font_montserrat_14);
        if (keypad_error) label("Enter 4 digits: 2200-2700", 6, 16, 308, &lv_font_montserrat_10);
        for (int i = 0; i < 9; ++i) {
            std::snprintf(text, sizeof(text), "%d", i + 1);
            button(text, 4 + (i % 3) * 105, 27 + (i / 3) * 35, 101, 32, 301 + i);
        }
        button("Erase", 4, 132, 101, 32, KeyErase);
        button("0", 109, 132, 101, 32, 300);
        button("Cancel", 214, 132, 102, 32, Cancel);
        button("OK (2200-2700 MHz)", 4, 170, 312, 30, KeyOk);
    }
    void build_tx()
    {
        char text[256];
        if (page == Page::TxSetup) {
            label("TX: local SDR32 variant", 6, 0, 308);
            const char *names[] = {"Wi-Fi channel", "Duration (s)", "Rate selection"};
            for (unsigned row = 0; row < 3; ++row) {
                int y = 21 + row * 34;
                unsigned value = row == 0 ? draft.tx_channel : row == 1 ? draft.tx_duration_s : draft.tx_rate_pps;
                label(names[row], 6, y + 8, 132);
                button("-", 140, y, 32, 30, 400 + row * 2);
                std::snprintf(text, sizeof(text), "%u", value);
                auto *number = label(text, 178, y + 8, 94);
                lv_obj_set_style_text_align(number, LV_TEXT_ALIGN_CENTER, 0);
                button("+", 280, y, 34, 30, 401 + row * 2);
            }
            label("Rate is not enforced; no frame-count cap.\nTX power: device maximum.", 6, 128, 308, &lv_font_montserrat_10);
            button("Cancel", 4, 168, 152, 32, Cancel);
            button("Continue", 160, 168, 156, 32, Continue);
        } else if (page == Page::TxConfirm) {
            label("Confirm TX", 6, 2, 308, &lv_font_montserrat_16);
            std::snprintf(text, sizeof(text), "Channel %u / %u seconds\nStored rate selection: %u\nActual rate varies with processing.\nNo maximum frame count.\nSDR reception pauses during TX.", draft.tx_channel, draft.tx_duration_s, draft.tx_rate_pps);
            label(text, 6, 30, 308);
            button("Cancel", 4, 168, 152, 32, Cancel);
            button("Start", 160, 168, 156, 32, Start);
        } else {
            std::snprintf(text, sizeof(text), "TX / channel %u", settings.tx_channel);
            label(text, 6, 2, 308, &lv_font_montserrat_16);
            status = label("Starting TX...", 6, 36, 308, &lv_font_montserrat_16);
            label("Probe requests / random vendor payload\nRate is not enforced; no frame-count cap.", 6, 115, 308, &lv_font_montserrat_10);
            button("Stop TX", 4, 168, 312, 32, Stop);
        }
    }
    void edit_setting(unsigned row, int delta)
    {
        static constexpr unsigned rates[] = {16, 40, 80}, sizes[] = {256, 512, 1024, 2048};
        static constexpr unsigned filters[] = {0, 13, 16, 21, 25, 33, 45, 51, 69};
        static constexpr unsigned averages[] = {1, 2, 4, 8}, refresh[] = {5, 10, 15, 20}, steps[] = {1, 5, 10};
        switch (tab * 4 + row) {
        case 0: cycle(draft.rate_mhz, rates, delta); break;
        case 1: cycle(draft.bandwidth_mhz, filters, delta); break;
        case 2: draft.agc = !draft.agc; break;
        case 3: draft.gain = std::clamp(int(draft.gain) + delta, 0, int(result.gain_max)); break;
        case 4: cycle(draft.fft_size, sizes, delta); break;
        case 5: cycle(draft.averaging, averages, delta); break;
        case 6: draft.remove_dc = !draft.remove_dc; break;
        case 7: draft.peak_hold = !draft.peak_hold; break;
        case 8: draft.floor_db = std::clamp(draft.floor_db + delta * 5, -120, -20); break;
        case 9: draft.range_db = std::clamp(int(draft.range_db) + delta * 5, 30, 100); break;
        case 10: draft.palette = (draft.palette + 3 + delta) % 3; break;
        case 12: cycle(draft.fps, refresh, delta); break;
        case 13: cycle(draft.step_mhz, steps, delta); break;
        }
        build();
    }
    void action(int action)
    {
        if (action >= 400 && action < 406) {
            static constexpr unsigned durations[] = {1, 5, 10, 360}, rates[] = {1, 5, 1000};
            int delta = action % 2 ? 1 : -1;
            switch ((action - 400) / 2) {
            case 0: draft.tx_channel = (draft.tx_channel + 10 + delta) % 11 + 1; break;
            case 1: cycle(draft.tx_duration_s, durations, delta); break;
            case 2: cycle(draft.tx_rate_pps, rates, delta); break;
            }
            build();
        } else if (action >= 300 && action <= 309) {
            if (digit_count < 4) { keypad_error = false; digits[digit_count++] = '0' + action - 300; digits[digit_count] = 0; build(); }
        } else if (action >= 200 && action < 204) { tab = action - 200; build(); }
        else if (action >= 100 && action < 108) edit_setting((action - 100) / 2, action % 2 ? 1 : -1);
        else switch (action) {
        case Minus: tune(int(settings.frequency_mhz) - settings.step_mhz); break;
        case Plus: tune(settings.frequency_mhz + settings.step_mhz); break;
        case Step: {
            static constexpr unsigned steps[] = {1, 5, 10};
            Settings value = settings;
            cycle(value.step_mhz, steps, 1);
            if (apply(value, sdr::Operation::Configure)) build();
            break;
        }
        case Pause:
            if (command(sdr::Operation::Pause, false, !paused)) { paused = !paused; build(); }
            break;
        case Peak: {
            Settings value = settings;
            value.peak_hold = !value.peak_hold;
            if (apply(value, sdr::Operation::ClearPeak)) build();
            break;
        }
        case SettingsPage: draft = settings; tab = 0; change_page(Page::Settings); break;
        case KeypadPage: digit_count = 0; digits[0] = 0; keypad_error = false; change_page(Page::Keypad); break;
        case TxPage:
            if (ui_red_team_enabled() && result.initialized && !result.fatal) { draft = settings; change_page(Page::TxSetup); }
            break;
        case Save: if (apply(draft, sdr::Operation::Configure, true)) change_page(Page::Main); break;
        case Defaults: draft = Settings{}; build(); break;
        case Cancel: change_page(page == Page::TxConfirm ? Page::TxSetup : Page::Main); break;
        case Continue: change_page(Page::TxConfirm); break;
        case Start:
            if (!ui_red_team_enabled()) { notify("TX requires Red Team in CoreS3 Settings"); break; }
            sdr::allow_tx(session, ui_red_team_enabled());
            if (apply(draft, sdr::Operation::StartTx, true)) {
                waiting_tx = true;
                had_tx = false;
                change_page(Page::TxRunning);
            }
            break;
        case Stop:
            if (command(sdr::Operation::StopTx)) { waiting_tx = had_tx = false; change_page(Page::Main); reset_history(); }
            break;
        case KeyErase: if (digit_count) digits[--digit_count] = 0; keypad_error = false; build(); break;
        case KeyOk: {
            int value = std::atoi(digits);
            if (digit_count == 4 && value >= 2200 && value <= 2700) {
                Settings next = settings;
                next.frequency_mhz = value;
                if (apply(next, sdr::Operation::Configure)) change_page(Page::Main);
            } else {
                keypad_error = true;
                build();
            }
            break;
        }
        }
    }
};
void allocation_back(lv_event_t *) { show_home_screen(); }
}

extern "C" void show_sdr_screen(void)
{
    void *memory = heap_caps_malloc(sizeof(Screen), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!memory) {
        auto *screen = ui_screen_clear();
        ui_create_top_bar(screen, "SDR: no memory", allocation_back, nullptr);
        return;
    }
    auto *screen = new (memory) Screen;
    screen->init();
}
