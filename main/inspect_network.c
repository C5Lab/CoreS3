#include "inspect_network.h"
#include "uart_handler.h"
#include "ui_helpers.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "inspect";

static volatile bool inspect_active = false;
static TaskHandle_t inspect_task   = NULL;

static bool extract_inspect_kv(const char *line, const char *key,
                               char *out, size_t out_sz)
{
    if (!line || !key || !out || out_sz == 0) return false;
    const char *p = strstr(line, key);
    if (!p) return false;
    p += strlen(key);
    size_t i = 0;
    while (*p && *p != ' ' && *p != '\r' && *p != '\n' && i + 1 < out_sz) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return i > 0;
}

static bool extract_inspect_uptime(const char *line, char *out, size_t out_sz)
{
    if (!line || !out || out_sz == 0) return false;
    const char *start = strstr(line, "uptime_str=");
    if (!start) return false;
    start += strlen("uptime_str=");

    const char *end = start;
    const char *probe = start;
    while (*probe) {
        if (*probe == ' ') {
            const char *q = probe + 1;
            while (*q && *q != ' ' && *q != '=') q++;
            if (*q == '=') {
                end = probe;
                break;
            }
        }
        probe++;
    }
    if (end == start) {
        end = start + strlen(start);
        while (end > start && (end[-1] == '\r' || end[-1] == '\n')) end--;
    }

    size_t len = (size_t)(end - start);
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    return len > 0;
}

static void update_info_label(wifi_network_t *net)
{
    if (!net || !net->info_label || !lv_obj_is_valid(net->info_label))
        return;

    const char *mfp = net->mfp_known
        ? (net->mfp_capable ? "MFP On" : "MFP Off")
        : "MFP ?";

    char up_disp[16];
    if (net->uptime[0]) {
        strncpy(up_disp, net->uptime, sizeof(up_disp) - 1);
        up_disp[sizeof(up_disp) - 1] = '\0';
    } else {
        up_disp[0] = '?';
        up_disp[1] = '\0';
    }

    char buf[128];
    if (net->vendor[0])
        snprintf(buf, sizeof(buf),
                 "%ddBm ch%d %s %s\n%s | Up: %s | %s",
                 net->rssi, net->channel, net->band, net->security,
                 mfp, up_disp, net->vendor);
    else
        snprintf(buf, sizeof(buf),
                 "%ddBm ch%d %s %s\n%s | Up: %s",
                 net->rssi, net->channel, net->band, net->security,
                 mfp, up_disp);
    lv_label_set_text(net->info_label, buf);
}

static void inspect_networks_task(void *arg)
{
    int count = (int)(intptr_t)arg;
    ESP_LOGI(TAG, "inspect task started for %d networks", count);

    char line[512];

    for (int i = 0; inspect_active && i < count; i++) {
        wifi_network_t *net = wifi_scan_get_networks();
        if (!net) break;

        char cmd[40];
        snprintf(cmd, sizeof(cmd), "inspect_network %d", net[i].index);

        bool got = uart_send_wait_line(cmd, "[INSPECT]", line, sizeof(line), 1500);
        if (!inspect_active) break;

        if (got) {
            char mfp_val[8] = {0};
            if (extract_inspect_kv(line, "mfp_capable=", mfp_val, sizeof(mfp_val))) {
                net[i].mfp_capable = (mfp_val[0] == '1');
                net[i].mfp_known   = true;
            }
            extract_inspect_uptime(line, net[i].uptime, sizeof(net[i].uptime));

            if (ui_display_lock_wait()) {
                update_info_label(&net[i]);
                ui_display_unlock_safe();
            }
        } else {
            ESP_LOGD(TAG, "inspect %d: no response", net[i].index);
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }

    ESP_LOGI(TAG, "inspect task finished");
    inspect_task = NULL;
    vTaskDelete(NULL);
}

void wifi_inspect_cancel(void)
{
    if (!inspect_task && !inspect_active) return;
    inspect_active = false;
    for (int w = 0; w < 25 && inspect_task != NULL; w++)
        vTaskDelay(pdMS_TO_TICKS(10));
}

void wifi_inspect_start(wifi_network_t *networks, int count)
{
    (void)networks;
    wifi_inspect_cancel();

    if (count <= 0) return;

    inspect_active = true;
    BaseType_t ok = xTaskCreate(inspect_networks_task, "inspect_net",
                                8192, (void *)(intptr_t)count,
                                4, &inspect_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create inspect task");
        inspect_active = false;
        inspect_task   = NULL;
    }
}
