// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "esp_err.h"

struct TxTestConfig {
    unsigned channel = 1;
    unsigned duration_s = 5;
    unsigned rate_pps = 5;
};

struct TxTestStatus {
    bool active = false;
    unsigned sent = 0;
    unsigned failed = 0;
    unsigned remaining_ms = 0;
};

inline bool tx_test_config_valid(const TxTestConfig &config) {
    bool duration = config.duration_s == 1 || config.duration_s == 5 ||
                    config.duration_s == 10 || config.duration_s == 360;
    bool rate = config.rate_pps == 1 || config.rate_pps == 5 || config.rate_pps == 1000;
    return config.channel >= 1 && config.channel <= 11 && duration && rate;
}
esp_err_t tx_test_start(const TxTestConfig &config);
// Sends at most one frame. A late UI iteration never creates a catch-up burst.
esp_err_t tx_test_poll(TxTestStatus &status);
esp_err_t tx_test_stop(TxTestStatus *status = nullptr);
bool tx_test_active();
