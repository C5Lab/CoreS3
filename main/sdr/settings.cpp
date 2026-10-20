// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings.hpp"
#include "nvs.h"

void Settings::validate() {
    frequency_mhz = std::clamp(frequency_mhz, 2200u, 2700u);
    if (rate_mhz != 16 && rate_mhz != 40 && rate_mhz != 80) rate_mhz = 40;
    if (fft_size != 256 && fft_size != 512 && fft_size != 1024 && fft_size != 2048) fft_size = 1024;
    if (bandwidth_mhz && (bandwidth_mhz < 13 || bandwidth_mhz > 69)) bandwidth_mhz = 0;
    gain = std::min(gain, 82u);
    if (averaging != 1 && averaging != 2 && averaging != 4 && averaging != 8) averaging = 4;
    floor_db = std::clamp(floor_db, -120, -20);
    range_db = std::clamp(range_db, 30u, 100u);
    if (fps != 5 && fps != 10 && fps != 15 && fps != 20) fps = 15;
    palette = std::min(palette, 2u);
    if (step_mhz != 1 && step_mhz != 5 && step_mhz != 10) step_mhz = 1;
    tx_channel = std::clamp(tx_channel, 1u, 11u);
    if (tx_duration_s != 1 && tx_duration_s != 5 && tx_duration_s != 10 && tx_duration_s != 360) tx_duration_s = 5;
    if (tx_rate_pps != 1 && tx_rate_pps != 5 && tx_rate_pps != 1000) tx_rate_pps = 5;
}
// Explicit keys keep padding and future struct changes out of flash storage.
#define SETTINGS_KEYS(X) \
 X(frequency_mhz, "freq") X(rate_mhz, "rate") X(fft_size, "fft") \
 X(bandwidth_mhz, "bw") X(agc, "agc") X(gain, "gain") \
 X(averaging, "avg") X(remove_dc, "dc") X(peak_hold, "hold") \
 X(floor_db, "floor") X(range_db, "range") X(fps, "fps") \
 X(palette, "palette") X(step_mhz, "step") \
 X(tx_channel, "txchan") X(tx_duration_s, "txtime") X(tx_rate_pps, "txrate")

void load_settings(Settings &s) {
    nvs_handle_t handle;
    if (nvs_open("sdr32", NVS_READONLY, &handle) != ESP_OK) return;
    uint32_t version = 0;
    if (nvs_get_u32(handle, "version", &version) == ESP_OK && version == 1) {
#define READ(field, key) { int32_t value; if (nvs_get_i32(handle, key, &value) == ESP_OK) s.field = value; }
        SETTINGS_KEYS(READ)
#undef READ
    }
    nvs_close(handle);
    s.validate();
}
esp_err_t save_settings(const Settings &s) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("sdr32", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u32(handle, "version", 1);
#define WRITE(field, key) if (err == ESP_OK) err = nvs_set_i32(handle, key, s.field);
    SETTINGS_KEYS(WRITE)
#undef WRITE
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}
