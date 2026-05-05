#include "uart_handler.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "uart";

#define UART_BUF_SIZE       (16 * 1024)   /* 16 KB – large scan output */
#define MAX_COLLECTED_LINES 128

static uart_line_callback_t line_callback = NULL;
static volatile bool collecting = false;
static char *collected_lines[MAX_COLLECTED_LINES];
static int collected_count = 0;
static char end_marker[64] = {0};
static uart_collect_callback_t collect_callback = NULL;
static TickType_t collect_start_tick = 0;

static void process_line(char *line)
{
    int len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n' || line[len - 1] == ' '))
        line[--len] = '\0';
    if (len == 0) return;

    ESP_LOGI(TAG, "RX: %s", line);

    if (collecting) {
        if (collected_count < MAX_COLLECTED_LINES) {
            collected_lines[collected_count] = strdup(line);
            if (collected_lines[collected_count])
                collected_count++;
        } else {
            ESP_LOGW(TAG, "COLLECT buffer full (%d), dropping line", MAX_COLLECTED_LINES);
        }
        bool marker_found = (strstr(line, end_marker) != NULL);
        ESP_LOGD(TAG, "COLLECT [%d]: \"%s\" (marker=%s)",
                 collected_count, line, marker_found ? "MATCH" : "no");
        if (marker_found) {
            ESP_LOGI(TAG, "Collection complete, %d lines", collected_count);
            collecting = false;
            if (collect_callback)
                collect_callback((const char **)collected_lines, collected_count);
            for (int i = 0; i < collected_count; i++) {
                free(collected_lines[i]);
                collected_lines[i] = NULL;
            }
            collected_count = 0;
        }
    }

    if (line_callback)
        line_callback(line);
}

static void finish_collect(bool timed_out)
{
    collecting = false;
    if (timed_out)
        ESP_LOGE(TAG, "Collection TIMEOUT after %d ms, %d lines collected",
                 UART_COLLECT_TIMEOUT_MS, collected_count);
    if (collect_callback)
        collect_callback((const char **)collected_lines, collected_count);
    for (int i = 0; i < collected_count; i++) {
        free(collected_lines[i]);
        collected_lines[i] = NULL;
    }
    collected_count = 0;
}

static void uart_rx_task(void *arg)
{
    uint8_t *rx_buf = malloc(UART_BUF_SIZE);
    char *line_buf = malloc(UART_MAX_LINE_LEN);
    int line_pos = 0;
    int idle_loops = 0;

    while (1) {
        int len = uart_read_bytes(UART_PORT, rx_buf, UART_BUF_SIZE - 1, pdMS_TO_TICKS(100));

        /* ---- timeout check ---- */
        if (collecting) {
            TickType_t elapsed = xTaskGetTickCount() - collect_start_tick;
            if (elapsed > pdMS_TO_TICKS(UART_COLLECT_TIMEOUT_MS)) {
                finish_collect(true);
            }
        }

        if (len > 0) {
            idle_loops = 0;
            ESP_LOGD(TAG, "RX chunk: %d bytes", len);

            /* warn if driver buffer is getting full */
            size_t buffered = 0;
            uart_get_buffered_data_len(UART_PORT, &buffered);
            if (buffered > (UART_BUF_SIZE / 2))
                ESP_LOGW(TAG, "UART RX buffer high: %d/%d bytes",
                         (int)buffered, UART_BUF_SIZE);

            for (int i = 0; i < len; i++) {
                char c = rx_buf[i];
                if (c == '\n' || c == '\r') {
                    if (line_pos > 0) {
                        line_buf[line_pos] = '\0';
                        process_line(line_buf);
                        line_pos = 0;
                    }
                } else if (line_pos < UART_MAX_LINE_LEN - 1) {
                    line_buf[line_pos++] = c;
                }
            }
        } else {
            idle_loops++;
            if (idle_loops % 50 == 0 && collecting) {
                ESP_LOGW(TAG, "Still collecting, %d lines so far, waiting for '%s'",
                         collected_count, end_marker);
            }
        }
    }

    free(rx_buf);
    free(line_buf);
}

void uart_handler_init(void)
{
    uart_config_t cfg = {
        .baud_rate  = UART_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, UART_BUF_SIZE, UART_BUF_SIZE, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));

    /* GPIO 43/44 default IO MUX function is U0TXD/U0RXD (UART0).
     * IO MUX has priority over GPIO Matrix, so we must reset the pins
     * to plain GPIO mode first, allowing uart_set_pin to route UART1
     * through the GPIO Matrix. */
    gpio_reset_pin(UART_TX_PIN);
    gpio_reset_pin(UART_RX_PIN);

    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_TX_PIN, UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    xTaskCreate(uart_rx_task, "uart_rx", 8192, NULL, 12, NULL);
    ESP_LOGI(TAG, "UART initialised TX=%d RX=%d @ %d (port %d)",
             UART_TX_PIN, UART_RX_PIN, UART_BAUD_RATE, UART_PORT);
}

void uart_send_command(const char *cmd)
{
    int bytes = strlen(cmd);
    ESP_LOGI(TAG, "TX: %s (%d bytes)", cmd, bytes);
    int written = uart_write_bytes(UART_PORT, cmd, bytes);
    uart_write_bytes(UART_PORT, "\n", 1);
    ESP_LOGD(TAG, "TX written: %d/%d bytes", written, bytes);
}

void uart_start_collect(const char *marker, uart_collect_callback_t on_complete)
{
    for (int i = 0; i < collected_count; i++) {
        free(collected_lines[i]);
        collected_lines[i] = NULL;
    }
    collected_count = 0;
    strncpy(end_marker, marker, sizeof(end_marker) - 1);
    end_marker[sizeof(end_marker) - 1] = '\0';
    collect_callback = on_complete;
    collect_start_tick = xTaskGetTickCount();
    collecting = true;
    ESP_LOGI(TAG, "Collecting until: '%s' (timeout %d ms)", marker, UART_COLLECT_TIMEOUT_MS);
}

void uart_stop_collect(void)
{
    collecting = false;
    collect_callback = NULL;
    for (int i = 0; i < collected_count; i++) {
        free(collected_lines[i]);
        collected_lines[i] = NULL;
    }
    collected_count = 0;
}

bool uart_is_collecting(void)
{
    return collecting;
}

void uart_set_line_callback(uart_line_callback_t callback)
{
    line_callback = callback;
}
