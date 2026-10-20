// SPDX-License-Identifier: GPL-3.0-or-later
/* Adapted from ESPARGOS/esp-sdr, main/targets/esp32s3/receiver.c
 * e2b35dc593a378e4c5eb69cb8db59313117c9838. Snapshot RX only.
 * Kept separate from display code because these are private PHY ABIs. */
#include "esp_sdr.h"
#include <string.h>
#include "sdkconfig.h"
#include "esp_event.h"
#include "esp_phy_cert_test.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "heap_memory_layout.h"
#include "soc/soc.h"
#include "rx_recalibration.h"
#include "rx_lo.h"
#include "rx_bandwidth.h"

#define BANK_START 0x3fcd0000
#define BANK_END   0x3fce0000
#define OWNER_REG  0x600c101cu
#define DUMP_CTRL  0x60033d5cu
#define DUMP_PACK  0x60033d90u
#define GAIN_REG   0x6001c02cu
#define IQ_BUFFER ((volatile uint32_t *)BANK_START)
SOC_RESERVE_MEMORY_REGION(BANK_START, BANK_END, sdr_snapshot);

extern void stop_tx_tone(unsigned);
extern void rom_pbus_workmode(void);
extern void rom_pbus_xpd_rx_on(unsigned);
extern void rom_pbus_xpd_tx_off(void);
extern void rom_set_rxclk_en(unsigned);
extern void set_chanfreq(unsigned, unsigned);
extern void set_rf_freq_offset(unsigned, unsigned, int);
extern void force_rx_gain(unsigned, unsigned, unsigned);
extern unsigned rom_chip_i2c_readReg(unsigned, unsigned, unsigned);
extern void rom_chip_i2c_writeReg(unsigned, unsigned, unsigned, unsigned);
// librftest references its shell parser; this application exposes no RF shell.
int cmd_parse(char *cmd, char *name, int *argc, char **argv) {
    (void)cmd; (void)name; (void)argc; (void)argv;
    return -1;
}
static sdr_config_t current = {2412, 40, 0, true, 40};
static bool initialized, ready, wifi_tx_test, owns_wifi, owns_event_loop;
static unsigned calibrated_mhz;

unsigned sdr_gain_max(void) {
    unsigned maximum = (REG_READ(GAIN_REG) >> 8) & 127u;
    return maximum <= 82 ? maximum : 0;
}
static void prepare_rx(void) {
    if (ready && !rx_recalibration_stale()) return;
    if (calibrated_mhz != current.frequency_mhz || rx_recalibration_stale()) {
        rx_recalibrate(current.frequency_mhz);
        calibrated_mhz = current.frequency_mhz;
    }
    unsigned mhz = current.frequency_mhz;
    bool channel = ((mhz >= 2412 && mhz <= 2472 && (mhz - 2412) % 5 == 0) || mhz == 2484);
    rx_lo_select(false);
    set_chanfreq(channel ? mhz : 2412, 0);
    if (!channel) set_rf_freq_offset(0, mhz, 0);
    stop_tx_tone(1);
    rom_pbus_workmode();
    rom_pbus_xpd_tx_off();
    rom_pbus_xpd_rx_on(1);
    rom_set_rxclk_en(1);
    unsigned index = current.gain_index;
    if (index > sdr_gain_max()) index = sdr_gain_max();
    force_rx_gain(!current.hardware_agc, index, 0);
    esp_rom_delay_us(3000);
    ready = true;
}

esp_err_t sdr_init(void) {
    if (initialized) return ESP_OK;
    if (owns_wifi || owns_event_loop) {
        esp_err_t cleanup = sdr_deinit();
        if (cleanup != ESP_OK) return cleanup;
    }
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    owns_event_loop = err == ESP_OK;
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    if ((err = esp_wifi_init(&config)) != ESP_OK) goto failed;
    owns_wifi = true;
    if ((err = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK) goto failed;
    if ((err = esp_wifi_set_mode(WIFI_MODE_NULL)) != ESP_OK) goto failed;
    if ((err = esp_wifi_start()) != ESP_OK) goto failed;
    if ((err = esp_wifi_set_ps(WIFI_PS_NONE)) != ESP_OK) goto failed;
    if ((err = esp_wifi_set_promiscuous(true)) != ESP_OK) goto failed;
    if ((err = esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE)) != ESP_OK) goto failed;
    prepare_rx();
    initialized = true;
    return ESP_OK;
failed:
    (void)sdr_deinit();
    return err;
}

esp_err_t sdr_deinit(void) {
    esp_err_t first = ESP_OK;
    initialized = ready = wifi_tx_test = false;
    calibrated_mhz = 0;
    if (owns_wifi) {
        // A synchronous capture has already returned SRAM to the CPU.
        REG_WRITE(DUMP_CTRL, 0);
        esp_err_t err = esp_wifi_stop();
        if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) first = err;
        err = esp_wifi_deinit();
        if (err == ESP_OK) owns_wifi = false;
        else if (first == ESP_OK) first = err;
    }
    if (owns_event_loop && !owns_wifi) {
        esp_err_t err = esp_event_loop_delete_default();
        if (err == ESP_OK) owns_event_loop = false;
        else if (first == ESP_OK) first = err;
    }
    return first;
}

esp_err_t sdr_configure(const sdr_config_t *config) {
    if (!initialized || wifi_tx_test) return ESP_ERR_INVALID_STATE;
    if (!config || config->frequency_mhz < 2200 || config->frequency_mhz > 2700 ||
        (config->sample_rate_mhz != 16 && config->sample_rate_mhz != 40 && config->sample_rate_mhz != 80) ||
        (config->bandwidth_mhz && (config->bandwidth_mhz < 13 || config->bandwidth_mhz > 69)) ||
        config->gain_index > 82) return ESP_ERR_INVALID_ARG;
    if (current.frequency_mhz != config->frequency_mhz ||
        current.hardware_agc != config->hardware_agc || current.gain_index != config->gain_index) ready = false;
    current = *config;
    prepare_rx();
    return ESP_OK;
}

esp_err_t sdr_capture(unsigned count, sdr_capture_t *result) {
    if (!initialized || wifi_tx_test) return ESP_ERR_INVALID_STATE;
    if (!result || count < 256 || count > 2048 || (count & (count - 1))) return ESP_ERR_INVALID_ARG;
    prepare_rx();
    // Written before handing SRAM to the modem; never dereference while owned.
    for (unsigned j = 0; j < count; ++j) IQ_BUFFER[j] = 0xa5a0055a;
    unsigned saved_filter[2];
    for (unsigned j = 0; j < 2; ++j) {
        saved_filter[j] = rom_chip_i2c_readReg(0x67, 0, 4 + j);
        unsigned dcap = rx_bandwidth_dcap(current.bandwidth_mhz);
        rom_chip_i2c_writeReg(0x67, 0, 4 + j, (saved_filter[j] & ~63u) | dcap);
    }
    uint32_t owner = REG_READ(OWNER_REG);
    uint32_t rate_bits = current.sample_rate_mhz == 40 ? 1u << 15 :
                         current.sample_rate_mhz == 16 ? 1u << 16 : 0;
    int64_t start = esp_timer_get_time();
    REG_WRITE(DUMP_CTRL, 0);
    REG_WRITE(DUMP_PACK, (1u << 6) | (2u << 12) | (3u << 18));
    REG_WRITE(OWNER_REG, (owner & ~15u) | 4u);
    uint32_t ctrl = 0x80000000u | rate_bits | count;
    REG_WRITE(DUMP_CTRL, ctrl);
    REG_WRITE(DUMP_CTRL, ctrl | (1u << 19));
    REG_WRITE(DUMP_CTRL, ctrl);
    while (!(REG_READ(DUMP_CTRL) & (1u << 18)) && esp_timer_get_time() - start < 20000) {}
    bool done = (REG_READ(DUMP_CTRL) & (1u << 18)) != 0;
    unsigned elapsed = esp_timer_get_time() - start;
    // Always stop the writer and return the bank, including the timeout path.
    REG_WRITE(DUMP_CTRL, 0);
    REG_WRITE(OWNER_REG, owner);
    for (unsigned j = 0; j < 2; ++j) rom_chip_i2c_writeReg(0x67, 0, 4 + j, saved_filter[j]);
    if (!done) { ready = false; return ESP_ERR_TIMEOUT; }
    unsigned clipped = 0;
    for (unsigned j = 0; j < count; ++j) {
        uint32_t w = IQ_BUFFER[j];
        if (w == 0xa5a0055a) { ready = false; return ESP_ERR_TIMEOUT; }
        unsigned i = w & 1023, q = (w >> 10) & 1023;
        clipped += (i == 511 || i == 512 || q == 511 || q == 512);
    }
    *result = (sdr_capture_t){(const uint32_t *)IQ_BUFFER, count, elapsed,
                            (IQ_BUFFER[0] >> 20) & 255, sdr_gain_max(), clipped};
    return ESP_OK;
}

bool sdr_wifi_tx_test_active(void) { return wifi_tx_test; }

esp_err_t sdr_begin_wifi_tx_test(unsigned channel) {
    if (!initialized || wifi_tx_test) return ESP_ERR_INVALID_STATE;
    if (channel < 1 || channel > 11) return ESP_ERR_INVALID_ARG;

    // The dump engine must be idle before the normal Wi-Fi MAC gets the PHY.
    REG_WRITE(DUMP_CTRL, 0);
    ready = false;
    esp_err_t err;
    wifi_tx_test = true;
    if ((err = esp_wifi_set_promiscuous(false)) != ESP_OK) goto restore;
    if ((err = esp_wifi_stop()) != ESP_OK) goto restore;
    if ((err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) goto restore;
    if ((err = esp_wifi_start()) != ESP_OK) goto restore;
    if ((err = esp_wifi_set_ps(WIFI_PS_NONE)) != ESP_OK) goto restore;
    // Preserve the local SDR32 request. The PHY applies its supported power cap.
    if ((err = esp_wifi_set_max_tx_power(84)) != ESP_OK) goto restore;
    if ((err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE)) != ESP_OK) goto restore;
    return ESP_OK;

restore:
    // sdr_end_wifi_tx_test() also handles a start that failed midway.
    (void)sdr_end_wifi_tx_test();
    return err;
}

esp_err_t sdr_end_wifi_tx_test(void) {
    if (!initialized) return ESP_ERR_INVALID_STATE;
    REG_WRITE(DUMP_CTRL, 0);
    esp_err_t first = ESP_OK;
#define KEEP_FIRST(call) do { \
    esp_err_t e_ = (call); \
    if (e_ == ESP_ERR_WIFI_NOT_STARTED) e_ = ESP_OK; \
    if (first == ESP_OK && e_ != ESP_OK) first = e_; \
} while (0)
    KEEP_FIRST(esp_wifi_stop());
    KEEP_FIRST(esp_wifi_set_mode(WIFI_MODE_NULL));
    KEEP_FIRST(esp_wifi_start());
    KEEP_FIRST(esp_wifi_set_ps(WIFI_PS_NONE));
    KEEP_FIRST(esp_wifi_set_promiscuous(true));
    KEEP_FIRST(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));
#undef KEEP_FIRST
    ready = false;
    calibrated_mhz = 0;
    wifi_tx_test = first != ESP_OK;
    if (first == ESP_OK) prepare_rx();
    return first;
}
