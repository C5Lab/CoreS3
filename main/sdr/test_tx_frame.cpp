// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_tx_frame.hpp"
#include <cstring>

std::size_t tx_test_build_probe(uint8_t *frame, std::size_t capacity,
                                const uint8_t source_mac[6], unsigned channel,
                                uint16_t test_sequence,
                                const uint8_t random_payload[TX_TEST_RANDOM_BYTES]) {
    if (!frame || !source_mac || !random_payload || capacity < TX_TEST_FRAME_BYTES ||
        channel < 1 || channel > 11) return 0;

    std::memset(frame, 0, TX_TEST_FRAME_BYTES);
    frame[0] = 0x40; // Probe Request management subtype, no protected/deauth bits.
    std::memset(frame + 4, 0xff, 6);       // destination: broadcast
    std::memcpy(frame + 10, source_mac, 6); // source: device station MAC
    std::memset(frame + 16, 0xff, 6);      // BSSID: broadcast
    // Sequence control is intentionally zero; esp_wifi_80211_tx() replaces it.

    std::size_t at = 24;
    static constexpr char ssid[] = "SDR32-TEST";
    frame[at++] = 0; // SSID IE
    frame[at++] = sizeof(ssid) - 1;
    std::memcpy(frame + at, ssid, sizeof(ssid) - 1);
    at += sizeof(ssid) - 1;

    static constexpr uint8_t rates[] = {0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24};
    frame[at++] = 1; // Supported Rates IE
    frame[at++] = sizeof(rates);
    std::memcpy(frame + at, rates, sizeof(rates));
    at += sizeof(rates);

    static constexpr uint8_t extended_rates[] = {0x30, 0x48, 0x60, 0x6c};
    frame[at++] = 50; // Extended Supported Rates IE
    frame[at++] = sizeof(extended_rates);
    std::memcpy(frame + at, extended_rates, sizeof(extended_rates));
    at += sizeof(extended_rates);

    frame[at++] = 3; // DS Parameter Set IE
    frame[at++] = 1;
    frame[at++] = static_cast<uint8_t>(channel);

    frame[at++] = 221; // Vendor Specific IE
    frame[at++] = 3 + 1 + 2 + TX_TEST_RANDOM_BYTES;
    // The OUI matches the actual device vendor prefix instead of impersonating
    // a different organization. Type 0x53 is local to this test format.
    std::memcpy(frame + at, source_mac, 3);
    at += 3;
    frame[at++] = 0x53;
    frame[at++] = static_cast<uint8_t>(test_sequence);
    frame[at++] = static_cast<uint8_t>(test_sequence >> 8);
    std::memcpy(frame + at, random_payload, TX_TEST_RANDOM_BYTES);
    at += TX_TEST_RANDOM_BYTES;
    return at == TX_TEST_FRAME_BYTES ? at : 0;
}
