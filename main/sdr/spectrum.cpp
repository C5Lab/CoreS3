// SPDX-License-Identifier: GPL-3.0-or-later
#include "spectrum.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "dsps_fft2r.h"

esp_err_t Spectrum::init() { return dsps_fft2r_init_fc32(twiddles_, MAX_FFT); }
void Spectrum::deinit() { dsps_fft2r_deinit_fc32(); }
void Spectrum::reset() { fresh_ = true; size_ = 0; clear_peak(); }
void Spectrum::clear_peak() { std::fill_n(peaks_, MAX_FFT, 0.0f); }

esp_err_t Spectrum::process(const sdr_capture_t &cap, const Settings &s, SpectrumFrame &frame) {
    const unsigned n = s.fft_size;
    if (!cap.words || cap.count != n || n > MAX_FFT || n < 256 || (n & (n - 1))) return ESP_ERR_INVALID_ARG;
    if (size_ != n) {
        window_sum_ = 0;
        for (unsigned j = 0; j < n; ++j) {
            window_[j] = 0.5f - 0.5f * std::cos(2.0f * 3.14159265358979323846f * j / n);
            window_sum_ += window_[j];
        }
        size_ = n;
        fresh_ = true;
        clear_peak();
    }
    float mean_i = 0, mean_q = 0;
    for (unsigned j = 0; j < n; ++j) {
        const uint32_t word = cap.words[j];
        // ESP-SDR's S3 pack order is I in bits 9:0, Q in bits 19:10.
        int i = word & 1023u, q = (word >> 10) & 1023u;
        if (i & 512) i -= 1024;
        if (q & 512) q -= 1024;
        fft_[2*j] = i;
        fft_[2*j+1] = q;
        mean_i += i;
        mean_q += q;
    }
    mean_i = s.remove_dc ? mean_i / n : 0;
    mean_q = s.remove_dc ? mean_q / n : 0;
    for (unsigned j = 0; j < n; ++j) {
        fft_[2*j] = (fft_[2*j] - mean_i) * window_[j];
        fft_[2*j+1] = (fft_[2*j+1] - mean_q) * window_[j];
    }
    esp_err_t err = dsps_fft2r_fc32(fft_, n);
    if (err != ESP_OK) return err;
    if ((err = dsps_bit_rev_fc32(fft_, n)) != ESP_OK) return err;
    // Coherent-gain normalized complex bin power relative to ADC full scale.
    // Relative dBFS, never calibrated dBm or a power-density measurement.
    const float norm = 1.0f / (512.0f * 512.0f * window_sum_ * window_sum_);
    const float alpha = 1.0f / s.averaging;
    float maximum = 0;
    unsigned strongest = 0;
    for (unsigned j = 0; j < n; ++j) {
        unsigned k = (j + n/2) % n; // FFT shift: negative frequencies on left
        float re = fft_[2*k], im = fft_[2*k+1];
        float power = (re*re + im*im) * norm;
        power_[j] = fresh_ ? power : power_[j] + alpha * (power - power_[j]);
        peaks_[j] = std::max(peaks_[j], power_[j]);
        if (power_[j] > maximum) { maximum = power_[j]; strongest = j; }
    }
    fresh_ = false;
    for (int x = 0; x < PLOT_WIDTH; ++x) {
        // Max aggregation preserves narrow carriers. For 256 bins, duplicate
        // the nearest bin into empty pixel intervals instead of reading OOB.
        unsigned first = x*n/PLOT_WIDTH;
        unsigned end = std::max(first+1, (x+1)*n/PLOT_WIDTH);
        float p = 0, hold = 0;
        for (unsigned j = first; j < end; ++j) {
            p = std::max(p, power_[j]);
            hold = std::max(hold, peaks_[j]);
        }
        frame.db[x] = 10.0f * std::log10(std::max(p, 1e-12f));
        frame.peak[x] = 10.0f * std::log10(std::max(hold, 1e-12f));
    }
    frame.strongest_db = 10.0f * std::log10(std::max(maximum, 1e-12f));
    frame.strongest_mhz = s.frequency_mhz + (static_cast<float>(strongest)/n - 0.5f)*s.rate_mhz;
    frame.gain = cap.gain_index;
    frame.clipped = cap.clipped;
    frame.capture_us = cap.capture_us;
    return ESP_OK;
}
