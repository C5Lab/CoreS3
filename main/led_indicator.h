#pragma once

#include <stdint.h>

void led_indicator_init(void);

/* Solid green on the whole strip for 1 second, then off. */
void led_indicator_signal_received(void);

/* Fast red blink until led_indicator_tx_stop() is called. */
void led_indicator_tx_start(void);
void led_indicator_tx_stop(void);

/* Fast red blink for the given duration (ms), then off. */
void led_indicator_tx_pulse(uint32_t ms);
