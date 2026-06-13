#ifndef IR_DB_H
#define IR_DB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Number of IR signals embedded in the database (TV Power codes). */
size_t ir_db_count(void);

/**
 * Fetch one signal by index.
 *
 * On success, *timings_out points to a freshly allocated uint32_t array of
 * *count_out durations (microseconds, alternating, index 0 = space). The
 * caller owns it and must free() it.
 *
 * @return true on success, false on bad index / allocation failure.
 */
bool ir_db_get(size_t index, uint32_t *carrier_hz, uint8_t *duty_pct,
               uint32_t **timings_out, uint16_t *count_out);

#endif /* IR_DB_H */
