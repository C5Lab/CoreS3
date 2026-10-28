#include "scan_csv.h"

#include <stdlib.h>
#include <string.h>

const char *scan_csv_parse_field(const char *p, char *out, size_t out_sz)
{
    size_t i = 0;

    if (!p || !out || out_sz == 0 || *p != '"')
        return NULL;

    p++;
    while (*p) {
        if (*p == '"') {
            if (p[1] == '"') {
                if (i + 1 < out_sz)
                    out[i++] = '"';
                p += 2;
                continue;
            }
            out[i] = '\0';
            p++;
            if (*p == ',')
                p++;
            return p;
        }
        if (i + 1 < out_sz)
            out[i++] = *p;
        p++;
    }

    out[i] = '\0';
    return NULL;
}

bool scan_csv_parse_line(const char *line, scan_csv_network_t *net)
{
    const char *p;
    char field[32];

    if (!line || !net || line[0] != '"')
        return false;

    memset(net, 0, sizeof(*net));
    p = line;

    p = scan_csv_parse_field(p, field, sizeof(field));
    if (!p)
        return false;
    net->index = atoi(field);

    p = scan_csv_parse_field(p, net->ssid, sizeof(net->ssid));
    if (!p)
        return false;
    p = scan_csv_parse_field(p, net->vendor, sizeof(net->vendor));
    if (!p)
        return false;
    p = scan_csv_parse_field(p, net->bssid, sizeof(net->bssid));
    if (!p)
        return false;

    p = scan_csv_parse_field(p, field, sizeof(field));
    if (!p)
        return false;
    net->channel = atoi(field);

    p = scan_csv_parse_field(p, net->security, sizeof(net->security));
    if (!p)
        return false;

    p = scan_csv_parse_field(p, field, sizeof(field));
    if (!p)
        return false;
    net->rssi = atoi(field);

    p = scan_csv_parse_field(p, net->band, sizeof(net->band));
    if (!p)
        return false;

    /* Legacy firmware left vendor empty and appended it as field 9. */
    if (net->vendor[0] == '\0' && *p == '"') {
        char legacy[sizeof(net->vendor)];
        const char *next = scan_csv_parse_field(p, legacy, sizeof(legacy));
        if (next && legacy[0] != '\0')
            memcpy(net->vendor, legacy, sizeof(net->vendor));
    }
    return true;
}
