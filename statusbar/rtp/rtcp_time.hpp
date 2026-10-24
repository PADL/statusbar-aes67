#pragma once

// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// RTCP time arithmetic on the PTP timescale: the PTP truncated timestamp an IPMX SR carries
/// (TR-10-1 Section 8.7), the LSR an RR echoes and the DLSR it adds (RFC 3550 Section 6.4.1), and
/// the round trip they give. Times are nanoseconds since the PTP epoch (1970, TAI). No NTP form.

#include <cstdint>
#include <optional>

namespace statusbar::rtp {

inline constexpr uint64_t NS_PER_SECOND = 1'000'000'000;

/// The low 64 bits of a PTP timestamp: seconds mod 2^32 and nanoseconds - TR-10-1 Figure 2
struct PtpTruncated
{
    uint32_t seconds{0};
    uint32_t nanoseconds{0};  ///< below NS_PER_SECOND

    auto operator==(PtpTruncated const&) const noexcept -> bool = default;
};

[[nodiscard]] constexpr auto ptp_truncated_from_ns(uint64_t const ptp_ns) noexcept -> PtpTruncated
{
    return {
        .seconds = static_cast<uint32_t>(ptp_ns / NS_PER_SECOND),
        .nanoseconds = static_cast<uint32_t>(ptp_ns % NS_PER_SECOND),
    };
}

/// The PTP time `truncated` names within 2^31 s (68 years) of `reference_ptp_ns`, the 2^32 s wrap
/// included; nullopt when the nanoseconds are out of range or the time would precede the epoch.
[[nodiscard]] constexpr auto ns_from_ptp_truncated(PtpTruncated const truncated, uint64_t const reference_ptp_ns) noexcept
    -> std::optional<uint64_t>
{
    if (truncated.nanoseconds >= NS_PER_SECOND) {
        return std::nullopt;
    }
    int64_t const reference_s = static_cast<int64_t>(reference_ptp_ns / NS_PER_SECOND);
    auto const delta = static_cast<int32_t>(truncated.seconds - static_cast<uint32_t>(reference_s));
    int64_t const seconds = reference_s + delta;
    if (seconds < 0) {
        return std::nullopt;
    }
    return (static_cast<uint64_t>(seconds) * NS_PER_SECOND) + truncated.nanoseconds;
}

/// The middle 32 bits of an SR's 64-bit time field, as an RR's LSR echoes them, whatever the field
/// holds (RFC 3550 Section 6.4.1).
[[nodiscard]] constexpr auto lsr_of(uint32_t const msw, uint32_t const lsw) noexcept -> uint32_t
{
    return (msw << 16U) | (lsw >> 16U);
}

/// A delay in RFC 3550's 1/65536 s units (a DLSR), saturating at 2^32 - 1.
[[nodiscard]] constexpr auto dlsr_from_ns(uint64_t const ns) noexcept -> uint32_t
{
    uint64_t const max_ns = (uint64_t{0xFFFF'FFFF} * NS_PER_SECOND) >> 16U;
    return ns >= max_ns ? 0xFFFF'FFFFU : static_cast<uint32_t>((ns << 16U) / NS_PER_SECOND);
}

/// A DLSR in nanoseconds.
[[nodiscard]] constexpr auto ns_from_dlsr(uint32_t const dlsr) noexcept -> uint64_t
{
    return (uint64_t{dlsr} * NS_PER_SECOND) >> 16U;
}

/// The round trip a sender measures from an RR (RFC 3550 Section 6.4.1, Figure 2): arrival less the
/// SR's sending time (both PTP ns) less the DLSR; nullopt when negative (a stale or foreign LSR).
[[nodiscard]] constexpr auto rtt_ns(uint64_t const arrival_ns, uint64_t const sr_sent_ns, uint32_t const dlsr) noexcept
    -> std::optional<uint64_t>
{
    uint64_t const held = ns_from_dlsr(dlsr);
    if (arrival_ns < sr_sent_ns || arrival_ns - sr_sent_ns < held) {
        return std::nullopt;
    }
    return arrival_ns - sr_sent_ns - held;
}

}  // namespace statusbar::rtp
