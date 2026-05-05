#ifndef UART_HANDLER_H
#define UART_HANDLER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>


// M5Bus (default): TX=43, RX=44 | Grove: TX=2, RX=1
// NOTE: Console must be routed to USB_SERIAL_JTAG (sdkconfig) to free these pins!

#define UART_TX_PIN           43
#define UART_RX_PIN           44
#define UART_BAUD_RATE        115200
#define UART_PORT             UART_NUM_1
#define UART_MAX_LINE_LEN     512
#define UART_COLLECT_TIMEOUT_MS 25000   /* max wait for end marker */

typedef void (*uart_line_callback_t)(const char *line);
typedef void (*uart_collect_callback_t)(const char **lines, int line_count);

void uart_handler_init(void);
void uart_send_command(const char *cmd);
void uart_start_collect(const char *end_marker, uart_collect_callback_t on_complete);
void uart_stop_collect(void);
bool uart_is_collecting(void);
void uart_set_line_callback(uart_line_callback_t callback);

#endif
