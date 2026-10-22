// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

#include "statusbar/mediaclock/mediaclock_ptp_clock.hpp"

#include "statusbar/test/test.hpp"

#include <cstdint>

using namespace statusbar::mediaclock;

namespace {

// 2026-10-22 in TAI nanoseconds: index arithmetic must stay exact at today's epoch.
constexpr std::uint64_t NOW_NS = 1'792'627'237'000'000'000ULL;

}  // namespace

TEST(mediaclock_ptp_clock, supports_only_the_48k_family)
{
    EXPECT_TRUE(PtpMediaClock::supports(48000));
    EXPECT_TRUE(PtpMediaClock::supports(96000));
    EXPECT_TRUE(PtpMediaClock::supports(192000));
    EXPECT_FALSE(PtpMediaClock::supports(44100));
    EXPECT_FALSE(PtpMediaClock::supports(0));
    EXPECT_FALSE(PtpMediaClock::supports(6000));   // under a sample per tick
    EXPECT_FALSE(PtpMediaClock::supports(36000));  // 4.5 samples per tick
    EXPECT_EQ(PtpMediaClock::make(48000)->samples_per_tick(), std::uint32_t{6});
    EXPECT_EQ(PtpMediaClock::make(96000)->samples_per_tick(), std::uint32_t{12});
    EXPECT_EQ(PtpMediaClock::make(192000)->samples_per_tick(), std::uint32_t{24});
    // a rate from an SDP that the clock cannot tick: refused, not divided by
    EXPECT_FALSE(PtpMediaClock::make(44100).has_value());
    EXPECT_FALSE(PtpMediaClock::make(0).has_value());
}

TEST(mediaclock_ptp_clock, index_is_ptp_time_times_rate)
{
    auto const clock = PtpMediaClock::make(48000).value();
    EXPECT_EQ(clock.index_at(0), std::uint64_t{0});
    EXPECT_EQ(clock.index_at(20'833), std::uint64_t{0});  // sample 1 is at 20833.33 ns
    EXPECT_EQ(clock.index_at(20'834), std::uint64_t{1});
    EXPECT_EQ(clock.index_at(1'000'000'000), std::uint64_t{48000});
    EXPECT_EQ(clock.index_at(NOW_NS), (NOW_NS / 1'000'000'000ULL) * 48000ULL);
}

TEST(mediaclock_ptp_clock, time_of_inverts_index_at)
{
    for (std::uint32_t const fs : {48000U, 96000U, 192000U}) {
        auto const clock = PtpMediaClock::make(fs).value();
        std::uint64_t const base = clock.index_at(NOW_NS);
        for (std::uint64_t n = base; n < base + (3 * fs); n += 7) {
            std::uint64_t const t = clock.time_of(n);
            EXPECT_EQ(clock.index_at(t), n);
            EXPECT_EQ(clock.index_at(t - 1), n - 1);  // time_of is the first ns of sample n
        }
    }
}

TEST(mediaclock_ptp_clock, rtp_timestamp_is_the_low_32_bits)
{
    auto const clock = PtpMediaClock::make(48000).value();
    std::uint64_t const n = clock.index_at(NOW_NS);
    auto const rtp = static_cast<std::uint32_t>(n);
    EXPECT_EQ(PtpMediaClock::unwrap(rtp, n), n);
    EXPECT_EQ(PtpMediaClock::unwrap(rtp, n + 1000), n);  // reference ahead
    EXPECT_EQ(PtpMediaClock::unwrap(rtp, n - 1000), n);  // reference behind
    EXPECT_EQ(PtpMediaClock::unwrap(rtp + 5, n), n + 5);
    // Across a 32-bit wrap.
    std::uint64_t const wrap = (n | 0xffff'ffffULL) + 1;
    EXPECT_EQ(PtpMediaClock::unwrap(static_cast<std::uint32_t>(wrap + 3), wrap - 3), wrap + 3);
    EXPECT_EQ(PtpMediaClock::unwrap(static_cast<std::uint32_t>(wrap - 3), wrap + 3), wrap - 3);
}

TEST(mediaclock_ptp_clock, rtp_timestamps_carry_the_direct_offset)
{
    // a Dante stream: a=mediaclk:direct=1266592257
    constexpr std::uint32_t OFFSET = 1266592257;
    auto const clock = PtpMediaClock::make(48000).value();
    std::uint64_t const n = clock.index_at(NOW_NS);
    std::uint32_t const rtp = PtpMediaClock::rtp_timestamp(n, OFFSET);
    EXPECT_EQ(rtp, static_cast<std::uint32_t>(n) + OFFSET);
    EXPECT_EQ(PtpMediaClock::index_of_rtp(rtp, OFFSET, n), n);
    EXPECT_EQ(PtpMediaClock::index_of_rtp(rtp, OFFSET, n + 1000), n);
    EXPECT_EQ(PtpMediaClock::index_of_rtp(rtp, OFFSET, n - 1000), n);
    EXPECT_EQ(PtpMediaClock::index_of_rtp(rtp + 6, OFFSET, n), n + 6);  // the next 125 us packet
    EXPECT_EQ(PtpMediaClock::rtp_timestamp(n, 0), static_cast<std::uint32_t>(n));
    // the same sample on the AVB side: its presentation time, mod 2^32
    EXPECT_EQ(clock.avtp_timestamp(n), static_cast<std::uint32_t>(clock.time_of(n)));
    EXPECT_EQ(clock.avtp_timestamp(n + 6) - clock.avtp_timestamp(n), std::uint32_t{125'000});
}

TEST_MAIN(statusbar_mediaclock, mediaclock_ptp_clock_test)
