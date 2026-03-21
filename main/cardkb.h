#ifndef CARDKB_H
#define CARDKB_H

#include "esp_err.h"
#include <stdint.h>

/*
 * CardKB Mini Keyboard V1.1 (MEGA8A)
 * I2C address 0x5F, connected via Grove Port A on CoreS3 SE.
 * Grove Port A pins: SDA = GPIO2, SCL = GPIO1.
 *
 * Reading 1 byte returns the ASCII code of the pressed key,
 * or 0x00 if no key is pressed.
 */

#define CARDKB_I2C_ADDR       0x5F
#define CARDKB_I2C_SDA        GPIO_NUM_2
#define CARDKB_I2C_SCL        GPIO_NUM_1
#define CARDKB_I2C_PORT       I2C_NUM_0
#define CARDKB_I2C_FREQ_HZ    100000

/**
 * Initialise the Grove I2C bus and add the CardKB device.
 */
esp_err_t cardkb_init(void);

/**
 * Read one key from the CardKB.
 * Returns 0 if no key is currently pressed.
 */
uint8_t cardkb_read_key(void);

/**
 * De-initialise (free I2C resources).
 */
esp_err_t cardkb_deinit(void);

#endif
