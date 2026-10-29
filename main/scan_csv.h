#ifndef SCAN_CSV_H
#define SCAN_CSV_H

#include <stddef.h>
#include <stdbool.h>

/* scan_networks / show_scan_results line:
 *   "index","SSID","vendor","BSSID","channel","auth","RSSI","band"
 *
 * RFC 4180: each field has one pair of enclosing quotes. A quote inside a
 * field is written as "". Fields (SSID, vendor) may contain commas.
 * When the vendor field is empty, an optional 9th quoted field is the
 * vendor (older firmware appends it there).
 *
 * Buffer sizes match wifi_network_t / obs_network_t. A field longer than
 * the destination is truncated and still consumed, so later fields stay
 * aligned and the destination is always NUL-terminated.
 */
typedef struct {
    int  index;
    char ssid[33];
    char vendor[48];
    char bssid[18];
    int  channel;
    char security[24];
    int  rssi;
    char band[8];
} scan_csv_network_t;

/* Read one quoted field at p. On success returns the pointer just past
 * the field and a following comma, if any. Returns NULL if p does not
 * start a terminated quoted field. out may be shorter than the field. */
const char *scan_csv_parse_field(const char *p, char *out, size_t out_sz);

bool scan_csv_parse_line(const char *line, scan_csv_network_t *net);

#endif
