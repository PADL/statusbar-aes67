// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// libFuzzer harness for sdp::parse: it may not crash or read out of bounds, and every media's
/// reference clocks in force (RFC 7273 Section 4.8) are compared with one another.

#include "statusbar/sdp/sdp.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

using namespace statusbar;

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    std::string_view const text{
        reinterpret_cast<char const*>(data), size};  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): bytes as text
    auto const session = sdp::parse(text);
    if (!session) {
        return 0;
    }
    for (auto const& media : session->media) {
        for (auto const& reference : session->effective_clocks(media)) {
            bool const known = reference.version != sdp::PtpVersion::other;
            if (known && reference.grandmaster && !session->names_clock(media, reference)) {
                std::abort();  // a reference naming a grandmaster in a known version names its own clock
            }
        }
    }
    return 0;
}
