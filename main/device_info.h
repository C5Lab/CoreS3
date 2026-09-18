#ifndef DEVICE_INFO_H
#define DEVICE_INFO_H

#include <stdbool.h>

/**
 * One-shot query of the firmware's `board_name` CLI command over UART.
 * Must be called once after uart_handler_init(); blocks for up to ~500 ms
 * waiting for the `board_name=...` response, then caches the result.
 * Also probes `subghz_status`, `init_nfc`, and (when NFC is present) `nfc_list`.
 */
void device_info_init(void);

/**
 * Returns the board label provisioned in eFuse, or "SubGhz-NoName" if the
 * board reports `<unset>`, the query timed out, or has not been performed.
 * Always returns a non-NULL, NUL-terminated string safe to use as a title.
 */
const char *device_info_subghz_title(void);

/** True if firmware responded to subghz_status with [SUBGHZ_STATUS]. */
bool device_info_has_subghz(void);

/** True if init_nfc reported [NFC] detected (boot probe, hub, or Settings). */
bool device_info_has_nfc(void);

/** Update the cached NFC-present flag after a later init_nfc probe. */
void device_info_set_has_nfc(bool has);

/** Number of saved .nfc cards on SD at boot (from nfc_list); 0 if none/unavailable. */
int device_info_nfc_card_count(void);

/** True if boot nfc_list reported at least one card on SD. */
bool device_info_has_nfc_cards(void);

#endif
