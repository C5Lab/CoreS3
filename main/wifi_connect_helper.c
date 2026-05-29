#include "wifi_connect_helper.h"
#include "uart_handler.h"
#include "ui_helpers.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "wifi_conn";

static volatile bool s_evil_collecting;
static char s_evil_target_ssid[33];
static char s_evil_found_pass[64];

static volatile int s_connect_result;

static void evil_lookup_line_cb(const char *line)
{
    if (!s_evil_collecting || !line || line[0] != '"') return;

    const char *ssid_start = line + 1;
    const char *ssid_end = strchr(ssid_start, '"');
    if (!ssid_end) return;

    char found_ssid[33];
    size_t slen = (size_t)(ssid_end - ssid_start);
    if (slen >= sizeof(found_ssid)) slen = sizeof(found_ssid) - 1;
    memcpy(found_ssid, ssid_start, slen);
    found_ssid[slen] = '\0';

    if (strcmp(found_ssid, s_evil_target_ssid) != 0) return;

    const char *sep = ssid_end + 1;
    while (*sep == ',' || *sep == ' ') sep++;
    if (*sep != '"') return;

    const char *pass_start = sep + 1;
    const char *pass_end = strchr(pass_start, '"');
    if (!pass_end) return;

    size_t plen = (size_t)(pass_end - pass_start);
    if (plen >= sizeof(s_evil_found_pass)) plen = sizeof(s_evil_found_pass) - 1;
    memcpy(s_evil_found_pass, pass_start, plen);
    s_evil_found_pass[plen] = '\0';
}

static void connect_line_cb(const char *line)
{
    if (!line || s_connect_result != 0) return;
    /* Getting a DHCP lease unambiguously means we are connected; some firmware
     * builds emit "DHCP IP: ..." without a separate "SUCCESS" token. */
    if (strstr(line, "SUCCESS") || strstr(line, "DHCP IP"))
        s_connect_result = 1;
    else if (strstr(line, "FAILED") || strstr(line, "TIMEOUT") ||
             strstr(line, "Error"))
        s_connect_result = -1;
}

bool wifi_network_is_open(const wifi_network_t *net)
{
    if (!net) return false;
    return (strstr(net->security, "OPEN") != NULL || net->security[0] == '\0');
}

void wifi_escape_quoted_arg(const char *in, char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return;
    if (!in) { out[0] = '\0'; return; }

    size_t j = 0;
    for (size_t i = 0; in[i] && j + 2 < out_sz; i++) {
        if (in[i] == '\\' || in[i] == '"') {
            if (j + 3 >= out_sz) break;
            out[j++] = '\\';
        }
        out[j++] = in[i];
    }
    out[j] = '\0';
}

bool wifi_lookup_evil_password(const char *ssid, char *pass, size_t pass_sz)
{
    if (!ssid || !pass || pass_sz == 0) return false;
    pass[0] = '\0';

    strncpy(s_evil_target_ssid, ssid, sizeof(s_evil_target_ssid) - 1);
    s_evil_target_ssid[sizeof(s_evil_target_ssid) - 1] = '\0';
    s_evil_found_pass[0] = '\0';
    s_evil_collecting = true;

    uart_set_line_callback(evil_lookup_line_cb);
    uart_handler_flush_rx();
    uart_send_command("show_pass evil");

    int waited = 0;
    while (s_evil_collecting && waited < 800) {
        vTaskDelay(pdMS_TO_TICKS(50));
        waited += 50;
        if (s_evil_found_pass[0]) break;
    }

    s_evil_collecting = false;
    uart_set_line_callback(NULL);

    if (s_evil_found_pass[0]) {
        strncpy(pass, s_evil_found_pass, pass_sz - 1);
        pass[pass_sz - 1] = '\0';
        ESP_LOGI(TAG, "Found evil-twin password for %s", ssid);
        return true;
    }
    return false;
}

bool wifi_connect_sync(const wifi_network_t *net, const char *password, int timeout_ms)
{
    if (!net) return false;

    char esc_ssid[67];
    char esc_pass[131];
    wifi_escape_quoted_arg(net->ssid, esc_ssid, sizeof(esc_ssid));

    char cmd[256];
    if (wifi_network_is_open(net) || !password || password[0] == '\0') {
        snprintf(cmd, sizeof(cmd), "wifi_connect \"%s\"", esc_ssid);
    } else {
        wifi_escape_quoted_arg(password, esc_pass, sizeof(esc_pass));
        snprintf(cmd, sizeof(cmd), "wifi_connect \"%s\" \"%s\"", esc_ssid, esc_pass);
    }

    s_connect_result = 0;
    uart_set_line_callback(connect_line_cb);
    uart_handler_flush_rx();
    uart_send_command(cmd);

    int waited = 0;
    while (s_connect_result == 0 && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(100));
        waited += 100;
    }

    uart_set_line_callback(NULL);
    return s_connect_result == 1;
}

int wifi_parse_list_hosts_line(const char *line, lan_host_t *host)
{
    if (!line || !host || strstr(line, "->") == NULL) return 0;

    const char *p = line;
    while (*p == ' ') p++;

    int i = 0;
    while (p[i] && p[i] != ' ' && i < (int)sizeof(host->ip) - 1) {
        host->ip[i] = p[i];
        i++;
    }
    host->ip[i] = '\0';
    if (i == 0) return 0;

    const char *arrow = strstr(line, "->");
    if (!arrow) return 0;
    arrow += 2;
    while (*arrow == ' ') arrow++;

    i = 0;
    while (arrow[i] && arrow[i] != ' ' && arrow[i] != '\r' && arrow[i] != '\n'
           && i < (int)sizeof(host->mac) - 1) {
        host->mac[i] = arrow[i];
        i++;
    }
    host->mac[i] = '\0';
    return (host->ip[0] && host->mac[0]) ? 1 : 0;
}

typedef struct {
    lan_host_t *out;
    int max;
    int count;
    volatile bool active;
    volatile bool got_header;
    int idle_ms;
} hosts_collect_t;

static hosts_collect_t s_hosts;

static void hosts_line_cb(const char *line)
{
    if (!s_hosts.active || !line) return;

    if (strstr(line, "Discovered Hosts") != NULL) {
        s_hosts.got_header = true;
        s_hosts.idle_ms = 0;
        return;
    }

    if (!s_hosts.got_header || s_hosts.count >= s_hosts.max) return;

    lan_host_t h;
    memset(&h, 0, sizeof(h));
    if (wifi_parse_list_hosts_line(line, &h)) {
        s_hosts.out[s_hosts.count++] = h;
        s_hosts.idle_ms = 0;
    }
}

int wifi_collect_list_hosts(lan_host_t *out, int max_hosts, int idle_timeout_ms)
{
    if (!out || max_hosts <= 0) return 0;

    memset(&s_hosts, 0, sizeof(s_hosts));
    s_hosts.out   = out;
    s_hosts.max   = max_hosts;
    s_hosts.active = true;

    uart_set_line_callback(hosts_line_cb);
    uart_handler_flush_rx();
    uart_send_command("list_hosts");

    /* The firmware runs a subnet discovery sweep that can take several seconds
     * before it prints the "Discovered Hosts" header, so allow a generous wall
     * clock before giving up on the header. After the header arrives we only
     * wait `idle_timeout_ms` past the last parsed host. */
    const int header_timeout_ms = 30000;
    int elapsed = 0;
    while (s_hosts.active && elapsed < header_timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(50));
        elapsed += 50;
        s_hosts.idle_ms += 50;
        if (s_hosts.got_header && s_hosts.idle_ms >= idle_timeout_ms)
            break;
    }

    s_hosts.active = false;
    uart_set_line_callback(NULL);
    ESP_LOGI(TAG, "list_hosts: %d hosts", s_hosts.count);
    return s_hosts.count;
}

typedef struct {
    wifi_network_t net;
    char password[64];
    bool has_pass;
    wifi_connect_done_cb_t cb;
    void *user_data;
} async_connect_ctx_t;

typedef struct {
    bool ok;
    wifi_connect_done_cb_t cb;
    void *user_data;
} async_connect_result_t;

static void async_connect_lvgl_cb(void *arg)
{
    async_connect_result_t *r = (async_connect_result_t *)arg;
    if (r && r->cb) r->cb(r->ok, r->user_data);
    free(r);
}

static void async_connect_task(void *arg)
{
    async_connect_ctx_t *ctx = (async_connect_ctx_t *)arg;
    char pass[64] = {0};
    const char *use_pass = NULL;

    if (!wifi_network_is_open(&ctx->net)) {
        if (ctx->has_pass && ctx->password[0]) {
            use_pass = ctx->password;
        } else if (wifi_lookup_evil_password(ctx->net.ssid, pass, sizeof(pass))) {
            use_pass = pass;
        } else if (ctx->password[0]) {
            use_pass = ctx->password;
        }
    }

    bool ok = wifi_connect_sync(&ctx->net, use_pass, 15000);
    wifi_connect_done_cb_t cb = ctx->cb;
    void *ud = ctx->user_data;
    free(ctx);

    async_connect_result_t *r = calloc(1, sizeof(*r));
    if (r) {
        r->ok = ok;
        r->cb = cb;
        r->user_data = ud;
        if (!ui_lvgl_async_call(async_connect_lvgl_cb, r))
            async_connect_lvgl_cb(r);
    } else if (cb) {
        cb(ok, ud);
    }

    vTaskDelete(NULL);
}

void wifi_connect_async(const wifi_network_t *net, const char *password,
                        wifi_connect_done_cb_t cb, void *user_data)
{
    async_connect_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx || !net) return;
    ctx->net = *net;
    ctx->cb  = cb;
    ctx->user_data = user_data;
    if (password && password[0]) {
        strncpy(ctx->password, password, sizeof(ctx->password) - 1);
        ctx->has_pass = true;
    }
    xTaskCreate(async_connect_task, "wifi_conn", 8192, ctx, 5, NULL);
}
