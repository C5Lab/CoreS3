// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
#include "esp_sdr.h"

struct Settings {
    unsigned frequency_mhz = 2412;
    unsigned rate_mhz = 40;
    unsigned fft_size = 1024;
    unsigned bandwidth_mhz = 33;
    bool agc = true;
    unsigned gain = 40;
    unsigned averaging = 4;
    bool remove_dc = true;
    bool peak_hold = false;
    int floor_db = -90;
    unsigned range_db = 80;
    unsigned fps = 15;
    unsigned palette = 0;
    unsigned step_mhz = 1;
    unsigned tx_channel = 1;
    unsigned tx_duration_s = 5;
    unsigned tx_rate_pps = 5;

    void validate();
    sdr_config_t radio() const {
        return {frequency_mhz, rate_mhz, bandwidth_mhz, agc, gain};
    }
};
void load_settings(Settings &settings);
esp_err_t save_settings(const Settings &settings);
