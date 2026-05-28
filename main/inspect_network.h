#ifndef INSPECT_NETWORK_H
#define INSPECT_NETWORK_H

#include "wifi_scan_screen.h"

/** Start background inspect_network task for all scanned networks. */
void wifi_inspect_start(wifi_network_t *networks, int count);

/** Cooperatively stop inspect task (wait up to ~250 ms). */
void wifi_inspect_cancel(void);

#endif
