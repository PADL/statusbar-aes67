// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

// The PTP-locked media clock: sample index n is presented at PTP time n / fs.
//
// This is the AES67 media clock of RFC 7273 `a=mediaclk:direct=<offset>`: the
// media clock is the PTP clock itself, so a sample's index is a pure function
// of PTP time and every device on the same grandmaster agrees on it without
// exchanging anything. An RTP timestamp is the low 32 bits of the index plus
// the stream's direct offset (subtract it before unwrap()); an AVTP
// presentation time is time_of(index) (mod 2^32 on the wire). A bridge maps a
// sample from one wire to the other with this arithmetic alone -- no rate
// estimation, no anchoring, no state.
//
// Exact integer arithmetic, no 128-bit types: a time is split into whole
// seconds and nanoseconds, and only the sub-second part is scaled, so the
// largest intermediate is 1e9 * fs (< 2^48 at 192 kHz). Only the 48 kHz family
// is supported (fs a multiple of 24 kHz), which keeps every 125 µs tick a whole
// number of samples.

#pragma once

#include <cstdint>

namespace statusbar::mediaclock {

class PtpMediaClock
{
  public:
    static constexpr std::uint64_t NS_PER_SECOND = 1'000'000'000;

    /// fs must pass supports() (48, 96 or 192 kHz in practice).
    explicit constexpr PtpMediaClock(std::uint32_t sample_rate_hz) noexcept
        : fs_{sample_rate_hz}
    {}

    [[nodiscard]] static constexpr auto supports(std::uint32_t sample_rate_hz) noexcept -> bool
    {
        return sample_rate_hz != 0 && sample_rate_hz % 24000 == 0;
    }

    [[nodiscard]] constexpr auto sample_rate_hz() const noexcept -> std::uint32_t { return fs_; }

    /// Samples per 125 µs (AVB class A) tick: 6, 12 or 24.
    [[nodiscard]] constexpr auto samples_per_tick() const noexcept -> std::uint32_t { return fs_ / 8000; }

    /// The latest sample presented at or before PTP time t: floor(t * fs / 1e9).
    [[nodiscard]] constexpr auto index_at(std::uint64_t ptp_ns) const noexcept -> std::uint64_t
    {
        std::uint64_t const seconds = ptp_ns / NS_PER_SECOND;
        std::uint64_t const ns = ptp_ns % NS_PER_SECOND;
        return (seconds * fs_) + ((ns * fs_) / NS_PER_SECOND);
    }

    /// The PTP time sample n is presented, rounded up to the nanosecond:
    /// ceil(n * 1e9 / fs). index_at(time_of(n)) == n for every n.
    [[nodiscard]] constexpr auto time_of(std::uint64_t index) const noexcept -> std::uint64_t
    {
        std::uint64_t const seconds = index / fs_;
        std::uint64_t const samples = index % fs_;
        return (seconds * NS_PER_SECOND) + (((samples * NS_PER_SECOND) + fs_ - 1) / fs_);
    }

    /// The full index of a 32-bit wire timestamp (RTP), taken as the nearest
    /// index to `reference` with those low 32 bits. Unambiguous within +/- 2^31
    /// samples of the reference (about 12 hours at 48 kHz).
    [[nodiscard]] static constexpr auto unwrap(std::uint32_t timestamp, std::uint64_t reference) noexcept -> std::uint64_t
    {
        auto const delta = static_cast<std::int32_t>(timestamp - static_cast<std::uint32_t>(reference));
        return reference + static_cast<std::uint64_t>(static_cast<std::int64_t>(delta));
    }

  private:
    std::uint32_t fs_;
};

}  // namespace statusbar::mediaclock
