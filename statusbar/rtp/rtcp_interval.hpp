#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// The RTCP transmission interval - RFC 3550 Section 6.3.1 and Appendix A.7, without timer
/// reconsideration or member tables (Sections 6.3.3 to 6.3.7): the caller states the members and
/// senders it assumes, and injects the random generator.

#include <algorithm>
#include <cstdint>
#include <numbers>
#include <random>

namespace statusbar::rtp {

/// What the interval is computed from.
struct RtcpIntervalParams
{
    uint64_t minimum_ns{5'000'000'000};  ///< Tmin; Section 6.2 recommends 5 s
    uint32_t members{2};                 ///< participants assumed, this one included
    uint32_t senders{1};                 ///< of which senders
    bool we_sent{false};                 ///< this participant sent data since its last two reports
    double rtcp_bandwidth{0.0};          ///< octets per second for RTCP (5 % of the session's); 0: unbounded
    double average_size{0.0};            ///< average compound packet, octets, UDP and IP headers included
    bool initial{false};                 ///< no report sent yet: Tmin is halved (Section 6.2)
};

/// The participants counted and the RTCP bandwidth they share: when senders are at most a quarter of
/// the members, senders share a quarter of it and receivers the rest (RFC 3550 Section 6.3.1).
struct RtcpBandwidthShare
{
    double participants;  ///< members, or the senders or receivers among them
    double bandwidth;     ///< octets per second
};

/// The share of the RTCP bandwidth this participant's group gets (Section 6.3.1).
[[nodiscard]] constexpr auto rtcp_bandwidth_share(RtcpIntervalParams const& p) noexcept -> RtcpBandwidthShare
{
    if (p.senders > p.members / 4.0) {
        return {.participants = static_cast<double>(p.members), .bandwidth = p.rtcp_bandwidth};
    }
    if (p.we_sent) {
        return {.participants = static_cast<double>(p.senders), .bandwidth = p.rtcp_bandwidth * 0.25};
    }
    return {.participants = static_cast<double>(p.members - p.senders), .bandwidth = p.rtcp_bandwidth * 0.75};
}

/// The deterministic interval Td of Section 6.3.1 in nanoseconds: the larger of Tmin (halved for
/// the first report) and the members' share of the RTCP bandwidth, at most a day.
[[nodiscard]] constexpr auto deterministic_rtcp_interval(RtcpIntervalParams const& p) noexcept -> uint64_t
{
    if (p.rtcp_bandwidth <= 0.0 || p.members == 0) {
        return p.initial ? p.minimum_ns / 2 : p.minimum_ns;
    }
    double const minimum = p.initial ? static_cast<double>(p.minimum_ns) / 2.0 : static_cast<double>(p.minimum_ns);
    auto const share = rtcp_bandwidth_share(p);
    double const interval = share.participants * p.average_size / share.bandwidth * 1e9;
    // Clamped to a day, so the conversion is defined for any inputs.
    constexpr double DAY_NS = 86'400e9;
    return static_cast<uint64_t>(std::min(std::max(minimum, interval), DAY_NS));
}

/// Td scaled by a uniform draw in [0.5, 1.5) and divided by e - 3/2 (Section 6.3.1), in ns.
template <std::uniform_random_bit_generator Rng>
[[nodiscard]] constexpr auto rtcp_interval(RtcpIntervalParams const& p, Rng& rng) noexcept -> uint64_t
{
    constexpr double COMPENSATION = std::numbers::e - 1.5;
    constexpr double SPAN = static_cast<double>(Rng::max() - Rng::min()) + 1.0;
    double u = static_cast<double>(rng() - Rng::min()) / SPAN;
    u = u < 1.0 ? u : 0x1.fffffffffffffp-1;  // a 64-bit draw can round up to 1
    return static_cast<uint64_t>(static_cast<double>(deterministic_rtcp_interval(p)) * (u + 0.5) / COMPENSATION);
}

}  // namespace statusbar::rtp
