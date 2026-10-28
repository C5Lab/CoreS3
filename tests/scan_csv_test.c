#include "scan_csv.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void expect(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        failures++;
    }
}

static void expect_eq(const char *got, const char *want, const char *msg)
{
    if (!got || !want || strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL: %s (got \"%s\" want \"%s\")\n",
                msg, got ? got : "(null)", want ? want : "(null)");
        failures++;
    }
}

static void check_plain(void)
{
    scan_csv_network_t net;
    const char *line =
        "\"1\",\"AX3\",\"\",\"C4:2B:44:12:29:20\",\"112\",\"WPA2/WPA3 Mixed\",\"-59\",\"5GHz\"";

    expect(scan_csv_parse_line(line, &net), "plain line parses");
    expect(net.index == 1, "plain index");
    expect_eq(net.ssid, "AX3", "plain ssid");
    expect_eq(net.vendor, "", "plain empty vendor");
    expect_eq(net.bssid, "C4:2B:44:12:29:20", "plain bssid");
    expect(net.channel == 112, "plain channel");
    expect_eq(net.security, "WPA2/WPA3 Mixed", "plain auth");
    expect(net.rssi == -59, "plain rssi");
    expect_eq(net.band, "5GHz", "plain band");
}

static void check_quoted_and_commas(void)
{
    scan_csv_network_t net;
    const char *line =
        "\"1\",\"moja \"\"specjalna\"\" siec\",\"Acme \"\"X\"\" Inc\","
        "\"AA:BB:CC:DD:EE:FF\",\"6\",\"WPA2\",\"-50\",\"2.4GHz\"";
    const char *commas =
        "\"2\",\"moja, \"\"specjalna\"\" siec\",\"Acme, \"\"X\"\" Inc\","
        "\"AA:BB:CC:DD:EE:FF\",\"11\",\"WPA2 \"\"Enterprise\"\"\",\"-40\",\"2.4GHz\"";

    expect(scan_csv_parse_line(line, &net), "quoted line parses");
    expect(net.index == 1, "quoted index");
    expect_eq(net.ssid, "moja \"specjalna\" siec", "quoted ssid");
    expect_eq(net.vendor, "Acme \"X\" Inc", "quoted vendor");
    expect_eq(net.bssid, "AA:BB:CC:DD:EE:FF", "quoted bssid");
    expect(net.channel == 6, "quoted channel");
    expect_eq(net.security, "WPA2", "quoted auth");
    expect(net.rssi == -50, "quoted rssi");
    expect_eq(net.band, "2.4GHz", "quoted band");

    expect(scan_csv_parse_line(commas, &net), "comma line parses");
    expect_eq(net.ssid, "moja, \"specjalna\" siec", "comma ssid");
    expect_eq(net.vendor, "Acme, \"X\" Inc", "comma vendor");
    expect_eq(net.bssid, "AA:BB:CC:DD:EE:FF", "comma bssid");
    expect(net.channel == 11, "comma channel");
    expect_eq(net.security, "WPA2 \"Enterprise\"", "comma auth");
    expect(net.rssi == -40, "comma rssi");
    expect_eq(net.band, "2.4GHz", "comma band");
}

static void check_legacy_vendor_and_edges(void)
{
    scan_csv_network_t net;
    const char *ninth =
        "\"4\",\"Cafe\",\"\",\"AA:BB:CC:DD:EE:FF\",\"6\",\"OPEN\",\"-70\",\"2.4GHz\","
        "\"Acme \"\"X\"\" Inc\"";
    const char *third_wins =
        "\"5\",\"Cafe\",\"Primary\",\"AA:BB:CC:DD:EE:FF\",\"6\",\"WPA2\",\"-41\",\"5GHz\","
        "\"Ignored\"";
    const char *hidden =
        "\"3\",\"\",\"\",\"11:22:33:44:55:66\",\"1\",\"OPEN\",\"-90\",\"2.4GHz\"";
    /* A field whose value is one quote is four quote characters: open, "", close. */
    char only_quote[96];
    {
        const char q = '"';
        snprintf(only_quote, sizeof(only_quote),
                 "%c6%c,%c%c%c%c,%c%c%c%c,%cAA:BB:CC:DD:EE:01%c,"
                 "%c36%c,%cWPA2%c,%c-33%c,%c5GHz%c",
                 q, q, q, q, q, q, q, q, q, q, q, q, q, q, q, q, q, q, q, q);
    }
    char tiny[4];
    char pre[4];
    char out[4];
    char post[4];
    const char *rest;
    char long_vendor[80];
    char line[256];
    int n;

    expect(scan_csv_parse_line(ninth, &net), "legacy 9th vendor parses");
    expect_eq(net.ssid, "Cafe", "legacy ssid");
    expect_eq(net.vendor, "Acme \"X\" Inc", "legacy vendor");
    expect_eq(net.bssid, "AA:BB:CC:DD:EE:FF", "legacy bssid");
    expect(net.channel == 6 && net.rssi == -70, "legacy channel/rssi");
    expect_eq(net.band, "2.4GHz", "legacy band");

    expect(scan_csv_parse_line(third_wins, &net), "third-field vendor wins");
    expect_eq(net.vendor, "Primary", "third field kept");

    expect(scan_csv_parse_line(hidden, &net), "hidden ssid parses");
    expect_eq(net.ssid, "", "hidden ssid empty");
    expect_eq(net.vendor, "", "hidden vendor empty");
    expect(net.channel == 1 && net.rssi == -90, "hidden channel/rssi");

    expect(scan_csv_parse_line(only_quote, &net), "quote-only fields parse");
    expect_eq(net.ssid, "\"", "ssid is one quote");
    expect_eq(net.vendor, "\"", "vendor is one quote");
    expect_eq(net.bssid, "AA:BB:CC:DD:EE:01", "quote-only bssid");

    expect(!scan_csv_parse_line(NULL, &net), "null line rejected");
    expect(!scan_csv_parse_line("\"1\",\"x\"", NULL), "null out rejected");
    expect(!scan_csv_parse_line("Scan results printed.", &net), "marker rejected");
    expect(!scan_csv_parse_line("\"1\",\"unterminated", &net), "unterminated rejected");
    expect(!scan_csv_parse_field(NULL, tiny, sizeof(tiny)), "null field rejected");
    expect(!scan_csv_parse_field("\"abc\"", NULL, 4), "null buffer rejected");
    expect(!scan_csv_parse_field("\"abc\"", tiny, 0), "zero buffer rejected");

    memset(pre, 0xA5, sizeof(pre));
    memset(out, 0xA5, sizeof(out));
    memset(post, 0xA5, sizeof(post));
    rest = scan_csv_parse_field("\"0123456789\",\"next\"", out, sizeof(out));
    expect(rest != NULL, "truncated field still consumed");
    expect_eq(rest, "\"next\"", "truncation leaves next field");
    expect_eq(out, "012", "truncated value");
    expect(memcmp(pre, "\xA5\xA5\xA5\xA5", 4) == 0, "canary before");
    expect(memcmp(post, "\xA5\xA5\xA5\xA5", 4) == 0, "canary after");

    char long_ssid[80];
    memset(long_ssid, 'S', 50);
    long_ssid[10] = '"';
    long_ssid[11] = '"';
    long_ssid[50] = '\0';
    n = snprintf(line, sizeof(line),
                 "\"8\",\"%s\",\"Vendor\",\"AA:BB:CC:DD:EE:FF\",\"7\",\"WPA2\",\"-8\",\"5GHz\"",
                 long_ssid);
    expect(n > 0 && n < (int)sizeof(line), "long ssid line fits");
    expect(scan_csv_parse_line(line, &net), "long ssid line parses");
    expect(strlen(net.ssid) == sizeof(net.ssid) - 1, "ssid truncated to buffer");
    expect(net.ssid[10] == '"', "truncated ssid kept embedded quote");
    expect_eq(net.vendor, "Vendor", "long ssid vendor");
    expect_eq(net.bssid, "AA:BB:CC:DD:EE:FF", "long ssid bssid");
    expect(net.channel == 7 && net.rssi == -8, "long ssid channel/rssi");
    expect_eq(net.band, "5GHz", "long ssid band");

    memset(long_vendor, 'V', 60);
    long_vendor[20] = '"';
    long_vendor[21] = '"';
    long_vendor[60] = '\0';
    n = snprintf(line, sizeof(line),
                 "\"9\",\"ssid\",\"%s\",\"AA:BB:CC:DD:EE:FF\",\"6\",\"WPA2\",\"-12\",\"2.4GHz\"",
                 long_vendor);
    expect(n > 0 && n < (int)sizeof(line), "long vendor line fits");
    expect(scan_csv_parse_line(line, &net), "long vendor line parses");
    expect_eq(net.ssid, "ssid", "long vendor ssid");
    expect(strlen(net.vendor) == sizeof(net.vendor) - 1, "vendor truncated to buffer");
    expect(net.vendor[20] == '"', "truncated vendor kept embedded quote");
    expect_eq(net.bssid, "AA:BB:CC:DD:EE:FF", "long vendor bssid");
    expect(net.channel == 6 && net.rssi == -12, "long vendor channel/rssi");
    expect_eq(net.band, "2.4GHz", "long vendor band");
}

int main(void)
{
    check_plain();
    check_quoted_and_commas();
    check_legacy_vendor_and_edges();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("scan csv PASS");
    return 0;
}
