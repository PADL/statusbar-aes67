// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// libFuzzer harness for rtp::parse_udp_frame and rtp::parse_rtp_frame, the AF_XDP receive path's parsers
/// (IPv4 options and 802.1Q tags included). Neither may crash or read out of bounds; the views must lie inside the input.

#include "padl/rtp/rtp_frame.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

using namespace statusbar;

namespace {

void require_inside(std::span<uint8_t const> const input, std::span<uint8_t const> const view)
{
    if (!view.empty() && (view.data() < input.data() || view.data() + view.size() > input.data() + input.size())) {
        std::abort();
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    std::span<uint8_t const> const input{data, size};
    if (auto const udp = padl::rtp::parse_udp_frame(input)) {
        require_inside(input, udp->datagram);
    }
    if (auto const frame = padl::rtp::parse_rtp_frame(input)) {
        require_inside(input, frame->datagram);
        require_inside(input, frame->packet.payload);
    }
    return 0;
}
