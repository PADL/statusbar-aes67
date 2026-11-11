// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// libFuzzer harness for rtp::parse_rtp on a UDP payload, as the socket transport hands it over. It may not crash or
/// read out of bounds; the payload view it returns must lie inside the input.

#include "padl/rtp/rtp_header.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

using namespace statusbar;

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    std::span<uint8_t const> const input{data, size};
    auto const packet = padl::rtp::parse_rtp(input);
    if (!packet || packet->payload.empty()) {
        return 0;
    }
    if (packet->payload.data() < input.data() || packet->payload.data() + packet->payload.size() > input.data() + input.size()) {
        std::abort();
    }
    (void)packet->header.payload_type();
    return 0;
}
