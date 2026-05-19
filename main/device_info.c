#include "device_info.h"
#include "uart_handler.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "device_info";

#define BOARD_NAME_PREFIX     "board_name="
#define BOARD_NAME_PREFIX_LEN (sizeof(BOARD_NAME_PREFIX) - 1)
#define BOARD_NAME_TIMEOUT_MS 500

static char s_board_name[40];
static SemaphoreHandle_t s_query_sem;

static void board_name_line_cb(const char *line)
{
    if (!line)
        return;

    const char *p = strstr(line, BOARD_NAME_PREFIX);
    if (!p)
        return;
    /* Only accept stand-alone prefix (start of line or after whitespace) so we
     * don't match unrelated debug lines that happen to contain the substring. */
    if (p != line && p[-1] != ' ' && p[-1] != '\t')
        return;

    const char *value = p + BOARD_NAME_PREFIX_LEN;

    if (value[0] == '\0' || strcmp(value, "<unset>") == 0) {
        s_board_name[0] = '\0';
    } else {
        snprintf(s_board_name, sizeof(s_board_name), "%s", value);
    }

    if (s_query_sem)
        xSemaphoreGive(s_query_sem);
}

void device_info_init(void)
{
    s_board_name[0] = '\0';

    s_query_sem = xSemaphoreCreateBinary();
    if (!s_query_sem) {
        ESP_LOGW(TAG, "semaphore alloc failed, skipping board_name query");
        return;
    }

    uart_set_line_callback(board_name_line_cb);
    uart_send_command("board_name");

    if (xSemaphoreTake(s_query_sem, pdMS_TO_TICKS(BOARD_NAME_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGI(TAG, "board_name query timed out (no firmware response)");
    }

    uart_set_line_callback(NULL);
    vSemaphoreDelete(s_query_sem);
    s_query_sem = NULL;

    if (s_board_name[0])
        ESP_LOGI(TAG, "board_name=%s", s_board_name);
    else
        ESP_LOGI(TAG, "board_name=<unset>");
}

const char *device_info_subghz_title(void)
{
    return s_board_name[0] ? s_board_name : "SubGhz-NoName";
}
