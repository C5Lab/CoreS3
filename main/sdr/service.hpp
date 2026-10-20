// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "settings.hpp"
#include "spectrum.hpp"
#include "test_tx.hpp"

namespace sdr {
enum class Operation { Configure, Pause, ClearPeak, StartTx, StopTx };
struct Command {
    Operation operation = Operation::Configure;
    Settings settings;
    uint32_t generation = 0;
    bool paused = false;
    bool save_now = false;
};
struct Result {
    SpectrumFrame frame;
    TxTestStatus tx;
    uint32_t generation = 0;
    uint32_t frame_sequence = 0;
    uint32_t save_sequence = 0;
    esp_err_t error = ESP_OK;
    esp_err_t save_error = ESP_OK;
    unsigned gain_max = 0;
    bool initialized = false;
    bool fatal = false;
};
struct Session;
// ESP_ERR_INVALID_STATE means a previous session is still shutting down.
Session *open(const Settings &settings, bool tx_allowed, esp_err_t &error);
bool send(Session *session, const Command &command);
bool receive(Session *session, Result &result);
void allow_tx(Session *session, bool allowed);
// Detaches immediately. The owner task flushes settings and frees its resources.
// No session pointer may be used after this call.
void close(Session *session);
}
