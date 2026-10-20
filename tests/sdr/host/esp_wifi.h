#pragma once
#include <cstddef>
#include <cstdint>
#include "esp_err.h"
#define WIFI_IF_STA 0
esp_err_t esp_wifi_get_mac(int interface, uint8_t *mac);
esp_err_t esp_wifi_80211_tx(int interface, const void *data, int length, bool system_sequence);
