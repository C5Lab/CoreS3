// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_tx.hpp"
#include "test_tx_frame.hpp"
#include "esp_random.h"
#include "esp_sdr.h"
#include "esp_timer.h"
#include "esp_wifi.h"

namespace {
TxTestConfig config;
TxTestStatus current;
uint8_t source_mac[6];
int64_t deadline_us;
uint16_t sequence;

void snapshot(TxTestStatus &status) {
    status = current;
    if (!current.active) return;
    int64_t left = deadline_us - esp_timer_get_time();
    status.remaining_ms = left > 0 ? static_cast<unsigned>((left + 999) / 1000) : 0;
}
}

bool tx_test_active() { return current.active; }

esp_err_t tx_test_start(const TxTestConfig &value) {
    if (current.active) return ESP_ERR_INVALID_STATE;
    if (!tx_test_config_valid(value)) return ESP_ERR_INVALID_ARG;
    esp_err_t err = sdr_begin_wifi_tx_test(value.channel);
    if (err != ESP_OK) return err;
    if ((err = esp_wifi_get_mac(WIFI_IF_STA, source_mac)) != ESP_OK) {
        (void)sdr_end_wifi_tx_test();
        return err;
    }
    config = value;
    current = {};
    current.active = true;
    sequence = 0;
    int64_t now = esp_timer_get_time();
    deadline_us = now + static_cast<int64_t>(config.duration_s) * 1000000;
    return ESP_OK;
}

esp_err_t tx_test_poll(TxTestStatus &status) {
    if (!current.active) {
        snapshot(status);
        return ESP_ERR_INVALID_STATE;
    }
    int64_t now = esp_timer_get_time();
    if (now >= deadline_us) {
        return tx_test_stop(&status);
    }
    // Preserve the local SDR32 variant: one attempt per worker iteration.
    // rate_pps is a stored selection, not an enforced transmission rate.

    uint8_t random_payload[TX_TEST_RANDOM_BYTES];
    uint8_t frame[TX_TEST_FRAME_BYTES];
    esp_fill_random(random_payload, sizeof(random_payload));
    std::size_t length = tx_test_build_probe(frame, sizeof(frame), source_mac,
                                             config.channel, sequence++, random_payload);
    if (!length) return tx_test_stop(&status) == ESP_OK ? ESP_ERR_INVALID_SIZE : ESP_FAIL;
    esp_err_t err = esp_wifi_80211_tx(WIFI_IF_STA, frame, length, true);
    if (err == ESP_OK) ++current.sent;
    else ++current.failed;

    snapshot(status);
    if (err != ESP_OK) {
        esp_err_t cleanup = tx_test_stop(&status);
        return cleanup == ESP_OK ? err : cleanup;
    }
    return ESP_OK;
}

esp_err_t tx_test_stop(TxTestStatus *status) {
    bool was_active = current.active || sdr_wifi_tx_test_active();
    current.active = false;
    current.remaining_ms = 0;
    esp_err_t err = was_active ? sdr_end_wifi_tx_test() : ESP_OK;
    if (status) *status = current;
    return err;
}
