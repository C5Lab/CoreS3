#include "gps_module.h"
#include "uart_handler.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "gps_module";

#define GPS_UART_PORT       UART_NUM_2
#define GPS_RX_BUF_SIZE     2048
#define GPS_LINE_MAX        128
#define GPS_FIX_STALE_US    (5LL * 1000 * 1000)   /* 5 s */
/* Loud auto-baud sweeps before we drop to a slow, quiet cadence. Mirrors
 * Tab5's USB-CDC GPS, which simply stays silent when no device is present:
 * if nothing is on G17/G18 we stop flooding the console. */
#define GPS_MAX_SWEEPS      2

/* NVS lives in ui_helpers; mirror the namespace + key there. */
#define GPS_NVS_NAMESPACE   "settings"
#define GPS_NVS_KEY_ENABLE  "ext_gps"

typedef struct {
    double  lat;
    double  lon;
    double  alt;
    int     sats;
    bool    valid;
    int64_t updated_us;
} gps_fix_t;

static gps_fix_t        s_fix;
static SemaphoreHandle_t s_fix_mtx;
static TaskHandle_t     s_task;
static volatile bool    s_running;

/* Auto-baud detection: cycle these until valid NMEA arrives, then lock. */
static const int        k_bauds[] = { GPS_MODULE_BAUD, 115200, 38400, 9600 };
#define GPS_NUM_BAUDS   (sizeof(k_bauds) / sizeof(k_bauds[0]))
static volatile int      s_baud      = GPS_MODULE_BAUD;
static volatile bool     s_locked    = false;
static volatile unsigned s_nmea_cnt  = 0;

/* ================================================================== */
/*  NMEA parsing                                                       */
/* ================================================================== */

/* Convert NMEA ddmm.mmmm / dddmm.mmmm to signed decimal degrees. */
static double nmea_to_deg(const char *field, const char *hemi)
{
    if (!field || !*field) return 0.0;
    double v = atof(field);
    double deg = (double)((int)(v / 100.0));
    double min = v - deg * 100.0;
    double dec = deg + min / 60.0;
    if (hemi && (*hemi == 'S' || *hemi == 'W')) dec = -dec;
    return dec;
}

/* Split a NMEA line into comma-separated fields (in place). */
static int nmea_split(char *line, char *fields[], int max_fields)
{
    int n = 0;
    char *p = line;
    /* strip checksum */
    char *star = strchr(p, '*');
    if (star) *star = '\0';
    while (n < max_fields) {
        fields[n++] = p;
        char *comma = strchr(p, ',');
        if (!comma) break;
        *comma = '\0';
        p = comma + 1;
    }
    return n;
}

static void gps_store_fix(double lat, double lon, double alt, int sats, bool valid)
{
    if (!s_fix_mtx) return;
    xSemaphoreTake(s_fix_mtx, portMAX_DELAY);
    if (valid) {
        s_fix.lat = lat;
        s_fix.lon = lon;
        if (alt != 0.0) s_fix.alt = alt;
        if (sats >= 0)  s_fix.sats = sats;
        s_fix.valid = true;
        s_fix.updated_us = esp_timer_get_time();
    } else {
        s_fix.valid = false;
    }
    xSemaphoreGive(s_fix_mtx);
}

static void gps_parse_line(char *line)
{
    if (line[0] != '$') return;
    /* talker (2 chars) + sentence type (3 chars), e.g. GPGGA / GNRMC */
    const char *type = line + 3;

    char *fields[20];

    if (strncmp(type, "GGA", 3) == 0) {
        int n = nmea_split(line, fields, 20);
        if (n < 10) return;
        /* 2=lat 3=N/S 4=lon 5=E/W 6=fixquality 7=sats 9=alt */
        int quality = atoi(fields[6]);
        int sats = atoi(fields[7]);
        if (quality <= 0) {
            gps_store_fix(0, 0, 0, sats, false);
            return;
        }
        double lat = nmea_to_deg(fields[2], fields[3]);
        double lon = nmea_to_deg(fields[4], fields[5]);
        double alt = atof(fields[9]);
        gps_store_fix(lat, lon, alt, sats, true);
    } else if (strncmp(type, "RMC", 3) == 0) {
        int n = nmea_split(line, fields, 20);
        if (n < 7) return;
        /* 2=status A/V 3=lat 4=N/S 5=lon 6=E/W */
        if (fields[2][0] != 'A') {
            gps_store_fix(0, 0, 0, -1, false);
            return;
        }
        double lat = nmea_to_deg(fields[3], fields[4]);
        double lon = nmea_to_deg(fields[5], fields[6]);
        gps_store_fix(lat, lon, 0, -1, true);
    }
}

/* ================================================================== */
/*  RX task                                                            */
/* ================================================================== */

static void gps_rx_task(void *arg)
{
    (void)arg;
    char line[GPS_LINE_MAX];
    int  pos = 0;
    uint8_t byte;
    int     baud_idx = 0;
    int     sweeps   = 0;   /* completed baud passes with no lock */
    int64_t last_switch_us = esp_timer_get_time();

    ESP_LOGI(TAG, "GPS reader started on UART%d RX=GPIO%d @ %d baud (auto-detect)",
             GPS_UART_PORT, GPS_MODULE_UART_RX_PIN, s_baud);

    while (s_running) {
        int r = uart_read_bytes(GPS_UART_PORT, &byte, 1, pdMS_TO_TICKS(200));
        if (r > 0) {
            if (byte == '\r') {
                /* ignore */
            } else if (byte == '\n') {
                if (pos > 0) {
                    line[pos] = '\0';
                    /* Raw NMEA log only while hunting baud, so we don't flood
                     * the console (and bury wardrive TX/RX) once locked. */
                    if (!s_locked) {
                        ESP_LOGI(TAG, "NMEA: %s", line);
                    }
                    if (line[0] == '$') {
                        s_nmea_cnt++;
                        if (!s_locked) {
                            s_locked = true;
                            ESP_LOGI(TAG, "Locked NMEA at %d baud", s_baud);
                        }
                    }
                    gps_parse_line(line);
                    pos = 0;
                }
            } else if (pos < GPS_LINE_MAX - 1) {
                line[pos++] = (char)byte;
            } else {
                pos = 0; /* overflow: drop line */
            }
        }

        /* Auto-baud: cycle rates until valid NMEA locks. Bound the *loud*
         * phase so a bare UART (nothing wired to G17/G18) doesn't spam the
         * console forever. After GPS_MAX_SWEEPS full passes we keep probing
         * but slowly (~15 s) and at debug level, having logged one warning —
         * the console-quiet analogue of Tab5's "silent when no GPS present". */
        if (!s_locked) {
            int64_t interval = (sweeps < GPS_MAX_SWEEPS) ? 3000000LL : 15000000LL;
            if ((esp_timer_get_time() - last_switch_us) > interval) {
                baud_idx = (baud_idx + 1) % GPS_NUM_BAUDS;
                bool wrapped = (baud_idx == 0);
                if (wrapped && sweeps < 1000000) sweeps++;
                s_baud = k_bauds[baud_idx];
                uart_flush_input(GPS_UART_PORT);
                uart_set_baudrate(GPS_UART_PORT, s_baud);
                pos = 0;
                last_switch_us = esp_timer_get_time();
                if (wrapped && sweeps == GPS_MAX_SWEEPS) {
                    ESP_LOGW(TAG, "No NMEA on UART%d (RX=G%d) after %d sweeps; "
                             "slowing auto-detect. Attach the M5 GPS to G17/G18 "
                             "or disable External GPS in Settings.",
                             GPS_UART_PORT, GPS_MODULE_UART_RX_PIN, GPS_MAX_SWEEPS);
                } else {
                    ESP_LOGD(TAG, "No NMEA yet, trying %d baud", s_baud);
                }
            }
        }
    }

    ESP_LOGI(TAG, "GPS reader stopped");
    s_task = NULL;
    vTaskDelete(NULL);
}

/* ================================================================== */
/*  Public API                                                         */
/* ================================================================== */

bool gps_module_start(void)
{
    if (s_running) return true;

    if (uart_port_mode == UART_PORT_MODE_PORTC) {
        ESP_LOGW(TAG, "Refusing to start: C5 link on Port C uses G17/G18 (pin clash)");
        return false;
    }

    if (!s_fix_mtx) {
        s_fix_mtx = xSemaphoreCreateMutex();
        if (!s_fix_mtx) return false;
    }

    uart_config_t cfg = {
        .baud_rate = GPS_MODULE_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(GPS_UART_PORT, GPS_RX_BUF_SIZE, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return false;
    }
    uart_param_config(GPS_UART_PORT, &cfg);
    gpio_reset_pin(GPS_MODULE_UART_TX_PIN);
    gpio_reset_pin(GPS_MODULE_UART_RX_PIN);
    uart_set_pin(GPS_UART_PORT, GPS_MODULE_UART_TX_PIN, GPS_MODULE_UART_RX_PIN,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    xSemaphoreTake(s_fix_mtx, portMAX_DELAY);
    memset(&s_fix, 0, sizeof(s_fix));
    xSemaphoreGive(s_fix_mtx);

    s_baud     = GPS_MODULE_BAUD;
    s_locked   = false;
    s_nmea_cnt = 0;

    s_running = true;
    if (xTaskCreate(gps_rx_task, "gps_rx", 4096, NULL, 5, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create GPS task");
        s_running = false;
        uart_driver_delete(GPS_UART_PORT);
        return false;
    }

    return true;
}

void gps_module_stop(void)
{
    if (!s_running) return;
    s_running = false;
    /* give the task a moment to exit its read loop, then free the driver */
    vTaskDelay(pdMS_TO_TICKS(250));
    uart_driver_delete(GPS_UART_PORT);
}

bool gps_module_is_running(void)
{
    return s_running;
}

bool gps_module_get_fix(double *lat, double *lon, double *alt, int *sats)
{
    if (!s_fix_mtx) return false;
    bool ok = false;
    xSemaphoreTake(s_fix_mtx, portMAX_DELAY);
    if (s_fix.valid &&
        (esp_timer_get_time() - s_fix.updated_us) < GPS_FIX_STALE_US) {
        if (lat)  *lat  = s_fix.lat;
        if (lon)  *lon  = s_fix.lon;
        if (alt)  *alt  = s_fix.alt;
        if (sats) *sats = s_fix.sats;
        ok = true;
    }
    xSemaphoreGive(s_fix_mtx);
    return ok;
}

int gps_module_get_baud(void)
{
    return s_baud;
}

unsigned gps_module_nmea_count(void)
{
    return s_nmea_cnt;
}

void gps_module_init(void)
{
    /* The enable flag is owned by ui_helpers (NVS). Read it directly to avoid
     * a forced include ordering; ui_helpers loads/saves the same key. */
    extern bool external_gps_enabled;
    if (external_gps_enabled) {
        if (!gps_module_start()) {
            ESP_LOGW(TAG, "External GPS enabled but reader could not start");
        }
    }
}
