#include "cardkb.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "cardkb";

static i2c_master_bus_handle_t grove_i2c_bus = NULL;
static i2c_master_dev_handle_t cardkb_dev    = NULL;

esp_err_t cardkb_init(void)
{
    if (cardkb_dev) {
        ESP_LOGW(TAG, "Already initialised");
        return ESP_OK;
    }

    /* Create a separate I2C master bus on the Grove Port A pins */
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port   = CARDKB_I2C_PORT,
        .sda_io_num = CARDKB_I2C_SDA,
        .scl_io_num = CARDKB_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &grove_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create Grove I2C bus: %s", esp_err_to_name(err));
        return err;
    }

    /* Add the CardKB device at 0x5F */
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = CARDKB_I2C_ADDR,
        .scl_speed_hz    = CARDKB_I2C_FREQ_HZ,
    };
    err = i2c_master_bus_add_device(grove_i2c_bus, &dev_cfg, &cardkb_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add CardKB device: %s", esp_err_to_name(err));
        i2c_del_master_bus(grove_i2c_bus);
        grove_i2c_bus = NULL;
        return err;
    }

    ESP_LOGI(TAG, "CardKB initialised (addr=0x%02X, SDA=%d, SCL=%d)",
             CARDKB_I2C_ADDR, CARDKB_I2C_SDA, CARDKB_I2C_SCL);
    return ESP_OK;
}

uint8_t cardkb_read_key(void)
{
    if (!cardkb_dev) return 0;

    uint8_t key = 0;
    esp_err_t err = i2c_master_receive(cardkb_dev, &key, 1, 50 /* ms */);
    if (err != ESP_OK) {
        /* Timeout / NACK is normal when no key is pressed on some FW versions */
        return 0;
    }
    return key;
}

esp_err_t cardkb_deinit(void)
{
    if (cardkb_dev) {
        i2c_master_bus_rm_device(cardkb_dev);
        cardkb_dev = NULL;
    }
    if (grove_i2c_bus) {
        i2c_del_master_bus(grove_i2c_bus);
        grove_i2c_bus = NULL;
    }
    return ESP_OK;
}
