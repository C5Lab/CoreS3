// SPDX-License-Identifier: GPL-3.0-or-later
// Runs the production spectrum pipeline with the vendored ESP-DSP ANSI kernel.
#include "spectrum.hpp"
#include "test_tx_frame.hpp"
#include "test_tx.hpp"
#include "nvs.h"
#include <cassert>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

static std::map<std::string, int32_t> flash;
static esp_err_t commit_result = ESP_OK;
esp_err_t nvs_open(const char *, int, nvs_handle_t *h) { *h = 1; return ESP_OK; }
void nvs_close(nvs_handle_t) {}
esp_err_t nvs_get_i32(nvs_handle_t, const char *key, int32_t *v) {
    if (!flash.count(key)) return ESP_ERR_NOT_FOUND;
    *v = flash[key]; return ESP_OK;
}
esp_err_t nvs_set_i32(nvs_handle_t, const char *key, int32_t v) { flash[key] = v; return ESP_OK; }
esp_err_t nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *v) { int32_t n = 0; auto e = nvs_get_i32(h, key, &n); *v = n; return e; }
esp_err_t nvs_set_u32(nvs_handle_t h, const char *key, uint32_t v) { return nvs_set_i32(h, key, v); }
esp_err_t nvs_commit(nvs_handle_t) { return commit_result; }

static std::vector<uint32_t> tone(unsigned n, int bin, float amplitude, int dc_i = 0, int dc_q = 0) {
    std::vector<uint32_t> data(n);
    for (unsigned j = 0; j < n; ++j) {
        float angle = 2*3.14159265358979323846f*bin*j/n;
        int i = std::lround(amplitude*std::cos(angle)) + dc_i;
        int q = std::lround(amplitude*std::sin(angle)) + dc_q;
        assert(i >= -512 && i <= 511 && q >= -512 && q <= 511);
        data[j] = (uint32_t(i)&1023) | ((uint32_t(q)&1023)<<10) | (40u<<20);
    }
    return data;
}
int main() {
    static Spectrum processor;
    assert(processor.init() == ESP_OK);
    Settings s;
    s.averaging = 1;
    SpectrumFrame f;
    for (unsigned n : {256u,512u,1024u,2048u}) {
        s.fft_size = n;
        for (int direction : {-1, 1}) {
            processor.reset();
            auto samples = tone(n, direction*int(n/8), 256);
            sdr_capture_t capture{samples.data(), n, 100, 40, 82, 0};
            assert(processor.process(capture, s, f) == ESP_OK);
            float expected = s.frequency_mhz + direction*float(s.rate_mhz)/8.0f;
            assert(std::fabs(f.strongest_mhz-expected) < .001);
            assert(std::fabs(f.strongest_db+6.0206f) < .04);
            int pixel = int((direction > 0 ? .625f : .375f)*PLOT_WIDTH);
            assert(f.db[pixel] > -6.1f);
        }
    }
    // DC removal must suppress a constant ADC offset before the Hann window.
    auto dc = tone(s.fft_size, 0, 0, 180, -90);
    sdr_capture_t capture{dc.data(), s.fft_size, 100, 40, 82, 0};
    processor.reset();
    s.remove_dc = true;
    assert(processor.process(capture, s, f) == ESP_OK);
    assert(f.strongest_db <= -119);
    processor.reset();
    s.remove_dc = false;
    assert(processor.process(capture, s, f) == ESP_OK);
    assert(f.strongest_db > -10);
    // Averaging operates in linear power; held peak survives a quiet frame.
    s.remove_dc = true;
    s.averaging = 4;
    auto loud = tone(s.fft_size, s.fft_size/8, 256);
    auto quiet = tone(s.fft_size, s.fft_size/8, 0);
    processor.reset();
    capture.words = loud.data();
    assert(processor.process(capture, s, f) == ESP_OK);
    capture.words = quiet.data();
    assert(processor.process(capture, s, f) == ESP_OK);
    assert(std::fabs(f.strongest_db-(-6.0206f+10*std::log10(.75f))) < .04);
    assert(std::fabs(f.peak[180]+6.0206f) < .04);
    processor.clear_peak();
    assert(processor.process(capture, s, f) == ESP_OK);
    assert(f.peak[180] < -8);
    capture.count = 255;
    assert(processor.process(capture, s, f) == ESP_ERR_INVALID_ARG);

    s.frequency_mhz = 2650; s.floor_db = -105; s.palette = 2;
    assert(save_settings(s) == ESP_OK);
    Settings restored;
    load_settings(restored);
    assert(restored.frequency_mhz == 2650 && restored.floor_db == -105 && restored.palette == 2);
    flash["rate"] = -1; flash["fft"] = 13; flash["freq"] = 9999;
    load_settings(restored);
    assert(restored.rate_mhz == 40 && restored.fft_size == 1024 && restored.frequency_mhz == 2700);
    flash["version"] = 99;
    Settings defaults;
    load_settings(defaults);
    assert(defaults.frequency_mhz == 2412);
    commit_result = ESP_FAIL;
    assert(save_settings(s) == ESP_FAIL);

    // The laboratory transmitter must remain a directed Probe Request with a
    // bounded random vendor IE. It must never turn into a deauth/disassoc frame.
    uint8_t frame[TX_TEST_FRAME_BYTES];
    const uint8_t mac[6] = {0x18, 0xfe, 0x34, 0x12, 0x34, 0x56};
    uint8_t random[TX_TEST_RANDOM_BYTES];
    for (unsigned i = 0; i < sizeof(random); ++i) random[i] = i;
    assert(tx_test_build_probe(frame, sizeof(frame), mac, 6, 0x1234, random) == sizeof(frame));
    assert(frame[0] == 0x40 && frame[1] == 0); // Probe Request, never deauth/disassoc
    for (unsigned i = 4; i < 10; ++i) assert(frame[i] == 0xff);
    assert(std::memcmp(frame + 10, mac, 6) == 0);
    for (unsigned i = 16; i < 22; ++i) assert(frame[i] == 0xff);
    std::size_t at = 24;
    assert(frame[at] == 0 && frame[at+1] == 10 && std::memcmp(frame+at+2, "SDR32-TEST", 10) == 0);
    at += 12;
    assert(frame[at] == 1 && frame[at+1] == 8); at += 10;
    assert(frame[at] == 50 && frame[at+1] == 4); at += 6;
    assert(frame[at] == 3 && frame[at+1] == 1 && frame[at+2] == 6); at += 3;
    assert(frame[at] == 221 && frame[at+1] == 38); at += 2;
    assert(std::memcmp(frame+at, mac, 3) == 0 && frame[at+3] == 0x53);
    assert(frame[at+4] == 0x34 && frame[at+5] == 0x12);
    assert(std::memcmp(frame+at+6, random, sizeof(random)) == 0);
    assert(tx_test_build_probe(frame, sizeof(frame)-1, mac, 6, 0, random) == 0);
    assert(tx_test_build_probe(frame, sizeof(frame), mac, 12, 0, random) == 0);

    Settings tx_invalid;
    tx_invalid.tx_channel = 99; tx_invalid.tx_duration_s = 29; tx_invalid.tx_rate_pps = 100;
    tx_invalid.validate();
    assert(tx_invalid.tx_channel == 11 && tx_invalid.tx_duration_s == 5 && tx_invalid.tx_rate_pps == 5);
    assert(tx_test_config_valid({11, 360, 1000}));
    assert(!tx_test_config_valid({12, 360, 1000}));
    assert(!tx_test_config_valid({1, 29, 10}));
    assert(!tx_test_config_valid({1, 360, 20}));
    Settings extended;
    extended.tx_duration_s = 360;
    extended.tx_rate_pps = 1000;
    commit_result = ESP_OK;
    assert(save_settings(extended) == ESP_OK);
    Settings loaded;
    load_settings(loaded);
    assert(loaded.tx_duration_s == 360 && loaded.tx_rate_pps == 1000);
    processor.deinit();
    std::puts("PASS: FFT, NVS, bounded Probe Request frame, random test payload and TX setting validation");
}
