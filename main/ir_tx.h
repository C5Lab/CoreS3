#ifndef IR_TX_H
#define IR_TX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* IR transmit LED of the M5GO Battery Bottom3.
 * M5-Bus pin 22 -> CoreS3 GPIO7 (GPIO5 is taken by the WS2812 strip). */
#define IR_TX_GPIO 7

/**
 * Lazily create the RMT TX channel + copy encoder for IR.
 * Safe to call multiple times; only initializes once.
 */
esp_err_t ir_tx_init(void);

/**
 * Transmit one IR signal, blocking until the burst has finished.
 *
 * @param timings_us  Alternating durations in microseconds. Index 0 is a
 *                    space (carrier off), index 1 a mark (carrier on), etc.
 * @param count       Number of timings.
 * @param carrier_hz  Carrier frequency (e.g. 38000, 36000, 40000).
 * @param duty_pct    Carrier duty cycle in percent (e.g. 33).
 */
void ir_tx_send(const uint32_t *timings_us, size_t count, uint32_t carrier_hz, uint8_t duty_pct);

#endif /* IR_TX_H */
