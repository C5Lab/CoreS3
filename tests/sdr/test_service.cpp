// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the production worker and TX scheduler with a host queue/task shim.
#include "service.hpp"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <cstdio>

using namespace std::chrono_literals;
struct HostQueue {
    unsigned length, bytes;
    std::mutex mutex;
    std::deque<std::vector<char>> items;
};
static std::vector<std::thread> tasks;
static std::atomic<unsigned> live_queues{0}, live_allocations{0}, captures{0}, transmissions{0}, starts{0}, cleanups{0}, saves{0};
static std::atomic<bool> fail_init{false}, fail_memory{false}, fail_restore{false}, block_init{false}, in_init{false};
static std::atomic<bool> rf_ready{false}, tx_active{false};
static std::map<std::string, int32_t> flash;

QueueHandle_t xQueueCreate(unsigned length, unsigned bytes) {
    auto *queue = new HostQueue;
    queue->length = length; queue->bytes = bytes; ++live_queues;
    return queue;
}
BaseType_t xQueueSend(QueueHandle_t queue, const void *data, TickType_t) {
    std::lock_guard<std::mutex> lock(queue->mutex);
    if (queue->items.size() == queue->length) return 0;
    queue->items.emplace_back(static_cast<const char *>(data), static_cast<const char *>(data) + queue->bytes);
    return pdTRUE;
}
BaseType_t xQueueOverwrite(QueueHandle_t queue, const void *data) {
    std::lock_guard<std::mutex> lock(queue->mutex);
    queue->items.clear();
    queue->items.emplace_back(static_cast<const char *>(data), static_cast<const char *>(data) + queue->bytes);
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t queue, void *data, TickType_t) {
    std::lock_guard<std::mutex> lock(queue->mutex);
    if (queue->items.empty()) return 0;
    std::memcpy(data, queue->items.front().data(), queue->bytes);
    queue->items.pop_front();
    return pdTRUE;
}
void vQueueDelete(QueueHandle_t queue) { delete queue; --live_queues; }
BaseType_t xTaskCreate(void (*function)(void *), const char *, unsigned, void *argument, unsigned, void *) {
    tasks.emplace_back(function, argument);
    return pdPASS;
}
void vTaskDelay(TickType_t ticks) { std::this_thread::sleep_for(std::chrono::milliseconds(ticks)); }
void vTaskDelete(void *) {}
void *heap_caps_aligned_alloc(std::size_t alignment, std::size_t bytes, unsigned) {
    if (fail_memory.load()) return nullptr;
    void *pointer = nullptr;
    if (posix_memalign(&pointer, alignment, bytes)) return nullptr;
    ++live_allocations;
    return pointer;
}
void heap_caps_free(void *pointer) { if (pointer) { --live_allocations; std::free(pointer); } }
int64_t esp_timer_get_time() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void esp_fill_random(void *data, std::size_t bytes) { std::memset(data, 0x53, bytes); }
esp_err_t esp_wifi_get_mac(int, uint8_t *mac) { std::memset(mac, 0x18, 6); return ESP_OK; }
esp_err_t esp_wifi_80211_tx(int, const void *data, int bytes, bool) {
    assert(rf_ready && tx_active);
    assert(bytes == 95 && *static_cast<const uint8_t *>(data) == 0x40);
    ++transmissions;
    return ESP_OK;
}
esp_err_t sdr_init() {
    in_init = true;
    while (block_init) std::this_thread::sleep_for(1ms);
    in_init = false;
    if (fail_init) return ESP_FAIL;
    rf_ready = true;
    return ESP_OK;
}
esp_err_t sdr_deinit() { rf_ready = tx_active = false; ++cleanups; return ESP_OK; }
esp_err_t sdr_configure(const sdr_config_t *) { assert(rf_ready && !tx_active); return ESP_OK; }
esp_err_t sdr_capture(unsigned count, sdr_capture_t *capture) {
    assert(rf_ready && !tx_active);
    static uint32_t words[2048]{};
    ++captures;
    *capture = {words, count, 100, 40, 82, 0};
    return ESP_OK;
}
unsigned sdr_gain_max() { return 82; }
bool sdr_wifi_tx_test_active() { return tx_active; }
esp_err_t sdr_begin_wifi_tx_test(unsigned channel) {
    assert(rf_ready && !tx_active && channel >= 1 && channel <= 11);
    tx_active = true; ++starts;
    return ESP_OK;
}
esp_err_t sdr_end_wifi_tx_test() {
    if (!rf_ready) return ESP_ERR_INVALID_STATE;
    if (fail_restore) return ESP_FAIL;
    tx_active = false;
    return ESP_OK;
}
esp_err_t nvs_open(const char *name, int, nvs_handle_t *handle) { assert(std::strcmp(name, "sdr32") == 0); *handle = 1; return ESP_OK; }
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
esp_err_t nvs_commit(nvs_handle_t) { ++saves; return ESP_OK; }

static void until(const std::function<bool()> &condition) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!condition()) { assert(std::chrono::steady_clock::now() < deadline); std::this_thread::sleep_for(2ms); }
}
static sdr::Result wait_result(sdr::Session *session, const std::function<bool(const sdr::Result &)> &condition) {
    sdr::Result result;
    until([&] { return sdr::receive(session, result) && condition(result); });
    return result;
}
static sdr::Session *open(bool allowed = true) {
    esp_err_t error;
    auto *session = sdr::open(Settings{}, allowed, error);
    assert(session && error == ESP_OK);
    return session;
}
static void close_and_join(sdr::Session *session) {
    sdr::close(session);
    for (auto &task : tasks) task.join();
    tasks.clear();
    assert(!rf_ready && !tx_active && live_queues == 0 && live_allocations == 0);
}
int main() {
    auto *session = open(false);
    wait_result(session, [](auto &r) { return r.frame_sequence > 0; });
    sdr::Command command;
    command.operation = sdr::Operation::Pause; command.paused = true;
    assert(sdr::send(session, command));
    std::this_thread::sleep_for(30ms);
    unsigned before = captures;
    std::this_thread::sleep_for(100ms);
    assert(captures == before);
    command.operation = sdr::Operation::StartTx; command.generation = 1;
    command.settings.tx_duration_s = 360; command.settings.tx_rate_pps = 1000;
    assert(sdr::send(session, command));
    wait_result(session, [](auto &r) { return r.generation == 1 && r.error == ESP_ERR_INVALID_STATE; });
    assert(starts == 0 && transmissions == 0);
    sdr::allow_tx(session, true);
    command.generation = 2;
    assert(sdr::send(session, command));
    wait_result(session, [](auto &r) { return r.tx.active; });
    assert(starts == 1);
    before = captures;
    std::this_thread::sleep_for(80ms);
    assert(captures == before && transmissions > 1);
    sdr::allow_tx(session, false);
    wait_result(session, [](auto &r) { return !r.tx.active; });
    assert(!tx_active);
    sdr::allow_tx(session, true);
    command.generation = 3;
    assert(sdr::send(session, command));
    wait_result(session, [](auto &r) { return r.tx.active; });
    close_and_join(session); // Close during TX also flushes pending settings.
    assert(saves > 0 && flash["txtime"] == 360 && flash["txrate"] == 1000);

    session = open(); // Reopen with a clean FFT table and radio state.
    wait_result(session, [](auto &r) { return r.frame_sequence > 0; });
    command.operation = sdr::Operation::Configure;
    command.settings.frequency_mhz = 2650; command.generation = 1;
    assert(sdr::send(session, command));
    close_and_join(session); // A queued final edit must not be lost.
    assert(flash["freq"] == 2650);

    session = open();
    wait_result(session, [](auto &r) { return r.initialized; });
    command.operation = sdr::Operation::StartTx;
    command.settings.tx_duration_s = 1;
    command.settings.tx_rate_pps = 1;
    command.generation = 1;
    assert(sdr::send(session, command));
    wait_result(session, [](auto &r) { return r.tx.active; });
    auto completed = wait_result(session, [](auto &r) { return !r.tx.active && r.tx.sent > 0; });
    assert(completed.tx.sent > 1); // The stored rate does not cap frame count.
    close_and_join(session);

    fail_init = true;
    session = open();
    wait_result(session, [](auto &r) { return r.fatal && !r.initialized; });
    close_and_join(session);
    fail_init = false;
    fail_memory = true;
    session = open();
    wait_result(session, [](auto &r) { return r.fatal && r.error == ESP_ERR_NO_MEM; });
    close_and_join(session);
    fail_memory = false;

    block_init = true;
    session = open();
    until([] { return in_init.load(); });
    sdr::close(session);
    esp_err_t error;
    assert(!sdr::open(Settings{}, true, error) && error == ESP_ERR_INVALID_STATE);
    block_init = false;
    for (auto &task : tasks) task.join();
    tasks.clear();
    assert(live_queues == 0 && live_allocations == 0 && !rf_ready);

    session = open();
    wait_result(session, [](auto &r) { return r.initialized; });
    command.operation = sdr::Operation::StartTx;
    assert(sdr::send(session, command));
    wait_result(session, [](auto &r) { return r.tx.active; });
    fail_restore = true;
    command.operation = sdr::Operation::StopTx;
    assert(sdr::send(session, command));
    wait_result(session, [](auto &r) { return r.fatal; });
    before = captures;
    std::this_thread::sleep_for(100ms);
    assert(captures == before); // Never resume RX with an uncertain PHY state.
    close_and_join(session);
    std::puts("PASS: worker RX/pause, Red Team gate, TX/stop, exit flush, reopen, init/allocation failures, fatal PHY recovery");
}
