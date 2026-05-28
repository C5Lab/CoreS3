#ifndef WIFI_CONNECT_HELPER_H
#define WIFI_CONNECT_HELPER_H

#include "wifi_scan_screen.h"
#include <stddef.h>
#include <stdbool.h>

#define WIFI_MAX_HOSTS 64

typedef struct {
    char ip[16];
    char mac[18];
} lan_host_t;

typedef void (*wifi_connect_done_cb_t)(bool success, void *user_data);

bool wifi_network_is_open(const wifi_network_t *net);

/** Escape SSID/password for quoted CLI arguments. */
void wifi_escape_quoted_arg(const char *in, char *out, size_t out_sz);

/** Look up password from `show_pass evil` output. Blocks up to ~800 ms. */
bool wifi_lookup_evil_password(const char *ssid, char *pass, size_t pass_sz);

/**
 * Build and send wifi_connect; wait for SUCCESS/FAILED/TIMEOUT.
 * password may be NULL for open networks.
 */
bool wifi_connect_sync(const wifi_network_t *net, const char *password, int timeout_ms);

/**
 * Send list_hosts and collect hosts until idle timeout.
 * Returns number of hosts parsed.
 */
int wifi_collect_list_hosts(lan_host_t *out, int max_hosts, int idle_timeout_ms);

/** Parse collected UART lines for host entries. */
int wifi_parse_list_hosts_line(const char *line, lan_host_t *host);

/**
 * Start async connect from a background task; invokes cb on LVGL thread.
 * If password is NULL and network is secured, tries evil twin DB first.
 */
void wifi_connect_async(const wifi_network_t *net, const char *password,
                        wifi_connect_done_cb_t cb, void *user_data);

#endif
