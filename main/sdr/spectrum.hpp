// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "settings.hpp"

constexpr int PLOT_WIDTH = 288;
constexpr unsigned MAX_FFT = 2048;
struct SpectrumFrame {
    float db[PLOT_WIDTH]{};
    float peak[PLOT_WIDTH]{};
    float strongest_db = -120;
    float strongest_mhz = 0;
    unsigned gain = 0;
    unsigned clipped = 0;
    unsigned capture_us = 0;
};

class Spectrum {
public:
    esp_err_t init();
    void deinit();
    void reset();
    void clear_peak();
    esp_err_t process(const sdr_capture_t &capture, const Settings &settings, SpectrumFrame &frame);
private:
    alignas(16) float fft_[2 * MAX_FFT]{};
    alignas(16) float twiddles_[MAX_FFT]{};
    float window_[MAX_FFT]{};
    float power_[MAX_FFT]{};
    float peaks_[MAX_FFT]{};
    unsigned size_ = 0;
    float window_sum_ = 1;
    bool fresh_ = true;
};
