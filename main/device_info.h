#ifndef DEVICE_INFO_H
#define DEVICE_INFO_H

/**
 * One-shot query of the firmware's `board_name` CLI command over UART.
 * Must be called once after uart_handler_init(); blocks for up to ~500 ms
 * waiting for the `board_name=...` response, then caches the result.
 */
void device_info_init(void);

/**
 * Returns the board label provisioned in eFuse, or "SubGhz-NoName" if the
 * board reports `<unset>`, the query timed out, or has not been performed.
 * Always returns a non-NULL, NUL-terminated string safe to use as a title.
 */
const char *device_info_subghz_title(void);

#endif
