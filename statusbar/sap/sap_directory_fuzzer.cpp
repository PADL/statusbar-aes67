// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// libFuzzer harness for SapDirectory::handle_packet (parse_sap_packet, sdp::parse, updates, deletions, eviction and
/// expiry) and, on what parses, origin_line_of() and sdp::to_string(). None may crash; the directory stays bounded.

#include "statusbar/sap/sap.hpp"
#include "statusbar/sap/sap_receiver.hpp"
#include "statusbar/sdp/sdp.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

using namespace statusbar;

namespace {

/// Each input 400 ms after the last: re-announcements, deletions and timeouts all happen.
constexpr int64_t STEP_NS = 400'000'000;

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    static sap::SapDirectory directory{sap::SapDirectory::DEFAULT_TIMEOUT_NS};
    static int64_t now_ns = 0;
    now_ns += STEP_NS;
    std::span<uint8_t const> const input{data, size};
    auto const message = sap::parse_sap_packet(input);
    bool const forged = size % 7 == 0;  // now and then from another host than its origin field names
    (void)directory.handle_packet(input, message && !forged ? message->origin : statusbar::ip::IPv4Address{10, 0, 0, 9}, now_ns);
    if (message) {
        (void)sap::origin_line_of(message->sdp);
        if (auto const session = sdp::parse(message->sdp)) {
            (void)sdp::to_string(*session);
        }
    }
    if ((now_ns / STEP_NS) % 1000 == 0) {
        (void)directory.expire(now_ns);
    }
    if (directory.entries().size() > sap::SapDirectory::MAX_SESSIONS) {
        std::abort();
    }
    return 0;
}
