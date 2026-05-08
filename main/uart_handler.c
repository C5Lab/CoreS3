#include "uart_handler.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "uart";

uart_port_mode_t uart_port_mode = UART_PORT_MODE_MBUS;

void uart_handler_get_pins(uart_port_mode_t mode, int *tx, int *rx)
{
    switch (mode) {
    case UART_PORT_MODE_PORTC:
        if (tx) *tx = 17;
        if (rx) *rx = 18;
        break;
    case UART_PORT_MODE_MBUS:
    default:
        if (tx) *tx = 43;
        if (rx) *rx = 44;
        break;
    }
}

#define UART_BUF_SIZE         (16 * 1024)  /* 16 KB – large scan output */
/* Górny limit żeby zbłądzony strumień UART nie wyczerpał PSRAM. */
#define UART_COLLECT_HARD_CAP 4096

static uart_line_callback_t line_callback = NULL;
static volatile bool collecting = false;
static char **collected_lines    = NULL;   /* w PSRAM, dynamicznie rośnie */
static int    collected_count    = 0;
static int    collected_capacity = 0;
static char end_marker[64] = {0};
static uart_collect_callback_t collect_callback = NULL;
static TickType_t collect_start_tick = 0;

static bool ensure_capacity(int needed)
{
    if (needed <= collected_capacity) return true;

    int new_cap = collected_capacity ? collected_capacity : 64;
    while (new_cap < needed && new_cap < UART_COLLECT_HARD_CAP) new_cap *= 2;
    if (new_cap > UART_COLLECT_HARD_CAP) new_cap = UART_COLLECT_HARD_CAP;
    if (new_cap <= collected_capacity) return false;

    char **np = heap_caps_realloc(collected_lines,
                                  new_cap * sizeof(char *),
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!np) {
        ESP_LOGE(TAG, "PSRAM realloc failed (%d -> %d)", collected_capacity, new_cap);
        return false;
    }
    collected_lines = np;
    collected_capacity = new_cap;
    return true;
}

static char *psram_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) memcpy(p, s, n);
    return p;
}

static void release_lines(void)
{
    for (int i = 0; i < collected_count; i++) {
        if (collected_lines && collected_lines[i]) {
            free(collected_lines[i]);
            collected_lines[i] = NULL;
        }
    }
    collected_count = 0;
}

static bool is_noisy_subghz_line(const char *line)
{
    return line && strstr(line, "[SUBGHZ_RSSI]") != NULL;
}

static void process_line(char *line)
{
    int len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n' || line[len - 1] == ' '))
        line[--len] = '\0';
    if (len == 0) return;

    if (is_noisy_subghz_line(line))
        ESP_LOGD(TAG, "RX: %s", line);
    else
        ESP_LOGI(TAG, "RX: %s", line);

    if (collecting) {
        if (collected_count >= UART_COLLECT_HARD_CAP) {
            ESP_LOGW(TAG, "COLLECT hard cap (%d) reached, dropping line",
                     UART_COLLECT_HARD_CAP);
        } else if (!ensure_capacity(collected_count + 1)) {
            ESP_LOGW(TAG, "COLLECT cannot grow PSRAM buffer, dropping line");
        } else {
            char *dup = psram_strdup(line);
            if (dup) {
                collected_lines[collected_count++] = dup;
            } else {
                ESP_LOGW(TAG, "COLLECT psram_strdup failed, dropping line");
            }
        }
        bool marker_found = (strstr(line, end_marker) != NULL);
        ESP_LOGD(TAG, "COLLECT [%d]: \"%s\" (marker=%s)",
                 collected_count, line, marker_found ? "MATCH" : "no");
        if (marker_found) {
            ESP_LOGI(TAG, "Collection complete, %d lines", collected_count);
            collecting = false;
            if (collect_callback)
                collect_callback((const char **)collected_lines, collected_count);
            release_lines();
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
    release_lines();
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

    int tx_pin = 43;
    int rx_pin = 44;
    uart_handler_get_pins(uart_port_mode, &tx_pin, &rx_pin);

    /* Default IO MUX functions on these pins may collide with UART0/other
     * peripherals. IO MUX has priority over GPIO Matrix, so we reset the pins
     * to plain GPIO mode first, allowing uart_set_pin to route UART1
     * through the GPIO Matrix. */
    gpio_reset_pin(tx_pin);
    gpio_reset_pin(rx_pin);

    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, tx_pin, rx_pin,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    xTaskCreate(uart_rx_task, "uart_rx", 8192, NULL, 12, NULL);
    const char *port_name = (uart_port_mode == UART_PORT_MODE_PORTC) ? "Port C" : "MBus";
    ESP_LOGI(TAG, "UART initialised %s TX=%d RX=%d @ %d (port %d)",
             port_name, tx_pin, rx_pin, UART_BAUD_RATE, UART_PORT);
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
    release_lines();
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
    release_lines();
}

bool uart_is_collecting(void)
{
    return collecting;
}

void uart_set_line_callback(uart_line_callback_t callback)
{
    line_callback = callback;
}
