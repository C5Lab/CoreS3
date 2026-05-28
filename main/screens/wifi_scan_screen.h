#ifndef WIFI_SCAN_SCREEN_H
#define WIFI_SCAN_SCREEN_H

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"

#define MAX_NETWORKS 512

typedef struct {
    uint8_t  index;
    char     ssid[33];
    char     bssid[18];
    uint8_t  channel;
    char     security[24];
    int8_t   rssi;
    char     band[8];
    char     vendor[48];
    bool     mfp_capable;
    bool     mfp_known;
    char     uptime[64];
    bool     selected;
    lv_obj_t *info_label;
} wifi_network_t;

void show_wifi_scan_screen(void);

wifi_network_t *wifi_scan_get_networks(void);
int  wifi_scan_get_network_count(void);
int  wifi_scan_get_selected(int *indices, int max_indices);

#endif
