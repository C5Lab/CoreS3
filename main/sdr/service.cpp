// SPDX-License-Identifier: GPL-3.0-or-later
#include "service.hpp"
#include <atomic>
#include <new>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

namespace sdr {
static std::atomic<bool> busy{false};
static constexpr const char *TAG = "sdr";
struct Session {
    QueueHandle_t commands = nullptr;
    QueueHandle_t results = nullptr;
    Settings initial;
    std::atomic<bool> detached{false};
    std::atomic<bool> tx_allowed{false};
};

static void worker(void *argument)
{
    auto *session = static_cast<Session *>(argument);
    Settings settings = session->initial;
    Result report;
    auto *memory = heap_caps_aligned_alloc(16, sizeof(Spectrum), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    auto *spectrum = memory ? new (memory) Spectrum() : nullptr;
    report.error = spectrum ? spectrum->init() : ESP_ERR_NO_MEM;
    if (report.error == ESP_OK && !session->detached.load()) report.error = sdr_init();
    if (report.error == ESP_OK && !session->detached.load()) {
        auto radio = settings.radio();
        report.error = sdr_configure(&radio);
    }
    report.initialized = report.error == ESP_OK;
    report.fatal = !report.initialized;
    if (report.initialized) report.gain_max = sdr_gain_max();
    if (report.fatal) ESP_LOGE(TAG, "Initialization failed: %s", esp_err_to_name(report.error));
    bool paused = false;
    bool dirty = false;
    int64_t save_at = 0, next_frame = 0, next_report = 0;

    auto save = [&] {
        report.save_error = save_settings(settings);
        ++report.save_sequence;
        // Keep failed writes pending, including a final retry on close.
        dirty = report.save_error != ESP_OK;
        save_at = dirty ? esp_timer_get_time() + 2500000 : 0;
        if (dirty) ESP_LOGW(TAG, "Settings save failed: %s", esp_err_to_name(report.save_error));
    };
    auto stop_tx = [&] {
        esp_err_t error = tx_test_stop(&report.tx);
        if (error != ESP_OK) {
            report.error = error;
            report.fatal = true;
        }
        if (spectrum) spectrum->reset();
        next_frame = 0;
    };

    while (!session->detached.load(std::memory_order_acquire)) {
        Command command;
        while (!session->detached.load() && xQueueReceive(session->commands, &command, 0) == pdTRUE) {
            if (command.operation == Operation::Pause) {
                paused = command.paused;
            } else if (command.operation == Operation::StopTx) {
                stop_tx();
            } else {
                settings = command.settings;
                settings.validate();
                report.generation = command.generation;
                dirty = true;
                save_at = esp_timer_get_time() + 2500000;
                if (command.operation == Operation::Configure) {
                    if (!report.fatal && !tx_test_active()) {
                        auto radio = settings.radio();
                        report.error = sdr_configure(&radio);
                        spectrum->reset();
                        next_frame = 0;
                    }
                } else if (command.operation == Operation::ClearPeak) {
                    if (spectrum) spectrum->clear_peak();
                } else if (command.operation == Operation::StartTx) {
                    // Independently enforce the permission published by the UI.
                    if (!session->tx_allowed.load() || report.fatal) {
                        report.error = ESP_ERR_INVALID_STATE;
                    } else {
                        report.error = tx_test_start({settings.tx_channel, settings.tx_duration_s, settings.tx_rate_pps});
                        if (report.error != ESP_OK && sdr_wifi_tx_test_active()) report.fatal = true;
                    }
                }
                if (command.save_now) save();
            }
            next_report = 0;
        }
        if (session->detached.load()) break;
        const int64_t now = esp_timer_get_time();
        if (dirty && now >= save_at) save();
        if (tx_test_active()) {
            if (!session->tx_allowed.load()) stop_tx();
            else {
                report.error = tx_test_poll(report.tx);
                if (!report.tx.active) {
                    if (sdr_wifi_tx_test_active()) report.fatal = true;
                    spectrum->reset();
                    next_frame = 0;
                }
            }
        } else if (!report.fatal && !paused && now >= next_frame) {
            sdr_capture_t capture;
            report.error = sdr_capture(settings.fft_size, &capture);
            if (report.error == ESP_OK) report.error = spectrum->process(capture, settings, report.frame);
            if (report.error == ESP_OK) {
                ++report.frame_sequence;
                report.gain_max = capture.gain_max;
            }
            next_frame = now + (report.error == ESP_OK ? 1000000 / settings.fps : 250000);
        }
        if (now >= next_report) {
            xQueueOverwrite(session->results, &report);
            next_report = now + 20000;
        }
        // Preserve the local TX loop cadence; RF/FFT never run on the LVGL task.
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    // Do not start queued transmissions while closing. Preserve accepted edits.
    Command pending;
    while (xQueueReceive(session->commands, &pending, 0) == pdTRUE) {
        if (pending.operation != Operation::Pause && pending.operation != Operation::StopTx) {
            settings = pending.settings;
            settings.validate();
            dirty = true;
        }
    }
    // Deinit stops the transmitter directly, without rebuilding a receiver
    // that is about to be closed. Reset the TX scheduler on the next session.
    esp_err_t cleanup = sdr_deinit();
    (void)tx_test_stop();
    if (cleanup != ESP_OK) ESP_LOGE(TAG, "RF cleanup failed: %s", esp_err_to_name(cleanup));
    if (dirty) save();
    if (spectrum) {
        spectrum->deinit();
        spectrum->~Spectrum();
        heap_caps_free(memory);
    }
    vQueueDelete(session->commands);
    vQueueDelete(session->results);
    delete session;
    busy.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}

Session *open(const Settings &settings, bool tx_allowed, esp_err_t &error)
{
    error = ESP_ERR_INVALID_STATE;
    bool expected = false;
    if (!busy.compare_exchange_strong(expected, true)) return nullptr;
    auto *session = new (std::nothrow) Session;
    if (session) {
        session->initial = settings;
        session->tx_allowed.store(tx_allowed);
        session->commands = xQueueCreate(16, sizeof(Command));
        session->results = xQueueCreate(1, sizeof(Result));
        if (session->commands && session->results &&
            xTaskCreate(worker, "sdr", 12288, session, 3, nullptr) == pdPASS) {
            error = ESP_OK;
            return session;
        }
        if (session->commands) vQueueDelete(session->commands);
        if (session->results) vQueueDelete(session->results);
        delete session;
    }
    busy.store(false);
    error = ESP_ERR_NO_MEM;
    return nullptr;
}
bool send(Session *session, const Command &command) { return xQueueSend(session->commands, &command, 0) == pdTRUE; }
bool receive(Session *session, Result &result) { return xQueueReceive(session->results, &result, 0) == pdTRUE; }
void allow_tx(Session *session, bool allowed) { session->tx_allowed.store(allowed); }
void close(Session *session) { if (session) session->detached.store(true, std::memory_order_release); }
}
