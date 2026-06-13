#include "ir_tx.h"

#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_log.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "ir_tx";

#define IR_RMT_RESOLUTION_HZ 1000000u /* 1 tick = 1 us */
#define IR_RMT_MAX_TICKS     32767u   /* 15-bit RMT duration field */

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_copy_encoder;
static uint32_t s_carrier_hz;
static uint8_t  s_duty_pct;

esp_err_t ir_tx_init(void)
{
    if (s_chan) return ESP_OK;

    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num          = IR_TX_GPIO,
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = IR_RMT_RESOLUTION_HZ,
        .mem_block_symbols = 128,
        .trans_queue_depth = 4,
        .flags = {
            .invert_out = false,
            .with_dma   = false,
        },
    };

    esp_err_t err = rmt_new_tx_channel(&tx_cfg, &s_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_tx_channel failed: %s", esp_err_to_name(err));
        s_chan = NULL;
        return err;
    }

    rmt_copy_encoder_config_t enc_cfg = {};
    err = rmt_new_copy_encoder(&enc_cfg, &s_copy_encoder);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_copy_encoder failed: %s", esp_err_to_name(err));
        rmt_del_channel(s_chan);
        s_chan = NULL;
        return err;
    }

    err = rmt_enable(s_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_enable failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "IR TX ready on GPIO%d", IR_TX_GPIO);
    return ESP_OK;
}

static esp_err_t ir_tx_apply_carrier(uint32_t carrier_hz, uint8_t duty_pct)
{
    if (carrier_hz == s_carrier_hz && duty_pct == s_duty_pct) return ESP_OK;

    rmt_carrier_config_t carrier_cfg = {
        .frequency_hz = carrier_hz,
        .duty_cycle   = (float)duty_pct / 100.0f,
        .flags = {
            .polarity_active_low = false,
        },
    };
    esp_err_t err = rmt_apply_carrier(s_chan, &carrier_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_apply_carrier failed: %s", esp_err_to_name(err));
        return err;
    }
    s_carrier_hz = carrier_hz;
    s_duty_pct = duty_pct;
    return ESP_OK;
}

/* Number of RMT sub-units a single timing expands into once split to fit the
 * 15-bit duration field. */
static size_t chunks_for(uint32_t duration_us)
{
    if (duration_us == 0) return 1;
    return (duration_us + IR_RMT_MAX_TICKS - 1) / IR_RMT_MAX_TICKS;
}

void ir_tx_send(const uint32_t *timings_us, size_t count, uint32_t carrier_hz, uint8_t duty_pct)
{
    if (!s_chan || !timings_us || count == 0) return;
    if (ir_tx_apply_carrier(carrier_hz, duty_pct) != ESP_OK) return;

    /* Expand timings (splitting long ones) into a flat level/duration unit
     * stream, then pack two units per RMT symbol. */
    size_t n_units = 0;
    for (size_t i = 0; i < count; ++i) {
        n_units += chunks_for(timings_us[i]);
    }
    size_t n_symbols = (n_units + 1) / 2;

    rmt_symbol_word_t *symbols = calloc(n_symbols, sizeof(rmt_symbol_word_t));
    if (!symbols) {
        ESP_LOGE(TAG, "calloc %zu symbols failed", n_symbols);
        return;
    }

    size_t unit = 0;
    for (size_t i = 0; i < count; ++i) {
        bool level = (i & 1u) ? true : false; /* even index = space, odd = mark */
        uint32_t remaining = timings_us[i];
        do {
            uint32_t d = remaining > IR_RMT_MAX_TICKS ? IR_RMT_MAX_TICKS : remaining;
            remaining -= d;

            rmt_symbol_word_t *sym = &symbols[unit / 2];
            if ((unit & 1u) == 0) {
                sym->duration0 = d;
                sym->level0 = level;
            } else {
                sym->duration1 = d;
                sym->level1 = level;
            }
            unit++;
        } while (remaining > 0);
    }

    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };

    esp_err_t err = rmt_transmit(s_chan, s_copy_encoder, symbols,
                                 n_symbols * sizeof(rmt_symbol_word_t), &tx_config);
    if (err == ESP_OK) {
        rmt_tx_wait_all_done(s_chan, -1);
    } else {
        ESP_LOGE(TAG, "rmt_transmit failed: %s", esp_err_to_name(err));
    }

    free(symbols);
}
