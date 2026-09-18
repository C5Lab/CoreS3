#ifndef NFC_EMULATE_SCREEN_H
#define NFC_EMULATE_SCREEN_H

#include <stdbool.h>

/**
 * Start NFC-A emulation from the already-loaded card (`start_nfc_emulate`
 * with no arg). `idx` is kept so Back can return to the same detail screen.
 */
void show_nfc_emulate_screen(int idx);

/** Optional type/data hint for emulate capability copy fallback. Call before show. */
void nfc_emulate_set_card_hint(bool have_data, const char *type);

#endif
