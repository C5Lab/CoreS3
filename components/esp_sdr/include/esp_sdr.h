// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned frequency_mhz; // 2200..2700: experimental outside nominal ISM band
    unsigned sample_rate_mhz; // 16, 40 or 80
    unsigned bandwidth_mhz; // 0 = widest; otherwise 13..69 (approximate)
    bool hardware_agc;
    unsigned gain_index; // PHY index, NOT dB; clamped to calibrated table
} sdr_config_t;

typedef struct {
    const uint32_t *words; // low bits 9:0 = I, 19:10 = Q, signed IQ10
    unsigned count;
    unsigned capture_us;
    unsigned gain_index;
    unsigned gain_max;
    unsigned clipped; // samples with either component at an ADC rail
} sdr_capture_t;

// Single owner API: call only from the UI/acquisition task, never concurrently.
esp_err_t sdr_init(void);
// Release only the local Wi-Fi/event-loop resources acquired by this backend.
esp_err_t sdr_deinit(void);
esp_err_t sdr_configure(const sdr_config_t *config);
// Snapshot pointer valid until the next capture. Returns after restoring SRAM
// ownership even on timeout. Capture has gaps; no continuous-stream claim.
esp_err_t sdr_capture(unsigned count, sdr_capture_t *result);
unsigned sdr_gain_max(void);
// Switch the shared Wi-Fi PHY from the private snapshot receiver to the
// public ESP-IDF station transmitter. Only channels 1..11 are accepted here;
// the test transmitter deliberately stays in the world-safe channel set.
esp_err_t sdr_begin_wifi_tx_test(unsigned channel);
// Stop the station transmitter and rebuild the SDR receive path. Safe to call
// when a partially-started test needs cleanup.
esp_err_t sdr_end_wifi_tx_test(void);
bool sdr_wifi_tx_test_active(void);
#ifdef __cplusplus
}
#endif
