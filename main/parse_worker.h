#pragma once

#include <stdbool.h>

typedef void (*parse_worker_fn_t)(void *arg);

void parse_worker_init(void);

/**
 * Run fn(arg) on a dedicated FreeRTOS task (Core 0, stack in PSRAM).
 * Safe to call from uart_rx or other high-priority contexts; keeps UART
 * handler thin and schedules LVGL work via ui_lvgl_async_call from the job.
 */
bool parse_worker_post(parse_worker_fn_t fn, void *arg);
