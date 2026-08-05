#include "device_info.h"
#include "uart_handler.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "device_info";

#define BOARD_NAME_PREFIX     "board_name="
#define BOARD_NAME_PREFIX_LEN (sizeof(BOARD_NAME_PREFIX) - 1)
#define BOARD_NAME_TIMEOUT_MS 600

static char s_board_name[40];
static bool s_has_subghz = false;
static bool s_has_nfc = false;
static int  s_nfc_card_count = 0;

static bool probe_subghz_status(void)
{
    ESP_LOGI(TAG, "Probing Sub-GHz module (subghz_status)...");

    char line[256];
    bool found = false;

    /* Retry a few times in case the firmware was busy. uart_send_wait_line
     * owns its own mutex/semaphore, so there is no use-after-free race with
     * the uart_rx task (unlike a per-call line callback). */
    for (int attempt = 0; attempt < 3 && !found; attempt++) {
        if (uart_send_wait_line("subghz_status", "[SUBGHZ_STATUS]",
                                line, sizeof(line), 400)) {
            found = true;
        }
    }

    if (found)
        ESP_LOGI(TAG, "Sub-GHz module available");
    else
        ESP_LOGI(TAG, "Sub-GHz module not detected");

    return found;
}

static bool probe_nfc_init(void)
{
    ESP_LOGI(TAG, "Probing NFC (init_nfc)...");

    char line[256];
    bool found = false;

    for (int attempt = 0; attempt < 1 && !found; attempt++) {
        if (uart_send_wait_line("init_nfc", "[NFC] detected",
                                line, sizeof(line), 3000)) {
            found = true;
        }
    }

    if (found)
        ESP_LOGI(TAG, "NFC module available");
    else
        ESP_LOGI(TAG, "NFC module not detected");

    return found;
}

static int probe_nfc_list_count(void)
{
    ESP_LOGI(TAG, "Probing NFC SD library (nfc_list)...");

    char line[256];
    if (!uart_send_wait_line("nfc_list", "card(s)",
                             line, sizeof(line), 5000)) {
        ESP_LOGI(TAG, "nfc_list timed out (no card count)");
        return 0;
    }

    /* "[NFC] N card(s)" */
    int n = 0;
    const char *p = strstr(line, "[NFC] ");
    if (p && sscanf(p, "[NFC] %d card(s)", &n) == 1) {
        ESP_LOGI(TAG, "NFC SD library: %d card(s)", n);
        return n;
    }

    ESP_LOGI(TAG, "nfc_list: could not parse count from '%s'", line);
    return 0;
}

void device_info_init(void)
{
    s_board_name[0] = '\0';
    s_has_subghz = false;
    s_has_nfc = false;
    s_nfc_card_count = 0;

    char line[256];
    if (uart_send_wait_line("board_name", BOARD_NAME_PREFIX,
                            line, sizeof(line), BOARD_NAME_TIMEOUT_MS)) {
        const char *p = strstr(line, BOARD_NAME_PREFIX);
        if (p) {
            const char *value = p + BOARD_NAME_PREFIX_LEN;
            if (value[0] != '\0' && strcmp(value, "<unset>") != 0)
                snprintf(s_board_name, sizeof(s_board_name), "%s", value);
        }
    } else {
        ESP_LOGI(TAG, "board_name query timed out (no firmware response)");
    }

    if (s_board_name[0])
        ESP_LOGI(TAG, "board_name=%s", s_board_name);
    else
        ESP_LOGI(TAG, "board_name=<unset>");

    s_has_subghz = probe_subghz_status();

    s_has_nfc = probe_nfc_init();
    if (s_has_nfc)
        s_nfc_card_count = probe_nfc_list_count();
}

const char *device_info_subghz_title(void)
{
    return s_board_name[0] ? s_board_name : "SubGhz-NoName";
}

bool device_info_has_subghz(void)
{
    return s_has_subghz;
}

bool device_info_has_nfc(void)
{
    return s_has_nfc;
}

int device_info_nfc_card_count(void)
{
    return s_nfc_card_count;
}

bool device_info_has_nfc_cards(void)
{
    return s_nfc_card_count > 0;
}
