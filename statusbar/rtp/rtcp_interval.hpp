#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// The RTCP transmission interval - RFC 3550 Section 6.3.1 and Appendix A.7, without timer
/// reconsideration or member tables (Sections 6.3.3 to 6.3.7): the caller states the members and
/// senders it assumes, and injects the random generator.

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

/// The deterministic interval Td of Section 6.3.1 in nanoseconds: the larger of Tmin (halved for
/// the first report) and the members' share of the RTCP bandwidth, at most a day.
[[nodiscard]] constexpr auto deterministic_rtcp_interval(RtcpIntervalParams const& p) noexcept -> uint64_t
{
    if (p.rtcp_bandwidth <= 0.0 || p.members == 0) {
        return p.initial ? p.minimum_ns / 2 : p.minimum_ns;
    }
    double const minimum = p.initial ? static_cast<double>(p.minimum_ns) / 2.0 : static_cast<double>(p.minimum_ns);
    // Senders get a quarter of the bandwidth when they are at most a quarter of the members.
    auto n = static_cast<double>(p.members);
    double bandwidth = p.rtcp_bandwidth;
    if (p.senders <= p.members / 4.0) {
        if (p.we_sent) {
            bandwidth *= 0.25;
            n = static_cast<double>(p.senders);
        } else {
            bandwidth *= 0.75;
            n = static_cast<double>(p.members - p.senders);
        }
    }
    // Clamped to a day, so the conversion is defined for any inputs.
    constexpr double DAY_NS = 86'400e9;
    double const share = n * p.average_size / bandwidth * 1e9;
    double const td = share > minimum ? share : minimum;
    return static_cast<uint64_t>(td < DAY_NS ? td : DAY_NS);
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
