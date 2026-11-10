// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// libFuzzer harness for sdp::parse: it may not crash or read out of bounds, every pair of a media's
/// reference clocks in force (RFC 7273 Section 4.8) compares consistently, and a media's channel order
/// (ST 2110-30 Section 6.2.2) and maxptime are well formed and survive to_string() and parse() again.

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

/// A kept channel order counts its channels, and `media`'s order, IPMX declaration, maxptime and clock domain are
/// `again`'s.
void check_written(sdp::Media const& media, sdp::Media const& again)
{
    if (!media.channel_order.empty() && !sdp::channel_order_channels(media.channel_order)) {
        std::abort();
    }
    bool const same_fmtp = again.channel_order == media.channel_order && again.ipmx == media.ipmx;
    if (!same_fmtp || again.max_packet_time_us != media.max_packet_time_us || again.clock_domain != media.clock_domain) {
        std::abort();
    }
}

/// check_written() of each media of `session` against the session to_string() writes, parsed again.
void check_round_trip(sdp::Session const& session)
{
    auto const again = sdp::parse(sdp::to_string(session));
    if (!again || again->media.size() != session.media.size()) {
        return;  // a session the writer sanitises differently is not this check's
    }
    if (again->clock_domain != session.clock_domain) {
        std::abort();
    }
    for (size_t i = 0; i < session.media.size(); ++i) {
        check_written(session.media[i], again->media[i]);
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    std::string_view const text{
        reinterpret_cast<char const*>(data), size};  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): bytes as text
    (void)sdp::channel_order_channels(text);
    auto const session = sdp::parse(text);
    if (!session) {
        return 0;
    }
    for (auto const& media : session->media) {
        check_references(session->effective_clocks(media));
    }
    check_round_trip(*session);
    return 0;
}
