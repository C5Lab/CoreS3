#ifndef GPS_MODULE_H
#define GPS_MODULE_H

#include <stdbool.h>

/*
 * On-CoreS3 driver for the M5Stack GPS Module v2.1 (MAX-M10S, NMEA over UART).
 *
 * The module stacks on the CoreS3 M5-Bus. With its DIP switch at the CoreS3
 * default mapping the GNSS lines are G17 (module TX) / G18 (module RX), so the
 * host reads NMEA on GPIO18. We use a dedicated UART (UART_NUM_2) for this,
 * leaving UART_NUM_1 (the JanOS C5 link on MBus G43/G44) untouched.
 *
 * IMPORTANT: G17/G18 are the same pins as the optional "Port C" mode of the C5
 * link. The GPS reader therefore refuses to start when uart_port_mode is
 * UART_PORT_MODE_PORTC (pin clash). Keep the C5 link on MBus, or move the GPS
 * DIP switch to a free group (e.g. TX=G13, RX=G7).
 */

#define GPS_MODULE_UART_RX_PIN  18   /* host RX  <- module TX (G17) */
#define GPS_MODULE_UART_TX_PIN  17   /* host TX  -> module RX (G18); config only */
/* First baud tried. The reader auto-cycles common rates until valid NMEA is
 * seen, so this is only the starting guess. MAX-M10S factory default is 9600;
 * the M5 CoreS3 Arduino example uses 115200. */
#define GPS_MODULE_BAUD         9600

/** Read persisted enable flag and, if set (and pins are free), start the reader. */
void gps_module_init(void);

/**
 * Start the NMEA reader on UART_NUM_2.
 * Returns false if the pins clash with the C5 Port-C link, or on driver error.
 */
bool gps_module_start(void);

/** Stop the reader and release UART_NUM_2. Safe to call when not running. */
void gps_module_stop(void);

/** True while the UART reader task is active. */
bool gps_module_is_running(void);

/**
 * Latest valid fix. Returns false if no fix yet or the last fix is stale
 * (older than ~5 s). Any out pointer may be NULL.
 */
bool gps_module_get_fix(double *lat, double *lon, double *alt, int *sats);

/** Baud rate currently used by the reader (may change during auto-detect). */
int gps_module_get_baud(void);

/** Number of NMEA ($...) lines received so far (diagnostic). */
unsigned gps_module_nmea_count(void);

#endif
