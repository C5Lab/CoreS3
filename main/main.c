#include "bsp/m5stack_core_s3.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "uart_handler.h"
#include "cardkb.h"
#include "home_screen.h"

static const char *TAG = "app";

void app_main(void)
{
    ESP_LOGI(TAG, "M5Stack Core S3 – LABORATORIUM");

    /* NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* BSP: display + touch + LVGL */
    bsp_display_start();
    bsp_display_brightness_set(80);

    /* UART to ESP32C5 */
    uart_handler_init();

    /* CardKB on Grove Port A (I2C) */
    ESP_ERROR_CHECK(cardkb_init());

    bsp_display_lock(0);
    show_home_screen();
    bsp_display_unlock();

    ESP_LOGI(TAG, "Init complete");
}
