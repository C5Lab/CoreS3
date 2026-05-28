#ifndef UART_HANDLER_H
#define UART_HANDLER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>


// MBus:   TX=43, RX=44 (default)
// Port C: TX=17, RX=18 (M5Go3 bottom base)
// NOTE: Console must be routed to USB_SERIAL_JTAG (sdkconfig) to free MBus pins!

#define UART_BAUD_RATE        115200
#define UART_PORT             UART_NUM_1
#define UART_MAX_LINE_LEN     512
#define UART_COLLECT_TIMEOUT_MS 25000   /* max wait for end marker */

typedef enum {
    UART_PORT_MODE_MBUS  = 0,   /* TX=43, RX=44 (default) */
    UART_PORT_MODE_PORTC = 1,   /* TX=17, RX=18 (M5Go3 Port C) */
} uart_port_mode_t;

extern uart_port_mode_t uart_port_mode;

void uart_handler_get_pins(uart_port_mode_t mode, int *tx, int *rx);

typedef void (*uart_line_callback_t)(const char *line);
typedef void (*uart_collect_callback_t)(const char **lines, int line_count);

void uart_handler_init(void);
void uart_send_command(const char *cmd);
void uart_start_collect(const char *end_marker, uart_collect_callback_t on_complete);
void uart_stop_collect(void);
bool uart_is_collecting(void);
void uart_set_line_callback(uart_line_callback_t callback);

/** Discard pending bytes in the UART RX driver buffer. */
void uart_handler_flush_rx(void);

/**
 * Send a command and block until a line containing `wait_substr` arrives
 * (or timeout). Copies the matching line into `out` (NUL-terminated).
 * Returns true on success. Must not be called from uart_rx_task.
 */
bool uart_send_wait_line(const char *cmd, const char *wait_substr,
                         char *out, size_t out_sz, int timeout_ms);

/**
 * Block until a line containing `wait_substr` arrives (no command sent).
 */
bool uart_wait_for_line(const char *wait_substr, char *out, size_t out_sz,
                        int timeout_ms);

#endif
