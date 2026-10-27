// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// libFuzzer harness for sdp::parse: it may not crash or read out of bounds, and every pair of a media's
/// reference clocks in force (RFC 7273 Section 4.8) compares consistently.

#include "statusbar/sdp/sdp.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string_view>

using namespace statusbar;

namespace {

/// same_clock() is symmetric, and an IEEE 802.1AS reference matches as a PTPv2 one in domain 0 would.
void check_pair(sdp::PtpReference const& a, sdp::PtpReference const& b)
{
    if (a.same_clock(b) != b.same_clock(a)) {
        std::abort();
    }
    if (a.version == sdp::PtpVersion::ieee802_1as && a.grandmaster) {
        auto const as_1588 = sdp::PtpReference::naming(*a.grandmaster, 0);
        if (a.same_clock(b) != as_1588.same_clock(b) && b.version != sdp::PtpVersion::ieee1588_2002) {
            std::abort();
        }
    }
}

/// check_pair() over every pair of `references`.
void check_references(std::span<sdp::PtpReference const> const references)
{
    for (auto const& a : references) {
        for (auto const& b : references) {
            check_pair(a, b);
        }
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    std::string_view const text{
        reinterpret_cast<char const*>(data), size};  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): bytes as text
    auto const session = sdp::parse(text);
    if (!session) {
        return 0;
    }
    for (auto const& media : session->media) {
        check_references(session->effective_clocks(media));
    }
    return 0;
}
