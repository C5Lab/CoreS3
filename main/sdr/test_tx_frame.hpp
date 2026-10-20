// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

constexpr std::size_t TX_TEST_RANDOM_BYTES = 32;
constexpr std::size_t TX_TEST_FRAME_BYTES = 95;

// Build a directed, standards-shaped Probe Request. The SSID is deliberately
// non-wildcard so nearby access points do not all answer. Only the vendor IE
// payload is random; the management header remains valid and non-disruptive.
std::size_t tx_test_build_probe(uint8_t *frame, std::size_t capacity,
                                const uint8_t source_mac[6], unsigned channel,
                                uint16_t test_sequence,
                                const uint8_t random_payload[TX_TEST_RANDOM_BYTES]);
